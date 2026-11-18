#pragma once

#include <cstdint>
#include <vector>

#include "shared_ring.h"

namespace fca {

struct SourceCounters {
  uint64_t copy_races = 0;
  uint64_t invalid_metadata = 0;
  uint64_t dark_skipped = 0;
};

class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual const char* kind() const = 0;
  virtual std::vector<CameraLayout> layout() const = 0;
  virtual void open() = 0;
  virtual bool next(SharedRing& ring, int wait_ms) = 0;
  virtual void close() = 0;
  virtual SourceCounters counters() const = 0;
};

}  // namespace fca
