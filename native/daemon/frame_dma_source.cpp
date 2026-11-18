#include "frame_dma_source.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <regex>

#include "fca.h"
#include "util.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

namespace fca {
namespace {

struct Role {
  const char *name, *sensor;
  uint32_t width, height;
};
constexpr Role kRoles[4] = {{"slam_right", "og01a1b 4-0036", 1056, 1024},
                            {"slam_left", "og01a1b 4-0060", 1056, 1024},
                            {"upper_left", "og0ve10 5-0060", 640, 480},
                            {"upper_right", "og0ve10 5-003e", 640, 480}};

constexpr size_t kMetaPrefix = 1560;
constexpr size_t kMetaBytes = 8192;
constexpr size_t kSofOffset = 256;
constexpr size_t kIspOffset = 288;
constexpr size_t kExtOffset = 1536;
constexpr int64_t kMaxSkewNs = 200'000;
constexpr int64_t kMaxFrameNs = 100'000'000;
constexpr int64_t kMaxAgeNs = 500'000'000;
constexpr int64_t kRevalidateNs = 5'000'000'000;
constexpr int64_t kNewerFrameNs = 25'000'000;

uint64_t read_u64(const uint8_t* bytes, size_t offset) {
  uint64_t value;
  std::memcpy(&value, bytes + offset, sizeof value);
  return value;
}
int64_t read_i64(const uint8_t* bytes, size_t offset) {
  int64_t value;
  std::memcpy(&value, bytes + offset, sizeof value);
  return value;
}
uint32_t read_u32(const uint8_t* bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes + offset, sizeof value);
  return value;
}

std::string read_link(const std::string& path) {
  char buffer[4096];
  const ssize_t length = readlink(path.c_str(), buffer, sizeof buffer - 1);
  if (length < 0) return {};
  return std::string(buffer, size_t(length));
}

std::string basename_of(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string process_start_ticks(pid_t pid) {
  std::string stat;
  if (!read_text("/proc/" + std::to_string(pid) + "/stat", stat)) return {};
  const auto paren = stat.rfind(')');
  if (paren == std::string::npos) return {};
  std::istringstream fields(stat.substr(paren + 1));
  std::string field;
  for (int i = 0; i <= 19 && fields >> field; ++i)
    if (i == 19) return field;
  return {};
}

double boot_time_epoch() {
  std::string stat;
  if (read_text("/proc/stat", stat)) {
    std::istringstream lines(stat);
    std::string line;
    while (std::getline(lines, line))
      if (line.rfind("btime ", 0) == 0) return std::stod(line.substr(6));
  }
  throw std::runtime_error("cannot read btime from /proc/stat");
}

bool parse_log_time(const std::string& text, double& epoch) {
  tm calendar_time{};
  const char* end = strptime(text.c_str(), "%a %b %d %Y %H:%M:%S", &calendar_time);
  if (!end || *end != '.' || !std::isdigit(static_cast<unsigned char>(end[1]))) return false;
  double fraction = 0, scale = 0.1;
  int digits = 0;
  for (++end; std::isdigit(static_cast<unsigned char>(*end)); ++end, ++digits, scale /= 10)
    fraction += (*end - '0') * scale;
  if (*end != '\0' || digits > 6) return false;
  calendar_time.tm_isdst = -1;
  const time_t seconds = mktime(&calendar_time);
  if (seconds == time_t(-1)) return false;
  epoch = double(seconds) + fraction;
  return true;
}

struct Binding {
  std::string video, subdev;
};

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) ::close(fd_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

template <typename LineHandler>
bool for_each_line(const std::string& path, LineHandler handle_line) {
  constexpr size_t kMaxLineBytes = 4096;
  const FileDescriptor file(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
  struct stat file_status{};
  if (file.get() < 0 || fstat(file.get(), &file_status) != 0 || !S_ISREG(file_status.st_mode)) return false;
  std::string line;
  bool overlong = false;
  char chunk[65536];
  for (;;) {
    const ssize_t count = ::read(file.get(), chunk, sizeof chunk);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return false;
    if (count == 0) break;
    for (ssize_t at = 0; at < count; ++at) {
      if (chunk[at] == '\n') {
        if (!overlong) handle_line(line);
        line.clear();
        overlong = false;
      } else if (line.size() < kMaxLineBytes) {
        line += chunk[at];
      } else {
        overlong = true;
      }
    }
  }
  if (!overlong && !line.empty()) handle_line(line);
  return true;
}

std::map<int, Binding> runtime_bindings(const std::string& log_path, double producer_start_epoch) {
  static const std::regex line_re(
      R"(^(.*?) INFO: TrackingCameraInit: index: (\d+)\. video device: (/dev/video\d+)\. v4l subdevice: (/dev/v4l-subdev\d+))");
  std::map<int, Binding> runtime;
  const bool log_read = for_each_line(log_path, [&](std::string line) {
    if (line.find("TrackingCameraInit: index:") == std::string::npos) return;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    double stamp = 0;
    if (!std::regex_search(line, match, line_re) || !parse_log_time(match[1].str(), stamp)) {
      runtime.clear();
      return;
    }
    if (stamp < producer_start_epoch - 1) return;
    const int index = std::stoi(match[2].str());
    if (index == 0) runtime.clear();
    if (index != int(runtime.size())) {
      runtime.clear();
      return;
    }
    runtime[index] = {match[3].str(), match[4].str()};
  });
  if (!log_read) throw std::runtime_error("cannot read the XRService log '" + log_path + "' (see --xrservice-log)");
  if (runtime.size() != 4)
    throw std::runtime_error("no complete TrackingCameraInit block from the running XRService in its log");
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (runtime[i].video == runtime[j].video || runtime[i].subdev == runtime[j].subdev)
        throw std::runtime_error("XRService log assigns one video node or subdevice twice");
  return runtime;
}

bool has_system_executable(pid_t pid) {
  struct stat exe_status{};
  return stat(("/proc/" + std::to_string(pid) + "/exe").c_str(), &exe_status) == 0 && exe_status.st_uid == 0 &&
         (exe_status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

}  // namespace

std::vector<CameraLayout> FrameDmaSource::layout() const {
  std::vector<CameraLayout> cameras;
  for (const Role& role : kRoles) cameras.push_back({role.name, role.sensor, role.width, role.height});
  return cameras;
}

FrameDmaSource::Identity FrameDmaSource::find_xrservice() {
  std::vector<pid_t> matches;
  DIR* proc_dir = opendir("/proc");
  if (!proc_dir) throw system_error("opendir /proc");
  while (const dirent* entry = readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char character) {
          return std::isdigit(static_cast<unsigned char>(character));
        }))
      continue;
    if (basename_of(read_link("/proc/" + name + "/exe")) == "XRService") matches.push_back(pid_t(std::stol(name)));
  }
  closedir(proc_dir);
  if (matches.size() > 1) {
    std::vector<pid_t> installed;
    std::copy_if(matches.begin(), matches.end(), std::back_inserter(installed), has_system_executable);
    if (installed.size() == 1) matches = installed;
  }
  if (matches.empty()) throw std::runtime_error("XRService is not running (is the headset on and SteamVR started?)");
  if (matches.size() != 1) throw std::runtime_error("more than one XRService process is running");
  Identity identity;
  identity.pid = matches[0];
  const std::string process_dir = "/proc/" + std::to_string(identity.pid);
  identity.start_ticks = process_start_ticks(identity.pid);
  identity.exe = read_link(process_dir + "/exe");
  identity.mount_ns = read_link(process_dir + "/ns/mnt");
  if (identity.start_ticks.empty() || identity.exe.empty() || identity.mount_ns.empty())
    throw std::runtime_error("cannot read XRService's process identity (the daemon needs CAP_SYS_PTRACE)");
  return identity;
}

std::array<FrameDmaSource::Node, 4> FrameDmaSource::discover(const Identity& producer) const {
  const double start_epoch = boot_time_epoch() + std::stod(producer.start_ticks) / double(sysconf(_SC_CLK_TCK));
  const std::map<int, Binding> runtime = runtime_bindings(options_.xrservice_log, start_epoch);

  std::array<Node, 4> nodes{};
  for (int i = 0; i < 4; ++i) {
    const Role& role = kRoles[i];
    const Binding& binding = runtime.at(i);
    std::string sensor;
    read_text("/sys/class/video4linux/" + basename_of(binding.subdev) + "/name", sensor);
    if (trim(sensor) != role.sensor)
      throw std::runtime_error(std::string(role.name) + ": " + binding.subdev + " is '" + trim(sensor) +
                               "', expected sensor '" + role.sensor + "'");
    const FileDescriptor fd(::open(binding.video.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) throw system_error(std::string(role.name) + ": open " + binding.video);
    uint32_t width = 0, height = 0, pixel_format = 0, stride = 0, size_image = 0;
    v4l2_format format_query{};
    format_query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd.get(), VIDIOC_G_FMT, &format_query) == 0) {
      width = format_query.fmt.pix_mp.width;
      height = format_query.fmt.pix_mp.height;
      pixel_format = format_query.fmt.pix_mp.pixelformat;
      stride = format_query.fmt.pix_mp.plane_fmt[0].bytesperline;
      size_image = format_query.fmt.pix_mp.plane_fmt[0].sizeimage;
    } else {
      format_query = v4l2_format{};
      format_query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(fd.get(), VIDIOC_G_FMT, &format_query) != 0)
        throw system_error(std::string(role.name) + ": VIDIOC_G_FMT " + binding.video);
      width = format_query.fmt.pix.width;
      height = format_query.fmt.pix.height;
      pixel_format = format_query.fmt.pix.pixelformat;
      stride = format_query.fmt.pix.bytesperline;
      size_image = format_query.fmt.pix.sizeimage;
    }
    if (pixel_format == V4L2_PIX_FMT_NV12 && size_image != 0) {
      uint32_t found = 0;
      for (uint32_t align : {1u, 64u, 128u, 256u}) {
        const uint32_t candidate_stride = (stride + align - 1) / align * align;
        if (uint64_t(candidate_stride) * height * 3 / 2 == size_image) {
          found = candidate_stride;
          break;
        }
      }
      if (found == 0)
        throw std::runtime_error(std::string(role.name) + ": " + binding.video + " NV12 plane size " +
                                 std::to_string(size_image) + " does not match a luma stride");
      stride = found;
    }
    if ((pixel_format != V4L2_PIX_FMT_GREY && pixel_format != V4L2_PIX_FMT_NV12) || width != role.width ||
        height != role.height || stride < width)
      throw std::runtime_error(std::string(role.name) + ": " + binding.video + " is not a " +
                               std::to_string(role.width) + "x" + std::to_string(role.height) + " GREY or NV12 camera");
    nodes[i] = {binding.video, stride};
  }
  return nodes;
}

