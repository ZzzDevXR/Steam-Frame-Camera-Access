#include "shared_ring.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <climits>

#include "fca.h"
#include "util.h"

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

namespace fca {
namespace {

uint64_t align_up(uint64_t bytes) { return (bytes + FCA_DATA_ALIGN - 1) / FCA_DATA_ALIGN * FCA_DATA_ALIGN; }

void copy_name(char (&out)[FCA_NAME_BYTES], const std::string& name) {
  std::memset(out, 0, sizeof out);
  std::memcpy(out, name.data(), std::min(name.size(), sizeof out - 1));
}

template <typename T>
void store_release(T* target, T value) {
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

}  // namespace

SharedRing::SharedRing(const std::vector<CameraLayout>& cameras, uint32_t slots, const std::string& source) {
  if (cameras.empty() || cameras.size() > FCA_MAX_CAMERAS) throw std::runtime_error("unsupported camera count");
  if (slots < 2 || slots > FCA_MAX_SLOTS) throw std::runtime_error("slot count must be 2-8");
  uint64_t slot_bytes = 0;
  for (const CameraLayout& camera : cameras) slot_bytes += align_up(uint64_t(camera.width) * camera.height);
  const uint64_t slot_stride = align_up(slot_bytes);
  size_ = FCA_DATA_ALIGN + slot_stride * slots;

  fd_ = memfd_create("frame-camera-access", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd_ < 0) throw system_error("memfd_create");
  if (ftruncate(fd_, off_t(size_)) != 0) throw system_error("ftruncate");
  if (fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) throw system_error("F_ADD_SEALS");
  void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping == MAP_FAILED) throw system_error("mmap ring");
  header_ = static_cast<fca_shm_header*>(mapping);
  const std::string own_fd_path = "/proc/self/fd/" + std::to_string(fd_);
  readonly_fd_ = open(own_fd_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (readonly_fd_ < 0) throw system_error("reopen ring read-only");
  if (fcntl(fd_, F_ADD_SEALS, F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0)
    throw system_error("F_ADD_SEALS F_SEAL_FUTURE_WRITE (needs Linux 5.1)");
  if (fchmod(fd_, 0444) != 0) log("ring: cannot make the memory file read-only (%s)", std::strerror(errno));

  fca_shm_header& header = *header_;
  header.magic = FCA_SHM_MAGIC;
  header.version = FCA_PROTOCOL_VERSION;
  header.header_size = sizeof(fca_shm_header);
  header.camera_count = uint32_t(cameras.size());
  header.slot_count = slots;
  header.state = FCA_STATE_STARTING;
  header.total_size = size_;
  header.data_offset = FCA_DATA_ALIGN;
  header.slot_stride = slot_stride;
  header.daemon_pid = uint32_t(getpid());
  header.daemon_start_ns = now_raw_ns();
  header.heartbeat_ns = header.daemon_start_ns;
  copy_name(header.source, source);
  uint64_t offset = 0;
  for (size_t i = 0; i < cameras.size(); ++i) {
    fca_shm_camera& camera = header.cameras[i];
    copy_name(camera.name, cameras[i].name);
    copy_name(camera.sensor, cameras[i].sensor);
    camera.width = cameras[i].width;
    camera.height = cameras[i].height;
    camera.stride = cameras[i].width;
    camera.pixel_format = FCA_PIXEL_FORMAT_GRAY8;
    camera.image_offset = offset;
    camera.image_size = uint64_t(camera.stride) * camera.height;
    offset += align_up(camera.image_size);
  }
}

SharedRing::~SharedRing() {
  if (header_) munmap(header_, size_);
  if (readonly_fd_ >= 0) close(readonly_fd_);
  if (fd_ >= 0) close(fd_);
}

uint64_t SharedRing::begin() {
  if (writing_) abort();
  pending_set_ = published_ + 1;
  pending_index_ = uint32_t(pending_set_ % header_->slot_count);
  fca_shm_slot& pending_slot = header_->slots[pending_index_];
  __atomic_store_n(&pending_slot.seq, pending_slot.seq + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  pending_slot.set_index = 0;
  writing_ = true;
  return pending_set_;
}

uint8_t* SharedRing::image(size_t camera) {
  return reinterpret_cast<uint8_t*>(header_) + header_->data_offset + uint64_t(pending_index_) * header_->slot_stride +
         header_->cameras[camera].image_offset;
}

fca_shm_slot& SharedRing::slot() { return header_->slots[pending_index_]; }

void SharedRing::publish() {
  if (!writing_) return;
  fca_shm_slot& pending_slot = header_->slots[pending_index_];
  pending_slot.set_index = pending_set_;
  store_release(&pending_slot.seq, pending_slot.seq + 1);
  writing_ = false;
  published_ = pending_set_;
  store_release(&header_->latest_set, published_);
  __atomic_fetch_add(&header_->sets_published, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&header_->frame_counter, 1, __ATOMIC_RELEASE);
  syscall(SYS_futex, &header_->frame_counter, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void SharedRing::abort() {
  if (!writing_) return;
  fca_shm_slot& pending_slot = header_->slots[pending_index_];
  pending_slot.set_index = 0;
  store_release(&pending_slot.seq, pending_slot.seq + 1);
  writing_ = false;
}

void SharedRing::set_state(uint32_t state, const std::string& status) {
  fca_shm_header& header = *header_;
  __atomic_store_n(&header.status_seq, header.status_seq + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  std::memset(header.status, 0, sizeof header.status);
  std::memcpy(header.status, status.data(), std::min(status.size(), sizeof header.status - 1));
  store_release(&header.status_seq, header.status_seq + 1);
  store_release(&header.state, state);
}

uint32_t SharedRing::state() const { return __atomic_load_n(&header_->state, __ATOMIC_ACQUIRE); }

void SharedRing::heartbeat() { store_release(&header_->heartbeat_ns, now_raw_ns()); }

void SharedRing::set_counters(uint64_t copy_races, uint64_t invalid_metadata, uint64_t dark_skipped) {
  __atomic_store_n(&header_->copy_races, copy_races, __ATOMIC_RELAXED);
  __atomic_store_n(&header_->invalid_metadata, invalid_metadata, __ATOMIC_RELAXED);
  __atomic_store_n(&header_->dark_skipped, dark_skipped, __ATOMIC_RELAXED);
}

}  // namespace fca
