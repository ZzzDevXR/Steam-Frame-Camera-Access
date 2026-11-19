#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace doodle {

struct Color {
  double r, g, b, a = 1.0;
};
constexpr Color rgb(int hex, double a = 1.0) {
  return {((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0, (hex & 0xff) / 255.0, a};
}
constexpr Color kGround = rgb(0x000000);
constexpr Color kWell = rgb(0x2a2a2a);
constexpr Color kChalk = rgb(0xfff3e8);
constexpr Color kChalkMuted = rgb(0xc9beb3);
constexpr Color kChalkDim = rgb(0xa69c92);
constexpr Color kBlue = rgb(0x1e9bf0);
constexpr Color kOrange = rgb(0xf2600c);
constexpr Color kRed = rgb(0xf54b46);
constexpr Color kBlush = rgb(0xf5a3b1);
constexpr Color kOnAccent = rgb(0x000000);

enum class Weight { Display, Bold, ExtraBold, Black };

constexpr double kDisplayLg = 46, kDisplayMd = 36, kLabelLg = 24, kLabel = 20, kBody = 20, kChipText = 18,
                 kCaption = 17;
constexpr double kStroke = 3;
constexpr double kControl = 56;
constexpr double kControlLg = 68;
constexpr double kControlMin = 44;
constexpr double kChipHeight = 38;
constexpr double kSwitchWidth = 88, kSwitchHeight = 48;

struct Corners {
  double h[4], v[4];
};
constexpr Corners kCornerButton{{18, 26, 16, 28}, {26, 16, 28, 18}};
constexpr Corners kCornerButtonAlt{{26, 16, 28, 18}, {18, 26, 16, 28}};
constexpr Corners kCornerChip{{16, 20, 15, 19}, {16, 20, 15, 19}};
constexpr Corners kCornerPanel{{28, 20, 30, 18}, {20, 30, 18, 28}};
constexpr Corners kCornerPanelAlt{{20, 30, 18, 28}, {30, 18, 28, 20}};
constexpr Corners kCornerSwitch{{28, 24, 26, 22}, {22, 26, 24, 28}};

struct Button {
  double x = 0, y = 0, w = 0, h = 0;
  std::string decision;
  std::string key;
  bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

class Canvas {
 public:
  Canvas(int width, int height);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  cairo_t* cr() const { return cr_; }
  int width() const { return width_; }
  int height() const { return height_; }
  void clear();
  const uint8_t* rgba();
  const uint8_t* rgba_premultiplied();
  bool write_png(const std::string& path) const;

 private:
  int width_, height_;
  cairo_surface_t* surface_;
  cairo_t* cr_;
  std::vector<uint8_t> rgba_;
};

void color(cairo_t* cr, Color c);

void font(cairo_t* cr, double size, Weight weight);
double text_width(cairo_t* cr, const std::string& s);
std::string fit(cairo_t* cr, std::string s, double max_width);
double text(cairo_t* cr, const std::string& s, double x, double y, double size, Weight weight, Color c,
            double max_width = 0, double align = 0);
int paragraph(cairo_t* cr, const std::string& s, double x, double y, double max_width, double size, Weight weight,
              Color c, double line_height, int max_lines = 3);

struct Shape {
  Color fill = kGround;
  bool filled = true;
  Color outline = kChalk;
  bool dashed = false;
  double stroke = kStroke;
  bool shadow = false;
  Color shadow_color = kOrange;
  double shadow_x = 4, shadow_y = 5;
};
void shape_path(cairo_t* cr, double x, double y, double w, double h, const Corners& corners);
void shape(cairo_t* cr, double x, double y, double w, double h, const Corners& corners, const Shape& style);
void rule(cairo_t* cr, double x, double y, double w, Color c = kChalkDim);

void page(cairo_t* cr, double w, double h);
void modal(cairo_t* cr, double w, double h, Color accent, double& x, double& y, double& box_w, double& box_h);

enum class Icon { None, Tick, Bang, Cross };
void icon(cairo_t* cr, Icon which, double x, double y, double size, Color c);

enum class Chip { Neutral, Good, Warn, Error, New, Off };
double chip(cairo_t* cr, const std::string& label, Chip kind, double x, double y, double max_width = 0);
double chip_width(cairo_t* cr, const std::string& label, Chip kind);

enum class Style { Outline, Primary, Loud, Danger, Selected, Disabled };
void button(cairo_t* cr, std::vector<Button>& buttons, int hover, const std::string& label, Style style, double x,
            double y, double w, double h, const std::string& decision, const std::string& key = "", bool alt = false);
void toggle(cairo_t* cr, std::vector<Button>& buttons, int hover, bool on, bool enabled, double x, double y,
            const std::string& decision, const std::string& key = "");

enum class Pose { ThumbsUp, Thinking };
void mascot(cairo_t* cr, Pose pose, double x, double y, double size);
void sticker_icon(Canvas& canvas, Pose pose);

}  // namespace doodle
