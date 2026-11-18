#pragma once

#include "frame_source.h"

namespace fca {

class SyntheticSource : public FrameSource {
 public:
  explicit SyntheticSource(double fps) : period_ns_(int64_t(1e9 / fps)) {}
  const char* kind() const override { return "synthetic"; }
  std::vector<CameraLayout> layout() const override;
  void open() override;
  bool next(SharedRing& ring, int wait_ms) override;
  void close() override {}
  SourceCounters counters() const override { return {}; }

  static uint8_t pattern(uint32_t x, uint32_t y, uint64_t set, uint32_t camera) {
    return uint8_t(x + y + 4 * set + 64 * camera);
  }

 private:
  int64_t period_ns_;
  int64_t next_ns_ = 0;
};

}  // namespace fca
