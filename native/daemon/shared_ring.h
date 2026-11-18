#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fca_protocol.h"

namespace fca {

struct CameraLayout {
  std::string name;
  std::string sensor;
  uint32_t width = 0, height = 0;
};

class SharedRing {
 public:
  SharedRing(const std::vector<CameraLayout>& cameras, uint32_t slots, const std::string& source);
  ~SharedRing();
  SharedRing(const SharedRing&) = delete;
  SharedRing& operator=(const SharedRing&) = delete;

  int readonly_fd() const { return readonly_fd_; }
  uint64_t size() const { return size_; }
  size_t camera_count() const { return header_->camera_count; }
  const fca_shm_camera& camera(size_t i) const { return header_->cameras[i]; }

  uint64_t begin();
  bool writing() const { return writing_; }
  uint8_t* image(size_t camera);
  fca_shm_slot& slot();
  void publish();
  void abort();

  void set_state(uint32_t state, const std::string& status);
  uint32_t state() const;
  void heartbeat();
  void set_counters(uint64_t copy_races, uint64_t invalid_metadata, uint64_t dark_skipped);

 private:
  int fd_ = -1, readonly_fd_ = -1;
  uint64_t size_ = 0;
  fca_shm_header* header_ = nullptr;
  bool writing_ = false;
  uint64_t pending_set_ = 0;
  uint32_t pending_index_ = 0;
  uint64_t published_ = 0;
};

}  // namespace fca
