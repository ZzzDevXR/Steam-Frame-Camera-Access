#include "doodle_assets.h"

#ifndef DOODLE_ASSET_DIR
#define DOODLE_ASSET_DIR "assets"
#endif

// clang-format off
#define DOODLE_EMBED(symbol, file)                                 \
  __asm__(".pushsection .rodata\n"                                 \
          ".balign 16\n"                                           \
          ".global " #symbol "\n"                                  \
          #symbol ":\n"                                            \
          ".incbin \"" DOODLE_ASSET_DIR "/" file "\"\n"            \
          ".global " #symbol "_end\n"                              \
          #symbol "_end:\n"                                        \
          ".byte 0\n"                                              \
          ".popsection\n");                                        \
  extern "C" {                                                     \
  extern const unsigned char symbol[];                             \
  extern const unsigned char symbol##_end[];                       \
  }
// clang-format on

DOODLE_EMBED(doodle_asset_caveat_brush, "CaveatBrush-Regular.ttf")
DOODLE_EMBED(doodle_asset_nunito, "Nunito-Variable.ttf")
DOODLE_EMBED(doodle_asset_mascot_thumbs_up, "mascot-thumbs-up.png")
DOODLE_EMBED(doodle_asset_mascot_thinking, "mascot-thinking.png")

namespace doodle {

#define DOODLE_ASSET(symbol) \
  Asset { symbol, static_cast<size_t>(symbol##_end - symbol) }

Asset asset_caveat_brush() { return DOODLE_ASSET(doodle_asset_caveat_brush); }
Asset asset_nunito() { return DOODLE_ASSET(doodle_asset_nunito); }
Asset asset_mascot_thumbs_up() { return DOODLE_ASSET(doodle_asset_mascot_thumbs_up); }
Asset asset_mascot_thinking() { return DOODLE_ASSET(doodle_asset_mascot_thinking); }

}  // namespace doodle
