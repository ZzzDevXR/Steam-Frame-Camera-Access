#include "synthetic_source.h"

#include <algorithm>

#include "fca.h"
#include "util.h"

namespace fca {

std::vector<CameraLayout> SyntheticSource::layout() const {
  return {{"slam_right", "synthetic", 1056, 1024},
          {"slam_left", "synthetic", 1056, 1024},
          {"upper_left", "synthetic", 640, 480},
          {"upper_right", "synthetic", 640, 480}};
}

void SyntheticSource::open() { next_ns_ = now_raw_ns(); }

bool SyntheticSource::next(SharedRing& ring, int wait_ms) {
  int64_t now = now_raw_ns();
  if (now < next_ns_) {
    sleep_ns(std::min(next_ns_ - now, int64_t(wait_ms) * 1'000'000));
    now = now_raw_ns();
    if (now < next_ns_) return false;
  }
  next_ns_ = std::max(next_ns_ + period_ns_, now - period_ns_);

  const uint64_t set = ring.begin();
  fca_shm_slot& slot = ring.slot();
  uint64_t sum = 0, samples = 0;
  for (size_t c = 0; c < ring.camera_count(); ++c) {
    const fca_shm_camera& cam = ring.camera(c);
    uint8_t* image = ring.image(c);
    for (uint32_t y = 0; y < cam.height; ++y) {
      uint8_t* row = image + uint64_t(y) * cam.stride;
      const uint8_t base = pattern(0, y, set, uint32_t(c));
      for (uint32_t x = 0; x < cam.width; ++x) row[x] = uint8_t(base + x);
    }
    std::memcpy(image, &set, sizeof set);
    std::memcpy(image + cam.image_size - sizeof set, &set, sizeof set);
    if (c == 0)
      for (uint64_t i = 0; i < cam.image_size; i += 4099, ++samples) sum += image[i];
    slot.sof_ns[c] = now - 12'000'000;
    slot.eof_ns[c] = now - 4'000'000;
  }
  slot.copy_end_ns = now_raw_ns();
  slot.mean_gray = samples ? float(sum) / float(samples) : 0.f;
  slot.flags = FCA_FRAME_FLAG_SYNTHETIC;
  ring.publish();
  return true;
}

}  // namespace fca
