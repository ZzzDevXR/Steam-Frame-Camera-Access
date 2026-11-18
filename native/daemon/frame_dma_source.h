#pragma once

#include <sys/types.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "frame_source.h"

namespace fca {

class FrameDmaSource : public FrameSource {
 public:
  struct Options {
    double settle_ms = 2;
    bool bright_only = true;
    double min_mean = 10;
    std::string xrservice_log;
  };
  explicit FrameDmaSource(Options options) : options_(std::move(options)) {}
  ~FrameDmaSource() override { close(); }

  const char* kind() const override { return "frame-dma"; }
  std::vector<CameraLayout> layout() const override;
  void open() override;
  bool next(SharedRing& ring, int wait_ms) override;
  void close() override;
  SourceCounters counters() const override { return counters_; }

 private:
  struct Identity {
    pid_t pid = 0;
    std::string start_ticks, exe, mount_ns;
    bool operator==(const Identity& other) const {
      return pid == other.pid && start_ticks == other.start_ticks && exe == other.exe && mount_ns == other.mount_ns;
    }
  };
  struct Node {
    std::string video;
    uint32_t stride = 0;
  };
  struct Buffer {
    int camera = 0;
    uint32_t index = 0;
    int remote_fd[2] = {-1, -1};
    uint32_t length[2] = {0, 0};
    const uint8_t* map[2] = {nullptr, nullptr};
  };
  struct Mapping {
    int fd = -1;
    void* addr = nullptr;
    size_t size = 0;
  };

  static Identity find_xrservice();
  std::array<Node, 4> discover(const Identity& producer) const;
  std::vector<Buffer> inventory(const std::array<Node, 4>& nodes) const;
  static std::vector<uint64_t> signature(const std::vector<Buffer>& buffers);
  void map_buffers();
  void revalidate();
  bool try_publish(SharedRing& ring);

  Options options_;
  Identity producer_;
  std::array<Node, 4> nodes_{};
  std::vector<Buffer> buffers_;
  std::map<int, Mapping> mappings_;
  int pidfd_ = -1;
  bool open_ = false;
  int64_t last_sof_ = 0, last_check_ns_ = 0;
  SourceCounters counters_;
};

}  // namespace fca