std::vector<FrameDmaSource::Buffer> FrameDmaSource::inventory(const std::array<Node, 4>& nodes) const {
  std::vector<Buffer> buffers;
  for (int camera = 0; camera < 4; ++camera) {
    const Node& node = nodes[camera];
    const FileDescriptor fd(::open(node.video.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) throw system_error(std::string(kRoles[camera].name) + ": open " + node.video);
    size_t found = 0;
    for (uint32_t index = 0; index < 64; ++index) {
      v4l2_plane planes[VIDEO_MAX_PLANES]{};
      v4l2_buffer query{};
      query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      query.memory = V4L2_MEMORY_DMABUF;
      query.index = index;
      query.length = VIDEO_MAX_PLANES;
      query.m.planes = planes;
      if (ioctl(fd.get(), VIDIOC_QUERYBUF, &query) != 0) {
        if (index == 0) throw system_error(std::string(kRoles[camera].name) + ": VIDIOC_QUERYBUF " + node.video);
        break;
      }
      const uint64_t image_bytes = uint64_t(node.stride) * kRoles[camera].height;
      if (query.memory != V4L2_MEMORY_DMABUF || query.length != 2 || planes[0].data_offset != 0 ||
          planes[1].data_offset != 0 || planes[0].m.fd < 0 || planes[1].m.fd < 0 || planes[0].length < image_bytes ||
          planes[1].length < kMetaBytes)
        throw std::runtime_error(std::string(kRoles[camera].name) + ": unsupported camera buffer layout");
      Buffer buffer;
      buffer.camera = camera;
      buffer.index = index;
      for (int plane = 0; plane < 2; ++plane) {
        buffer.remote_fd[plane] = planes[plane].m.fd;
        buffer.length[plane] = planes[plane].length;
      }
      buffers.push_back(buffer);
      ++found;
    }
    if (!found) throw std::runtime_error(std::string(kRoles[camera].name) + ": no camera buffers queued");
  }
  return buffers;
}

std::vector<uint64_t> FrameDmaSource::signature(const std::vector<Buffer>& buffers) {
  std::vector<uint64_t> values;
  for (const Buffer& buffer : buffers)
    values.insert(values.end(), {uint64_t(buffer.camera), buffer.index, uint64_t(buffer.remote_fd[0]), buffer.length[0],
                                 uint64_t(buffer.remote_fd[1]), buffer.length[1]});
  return values;
}

void FrameDmaSource::map_buffers() {
  pidfd_ = int(syscall(SYS_pidfd_open, producer_.pid, 0));
  if (pidfd_ < 0) throw system_error("pidfd_open XRService");
  for (Buffer& buffer : buffers_) {
    for (int plane = 0; plane < 2; ++plane) {
      auto existing = mappings_.find(buffer.remote_fd[plane]);
      if (existing == mappings_.end()) {
        const int fd = int(syscall(SYS_pidfd_getfd, pidfd_, buffer.remote_fd[plane], 0));
        if (fd < 0) throw system_error("pidfd_getfd XRService camera buffer (the daemon needs CAP_SYS_PTRACE)");
        Mapping mapping;
        mapping.fd = fd;
        mappings_[buffer.remote_fd[plane]] = mapping;
        if (read_link("/proc/self/fd/" + std::to_string(fd)).find("/dmabuf") == std::string::npos)
          throw std::runtime_error("XRService descriptor is not a DMA buffer (camera layout changed?)");
        std::string fdinfo;
        read_text("/proc/self/fdinfo/" + std::to_string(fd), fdinfo);
        size_t size = 0;
        std::istringstream lines(fdinfo);
        std::string line;
        while (std::getline(lines, line))
          if (line.rfind("size:", 0) == 0) size = size_t(std::stoull(trim(line.substr(5))));
        if (size < buffer.length[plane]) throw std::runtime_error("DMA buffer shorter than the camera plane");
        void* address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (address == MAP_FAILED) throw system_error("mmap camera buffer");
        mappings_[buffer.remote_fd[plane]].addr = address;
        mappings_[buffer.remote_fd[plane]].size = size;
        existing = mappings_.find(buffer.remote_fd[plane]);
      }
      buffer.map[plane] = static_cast<const uint8_t*>(existing->second.addr);
    }
  }
}

void FrameDmaSource::open() {
  close();
  try {
    producer_ = find_xrservice();
    nodes_ = discover(producer_);
    buffers_ = inventory(nodes_);
    map_buffers();
  } catch (...) {
    close();
    throw;
  }
  open_ = true;
  last_sof_ = 0;
  last_check_ns_ = now_raw_ns();
  log("cameras opened: XRService pid %d, %s %s %s %s, %zu buffers", int(producer_.pid), nodes_[0].video.c_str(),
      nodes_[1].video.c_str(), nodes_[2].video.c_str(), nodes_[3].video.c_str(), buffers_.size());
}

void FrameDmaSource::close() {
  for (auto& [remote, mapping] : mappings_) {
    if (mapping.addr) munmap(mapping.addr, mapping.size);
    if (mapping.fd >= 0) ::close(mapping.fd);
  }
  mappings_.clear();
  buffers_.clear();
  if (pidfd_ >= 0) ::close(pidfd_);
  pidfd_ = -1;
  open_ = false;
}

void FrameDmaSource::revalidate() {
  const int64_t now = now_raw_ns();
  if (now - last_check_ns_ < kRevalidateNs) return;
  last_check_ns_ = now;
  if (!(find_xrservice() == producer_)) throw std::runtime_error("XRService restarted; reopening the cameras");
  if (signature(inventory(nodes_)) != signature(buffers_))
    throw std::runtime_error("camera buffers changed; reopening the cameras");
}

bool FrameDmaSource::next(SharedRing& ring, int wait_ms) {
  if (!open_) throw std::runtime_error("camera source is not open");
  revalidate();
  if (try_publish(ring)) return true;
  sleep_ns(int64_t(std::clamp(wait_ms, 0, 2)) * 1'000'000);
  return false;
}

bool FrameDmaSource::try_publish(SharedRing& ring) {
  struct Frame {
    int64_t sof, eof;
    const Buffer* buffer;
    std::array<uint8_t, kMetaPrefix> metadata;
  };
  const int64_t now = now_raw_ns();
  std::array<std::vector<Frame>, 4> frames_by_camera;
  for (const Buffer& buffer : buffers_) {
    Frame frame;
    std::memcpy(frame.metadata.data(), buffer.map[1], kMetaPrefix);
    frame.sof = int64_t(read_u64(frame.metadata.data(), kSofOffset));
    frame.eof = int64_t(read_u64(frame.metadata.data(), kSofOffset + 8));
    frame.buffer = &buffer;
    if (read_u32(frame.metadata.data(), kExtOffset) != 1 || read_u32(frame.metadata.data(), kExtOffset + 4) != 24 ||
        !(0 < frame.eof - frame.sof && frame.eof - frame.sof < kMaxFrameNs)) {
      ++counters_.invalid_metadata;
      continue;
    }
    if (!(0 < now - frame.sof && now - frame.sof < kMaxAgeNs)) continue;
    frames_by_camera[buffer.camera].push_back(frame);
  }
  for (const auto& frames : frames_by_camera)
    if (frames.empty()) return false;

  const bool settle = options_.settle_ms >= 0;
  const int64_t settle_ns = int64_t(options_.settle_ms * 1e6);
  auto newest_sof = [](const std::vector<Frame>& frames) {
    return std::max_element(frames.begin(), frames.end(),
                            [](const Frame& first, const Frame& second) { return first.sof < second.sof; })
        ->sof;
  };
  const int64_t cutoff = std::min(newest_sof(frames_by_camera[0]), newest_sof(frames_by_camera[1])) - kNewerFrameNs;
  std::array<const Frame*, 4> best{};
  for (const Frame& right : frames_by_camera[0]) {
    if (!(last_sof_ < right.sof) || (!settle && right.sof > cutoff)) continue;
    std::array<const Frame*, 4> set{&right};
    for (int camera = 1; camera < 4; ++camera) {
      const std::vector<Frame>& frames = frames_by_camera[camera];
      set[camera] = &*std::min_element(frames.begin(), frames.end(), [&](const Frame& first, const Frame& second) {
        return std::llabs(first.sof - right.sof) < std::llabs(second.sof - right.sof);
      });
    }
    if (settle && std::any_of(set.begin(), set.end(), [&](const Frame* frame) { return now - frame->eof < settle_ns; }))
      continue;
    const auto [earliest, latest] = std::minmax_element(
        set.begin(), set.end(), [](const Frame* first, const Frame* second) { return first->sof < second->sof; });
    if ((*latest)->sof - (*earliest)->sof > kMaxSkewNs) continue;
    if (!best[0] || right.sof > best[0]->sof) best = set;
  }
  if (!best[0]) return false;

  const Buffer& right_buffer = *best[0]->buffer;
  const uint32_t right_width = kRoles[0].width, right_height = kRoles[0].height;
  uint64_t sum = 0, samples = 0;
  for (uint64_t pixel = 0; pixel < uint64_t(right_width) * right_height; pixel += 4099, ++samples)
    sum += right_buffer.map[0][(pixel / right_width) * nodes_[0].stride + pixel % right_width];
  const float mean = float(double(sum) / double(samples));
  const bool dark = mean < options_.min_mean;
  if (dark && options_.bright_only) {
    ++counters_.dark_skipped;
    last_sof_ = best[0]->sof;
    return false;
  }

  ring.begin();
  fca_shm_slot& slot = ring.slot();
  uint8_t metadata_before[kMetaBytes];
  for (int camera = 0; camera < 4; ++camera) {
    const Frame& frame = *best[camera];
    const Buffer& buffer = *frame.buffer;
    const uint32_t width = kRoles[camera].width, height = kRoles[camera].height, stride = nodes_[camera].stride;
    std::memcpy(metadata_before, buffer.map[1], kMetaBytes);
    bool raced = std::memcmp(metadata_before, frame.metadata.data(), kMetaPrefix) != 0;
    uint8_t* destination = ring.image(size_t(camera));
    if (!raced) {
      if (stride == width)
        std::memcpy(destination, buffer.map[0], size_t(width) * height);
      else
        for (uint32_t row = 0; row < height; ++row)
          std::memcpy(destination + size_t(row) * width, buffer.map[0] + size_t(row) * stride, width);
      for (uint32_t row = 0; row < height && !raced; ++row)
        raced = std::memcmp(destination + size_t(row) * width, buffer.map[0] + size_t(row) * stride, width) != 0;
      raced = raced || std::memcmp(metadata_before, buffer.map[1], kMetaBytes) != 0;
    }
    if (raced) {
      ++counters_.copy_races;
      ring.abort();
      return false;
    }
    const uint64_t counter_word = read_u64(metadata_before, 0);
    const double sof_error_ns = double(static_cast<long double>(counter_word >> 8) * 1000.0L) / 19.2 +
                                double(read_i64(metadata_before, kIspOffset)) - double(frame.sof);
    if (std::fabs(sof_error_ns) > 2) {
      ++counters_.invalid_metadata;
      ring.abort();
      return false;
    }
    slot.sof_ns[camera] = frame.sof;
    slot.eof_ns[camera] = frame.eof;
  }
  for (const Frame* frame : best)
    if (std::memcmp(frame->buffer->map[1], frame->metadata.data(), kMetaPrefix) != 0) {
      ++counters_.copy_races;
      ring.abort();
      return false;
    }
  slot.copy_end_ns = now_raw_ns();
  slot.mean_gray = mean;
  slot.flags = dark ? FCA_FRAME_FLAG_DARK : 0;
  ring.publish();
  last_sof_ = best[0]->sof;
  return true;
}

}  // namespace fca
