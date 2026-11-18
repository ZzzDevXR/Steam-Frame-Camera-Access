#pragma once

#include <cstddef>

namespace doodle {

struct Asset {
  const unsigned char* data;
  size_t size;
};

Asset asset_caveat_brush();
Asset asset_nunito();
Asset asset_mascot_thumbs_up();
Asset asset_mascot_thinking();

}  // namespace doodle
