#include "doodle_style.h"

#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstring>

#include "doodle_assets.h"

namespace doodle {

namespace {

class Fonts {
 public:
  static Fonts& get() {
    static Fonts fonts;
    return fonts;
  }
  cairo_font_face_t* display() const { return display_; }
  cairo_font_face_t* body() const { return body_; }

 private:
  Fonts() {
    if (FT_Init_FreeType(&library_) != 0) return;
    display_ = load(asset_caveat_brush());
    body_ = load(asset_nunito());
  }
  cairo_font_face_t* load(Asset asset) {
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(library_, asset.data, FT_Long(asset.size), 0, &face) != 0) return nullptr;
    return cairo_ft_font_face_create_for_ft_face(face, 0);
  }
  FT_Library library_ = nullptr;
  cairo_font_face_t* display_ = nullptr;
  cairo_font_face_t* body_ = nullptr;
};

constexpr double kNoiseFrequency = 0.035, kNoiseScale = 4.0;

double lattice(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return double(h & 0xffffff) / double(0xffffff);
}

double value_noise(double x, double y, uint32_t seed) {
  const double fx = std::floor(x), fy = std::floor(y);
  const int ix = int(fx), iy = int(fy);
  double tx = x - fx, ty = y - fy;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
  const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
  return a + (b - a) * tx + (c - a + (a - b - c + d) * tx) * ty;
}

void wobble(double& x, double& y) {
  const auto channel = [&](uint32_t seed) {
    const double n = value_noise(x * kNoiseFrequency, y * kNoiseFrequency, seed) * 2.0 / 3.0 +
                     value_noise(x * kNoiseFrequency * 2, y * kNoiseFrequency * 2, seed + 17) / 3.0;
    return (n - 0.5) * kNoiseScale;
  };
  const double dx = channel(4), dy = channel(91);
  x += dx;
  y += dy;
}

struct Point {
  double x, y;
};

std::vector<Point> outline_points(double x, double y, double w, double h, const Corners& corners) {
  double rh[4], rv[4];
  for (int i = 0; i < 4; ++i) {
    rh[i] = std::clamp(corners.h[i], 0.0, w / 2);
    rv[i] = std::clamp(corners.v[i], 0.0, h / 2);
  }
  std::vector<Point> points;
  const auto line = [&](double x0, double y0, double x1, double y1) {
    const int steps = std::max(1, int(std::hypot(x1 - x0, y1 - y0) / 6.0));
    for (int i = 0; i < steps; ++i) points.push_back({x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps});
  };
  const auto arc = [&](double cx, double cy, double radius_x, double radius_y, double from_degrees) {
    const int steps = std::max(2, int((radius_x + radius_y) / 6.0));
    for (int i = 0; i < steps; ++i) {
      const double a = (from_degrees + 90.0 * i / steps) * M_PI / 180.0;
      points.push_back({cx + radius_x * std::cos(a), cy + radius_y * std::sin(a)});
    }
  };
  line(x + rh[0], y, x + w - rh[1], y);
  arc(x + w - rh[1], y + rv[1], rh[1], rv[1], -90);
  line(x + w, y + rv[1], x + w, y + h - rv[2]);
  arc(x + w - rh[2], y + h - rv[2], rh[2], rv[2], 0);
  line(x + w - rh[2], y + h, x + rh[3], y + h);
  arc(x + rh[3], y + h - rv[3], rh[3], rv[3], 90);
  line(x, y + h - rv[3], x, y + rv[0]);
  arc(x + rh[0], y + rv[0], rh[0], rv[0], 180);
  return points;
}

Corners grown(const Corners& corners, double by) {
  Corners out = corners;
  for (int i = 0; i < 4; ++i) {
    out.h[i] = std::max(0.0, out.h[i] + by);
    out.v[i] = std::max(0.0, out.v[i] + by);
  }
  return out;
}

void stroke_outline(cairo_t* cr, Color c, double width, bool dashed) {
  color(cr, c);
  cairo_set_line_width(cr, width);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  if (dashed) {
    const double dash[2] = {width * 3, width * 2.4};
    cairo_set_dash(cr, dash, 2, 0);
  }
  cairo_stroke(cr);
  cairo_set_dash(cr, nullptr, 0, 0);
}

void hover_ring(cairo_t* cr, double x, double y, double w, double h, const Corners& corners) {
  const double out = 3 + kStroke / 2;
  shape_path(cr, x - out, y - out, w + 2 * out, h + 2 * out, grown(corners, out));
  stroke_outline(cr, kOrange, kStroke, false);
}

double centred_baseline(cairo_t* cr, double y, double h) {
  cairo_font_extents_t fe;
  cairo_font_extents(cr, &fe);
  return y + h / 2 + (fe.ascent - fe.descent) / 2;
}

struct PngReader {
  Asset asset;
  size_t at = 0;
  static cairo_status_t read(void* closure, unsigned char* data, unsigned int length) {
    auto* self = static_cast<PngReader*>(closure);
    if (self->at + length > self->asset.size) return CAIRO_STATUS_READ_ERROR;
    std::memcpy(data, self->asset.data + self->at, length);
    self->at += length;
    return CAIRO_STATUS_SUCCESS;
  }
};

cairo_surface_t* mascot_surface(Pose pose) {
  static cairo_surface_t* surfaces[2] = {};
  cairo_surface_t*& surface = surfaces[pose == Pose::ThumbsUp ? 0 : 1];
  if (surface == nullptr) {
    PngReader reader{pose == Pose::ThumbsUp ? asset_mascot_thumbs_up() : asset_mascot_thinking()};
    surface = cairo_image_surface_create_from_png_stream(PngReader::read, &reader);
  }
  return cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS ? surface : nullptr;
}

}  // namespace

Canvas::Canvas(int width, int height)
    : width_(width),
      height_(height),
      surface_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height)),
      cr_(cairo_create(surface_)),
      rgba_(size_t(width) * height * 4) {}

Canvas::~Canvas() {
  cairo_destroy(cr_);
  cairo_surface_destroy(surface_);
}

void Canvas::clear() {
  cairo_save(cr_);
  cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr_);
  cairo_restore(cr_);
}

const uint8_t* Canvas::rgba() {
  cairo_surface_flush(surface_);
  const uint8_t* data = cairo_image_surface_get_data(surface_);
  const int stride = cairo_image_surface_get_stride(surface_);
  for (int y = 0; y < height_; ++y) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * stride);
    uint8_t* out = rgba_.data() + size_t(y) * width_ * 4;
    for (int x = 0; x < width_; ++x) {
      const uint32_t p = row[x];
      const uint32_t a = p >> 24;
      const auto unpremultiply = [a](uint32_t c) { return a == 0 ? 0u : std::min(255u, (c * 255 + a / 2) / a); };
      out[4 * x + 0] = uint8_t(unpremultiply((p >> 16) & 0xff));
      out[4 * x + 1] = uint8_t(unpremultiply((p >> 8) & 0xff));
      out[4 * x + 2] = uint8_t(unpremultiply(p & 0xff));
      out[4 * x + 3] = uint8_t(a);
    }
  }
  return rgba_.data();
}

const uint8_t* Canvas::rgba_premultiplied() {
  cairo_surface_flush(surface_);
  const uint8_t* data = cairo_image_surface_get_data(surface_);
  const int stride = cairo_image_surface_get_stride(surface_);
  for (int y = 0; y < height_; ++y) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * stride);
    uint8_t* out = rgba_.data() + size_t(y) * width_ * 4;
    for (int x = 0; x < width_; ++x) {
      const uint32_t p = row[x];
      out[4 * x + 0] = uint8_t(p >> 16);
      out[4 * x + 1] = uint8_t(p >> 8);
      out[4 * x + 2] = uint8_t(p);
      out[4 * x + 3] = uint8_t(p >> 24);
    }
  }
  return rgba_.data();
}

bool Canvas::write_png(const std::string& path) const {
  return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

void color(cairo_t* cr, Color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void font(cairo_t* cr, double size, Weight weight) {
  cairo_font_face_t* face = weight == Weight::Display ? Fonts::get().display() : Fonts::get().body();
  if (face != nullptr)
    cairo_set_font_face(cr, face);
  else
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size);
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  if (weight == Weight::Bold) cairo_font_options_set_variations(options, "wght=700");
  if (weight == Weight::ExtraBold) cairo_font_options_set_variations(options, "wght=800");
  if (weight == Weight::Black) cairo_font_options_set_variations(options, "wght=900");
  cairo_set_font_options(cr, options);
  cairo_font_options_destroy(options);
}

double text_width(cairo_t* cr, const std::string& s) {
  cairo_text_extents_t e;
  cairo_text_extents(cr, s.c_str(), &e);
  return e.x_advance;
}

std::string fit(cairo_t* cr, std::string s, double max_width) {
  if (max_width <= 0 || text_width(cr, s) <= max_width) return s;
  while (!s.empty()) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (text_width(cr, s + "…") <= max_width) return s + "…";
  }
  return "…";
}

double text(cairo_t* cr, const std::string& s, double x, double y, double size, Weight weight, Color c,
            double max_width, double align) {
  font(cr, size, weight);
  const std::string shown = fit(cr, s, max_width);
  const double width = text_width(cr, shown);
  color(cr, c);
  cairo_move_to(cr, x - align * width, y);
  cairo_show_text(cr, shown.c_str());
  return width;
}

int paragraph(cairo_t* cr, const std::string& s, double x, double y, double max_width, double size, Weight weight,
              Color c, double line_height, int max_lines) {
  font(cr, size, weight);
  std::vector<std::string> lines;
  std::string line;
  size_t at = 0;
  while (at < s.size()) {
    const size_t space = s.find(' ', at);
    const std::string word = s.substr(at, space == std::string::npos ? std::string::npos : space - at);
    at = space == std::string::npos ? s.size() : space + 1;
    if (word.empty()) continue;
    const std::string longer = line.empty() ? word : line + ' ' + word;
    if (!line.empty() && text_width(cr, longer) > max_width) {
      lines.push_back(line);
      line = word;
    } else {
      line = longer;
    }
  }
  if (!line.empty()) lines.push_back(line);
  if (int(lines.size()) > max_lines) {
    for (size_t i = size_t(max_lines); i < lines.size(); ++i) lines[size_t(max_lines) - 1] += ' ' + lines[i];
    lines.resize(size_t(max_lines));
  }
  for (size_t i = 0; i < lines.size(); ++i)
    text(cr, lines[i], x, y + double(i) * line_height, size, weight, c, max_width);
  return int(lines.size());
}

void shape_path(cairo_t* cr, double x, double y, double w, double h, const Corners& corners) {
  bool first = true;
  for (Point p : outline_points(x, y, w, h, corners)) {
    wobble(p.x, p.y);
    if (first)
      cairo_move_to(cr, p.x, p.y);
    else
      cairo_line_to(cr, p.x, p.y);
    first = false;
  }
  cairo_close_path(cr);
}

void shape(cairo_t* cr, double x, double y, double w, double h, const Corners& corners, const Shape& style) {
  const double inset = style.stroke / 2;
  const Corners inner = grown(corners, -inset);
  if (style.shadow) {
    shape_path(cr, x + inset + style.shadow_x, y + inset + style.shadow_y, w - 2 * inset, h - 2 * inset, inner);
    color(cr, style.shadow_color);
    cairo_fill_preserve(cr);
    stroke_outline(cr, style.shadow_color, style.stroke, false);
  }
  shape_path(cr, x + inset, y + inset, w - 2 * inset, h - 2 * inset, inner);
  if (style.filled) {
    color(cr, style.fill);
    cairo_fill_preserve(cr);
  }
  stroke_outline(cr, style.outline, style.stroke, style.dashed);
}

void rule(cairo_t* cr, double x, double y, double w, Color c) {
  const int steps = std::max(1, int(w / 6.0));
  for (int i = 0; i <= steps; ++i) {
    double px = x + w * i / steps, py = y;
    wobble(px, py);
    if (i == 0)
      cairo_move_to(cr, px, py);
    else
      cairo_line_to(cr, px, py);
  }
  stroke_outline(cr, c, 2, false);
}

void page(cairo_t* cr, double w, double h) { shape(cr, 3, 3, w - 6, h - 6, kCornerPanel, Shape{}); }

void modal(cairo_t* cr, double w, double h, Color accent, double& x, double& y, double& box_w, double& box_h) {
  Shape style;
  style.outline = accent;
  style.shadow = true;
  style.shadow_color = accent;
  style.shadow_x = 6;
  style.shadow_y = 7;
  x = y = 3;
  box_w = w - 6 - style.shadow_x;
  box_h = h - 6 - style.shadow_y;
  shape(cr, x, y, box_w, box_h, kCornerPanelAlt, style);
}

void icon(cairo_t* cr, Icon which, double x, double y, double size, Color c) {
  if (which == Icon::None) return;
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  switch (which) {
    case Icon::Tick:
      cairo_move_to(cr, 4.5, 12.5);
      cairo_line_to(cr, 9.5, 17.5);
      cairo_line_to(cr, 19.5, 6.5);
      break;
    case Icon::Bang:
      cairo_move_to(cr, 12, 4.5);
      cairo_line_to(cr, 12, 13.5);
      cairo_move_to(cr, 12, 19);
      cairo_line_to(cr, 12, 19.5);
      break;
    case Icon::Cross:
      cairo_move_to(cr, 6, 6.5);
      cairo_line_to(cr, 18, 17.5);
      cairo_move_to(cr, 18, 6);
      cairo_line_to(cr, 6, 18);
      break;
    case Icon::None:
      break;
  }
  stroke_outline(cr, c, 3.6, false);
  cairo_restore(cr);
}

namespace {

constexpr double kChipPadding = 14, kChipIcon = 18, kChipGap = 8;

Icon chip_icon(Chip kind) {
  switch (kind) {
    case Chip::Good:
      return Icon::Tick;
    case Chip::Warn:
      return Icon::Bang;
    case Chip::Error:
      return Icon::Cross;
    default:
      return Icon::None;
  }
}

}  // namespace

double chip_width(cairo_t* cr, const std::string& label, Chip kind) {
  font(cr, kChipText, Weight::ExtraBold);
  return 2 * kChipPadding + (chip_icon(kind) != Icon::None ? kChipIcon + kChipGap : 0) + text_width(cr, label);
}

double chip(cairo_t* cr, const std::string& label, Chip kind, double x, double y, double max_width) {
  const Icon mark = chip_icon(kind);
  const double extra = 2 * kChipPadding + (mark != Icon::None ? kChipIcon + kChipGap : 0);
  font(cr, kChipText, Weight::ExtraBold);
  const std::string shown = fit(cr, label, max_width > 0 ? max_width - extra : 0);
  const double w = extra + text_width(cr, shown);
  Shape style;
  Color ink = kOnAccent;
  switch (kind) {
    case Chip::Neutral:
      style.filled = false;
      ink = kChalk;
      break;
    case Chip::Good:
      style.fill = kBlue;
      break;
    case Chip::Warn:
      style.fill = kOrange;
      break;
    case Chip::Error:
      style.fill = kRed;
      break;
    case Chip::New:
      style.fill = kBlush;
      break;
    case Chip::Off:
      style.filled = false;
      style.outline = kChalkDim;
      style.dashed = true;
      ink = kChalkMuted;
      break;
  }
  shape(cr, x, y, w, kChipHeight, kCornerChip, style);
  double tx = x + kChipPadding;
  if (mark != Icon::None) {
    icon(cr, mark, tx, y + (kChipHeight - kChipIcon) / 2, kChipIcon, ink);
    tx += kChipIcon + kChipGap;
  }
  font(cr, kChipText, Weight::ExtraBold);
  text(cr, shown, tx, centred_baseline(cr, y, kChipHeight), kChipText, Weight::ExtraBold, ink);
  return w;
}

void button(cairo_t* cr, std::vector<Button>& buttons, int hover, const std::string& label, Style style, double x,
            double y, double w, double h, const std::string& decision, const std::string& key, bool alt) {
  const bool enabled = style != Style::Disabled;
  const bool hovered = enabled && hover == int(buttons.size());
  if (enabled) buttons.push_back({x, y, w, h, decision, key});

  Shape look;
  Color ink = kOnAccent;
  bool first_corners = true;
  switch (style) {
    case Style::Outline:
      look.filled = false;
      ink = kChalk;
      first_corners = false;
      break;
    case Style::Primary:
      look.fill = kBlue;
      look.shadow = true;
      look.shadow_color = kOrange;
      break;
    case Style::Loud:
      look.fill = kOrange;
      look.shadow = true;
      look.shadow_color = kBlue;
      first_corners = false;
      break;
    case Style::Danger:
      look.fill = kRed;
      look.shadow = true;
      look.shadow_color = kChalk;
      break;
    case Style::Selected:
      look.fill = kBlue;
      break;
    case Style::Disabled:
      look.fill = kWell;
      look.outline = kChalkDim;
      look.dashed = true;
      ink = kChalkDim;
      first_corners = false;
      break;
  }
  const Corners& corners = first_corners != alt ? kCornerButton : kCornerButtonAlt;
  shape(cr, x, y, w, h, corners, look);
  if (hovered) hover_ring(cr, x, y, w, h, corners);

  const bool large = h >= kControlLg;
  const double size = large ? kLabelLg : kLabel;
  const Weight weight = large ? Weight::Black : Weight::ExtraBold;
  const double mark = style == Style::Selected ? kChipIcon + kChipGap : 0;
  font(cr, size, weight);
  const std::string shown = fit(cr, label, w - 24 - mark);
  const double content = mark + text_width(cr, shown), baseline = centred_baseline(cr, y, h);
  double tx = x + (w - content) / 2;
  if (mark > 0) {
    icon(cr, Icon::Tick, tx, y + (h - kChipIcon) / 2, kChipIcon, ink);
    tx += mark;
  }
  text(cr, shown, tx, baseline, size, weight, ink);
}

void toggle(cairo_t* cr, std::vector<Button>& buttons, int hover, bool on, bool enabled, double x, double y,
            const std::string& decision, const std::string& key) {
  const double w = kSwitchWidth, h = kSwitchHeight, knob = 34;
  const bool hovered = enabled && hover == int(buttons.size());
  if (enabled) buttons.push_back({x, y, w, h, decision, key});
  Shape track;
  track.fill = on && enabled ? kBlue : kWell;
  if (!enabled) {
    track.outline = kChalkDim;
    track.dashed = true;
  }
  shape(cr, x, y, w, h, kCornerSwitch, track);
  if (hovered) hover_ring(cr, x, y, w, h, kCornerSwitch);
  const double pad = kStroke + 4;
  const double kx = on ? x + w - pad - knob : x + pad, ky = y + (h - knob) / 2;
  Shape dot;
  dot.fill = on && enabled ? kChalk : kChalkDim;
  dot.outline = kOnAccent;
  shape(cr, kx, ky, knob, knob, Corners{{17.7, 16.3, 15.6, 17}, {17.7, 16.3, 15.6, 17}}, dot);
}

void mascot(cairo_t* cr, Pose pose, double x, double y, double size) {
  cairo_surface_t* surface = mascot_surface(pose);
  if (surface == nullptr) return;
  cairo_save(cr);
  cairo_translate(cr, x, y);
  const double scale = size / cairo_image_surface_get_width(surface);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_restore(cr);
}

void sticker_icon(Canvas& canvas, Pose pose) {
  cairo_t* cr = canvas.cr();
  canvas.clear();
  cairo_save(cr);
  const double unit = std::min(canvas.width(), canvas.height()) / 256.0;
  cairo_translate(cr, canvas.width() / 2.0, canvas.height() / 2.0);
  cairo_rotate(cr, -2.5 * M_PI / 180.0);
  cairo_scale(cr, unit, unit);
  cairo_translate(cr, -128, -128);
  const double x = 22, y = 20, side = 200, stroke = 9, shadow_x = 10, shadow_y = 12;
  const Corners corners{{44, 32, 46, 30}, {32, 46, 30, 44}};
  shape_path(cr, x + shadow_x, y + shadow_y, side, side, corners);
  color(cr, kOrange);
  cairo_fill_preserve(cr);
  stroke_outline(cr, kOrange, stroke, false);
  shape_path(cr, x, y, side, side, corners);
  color(cr, kGround);
  cairo_fill_preserve(cr);
  cairo_save(cr);
  cairo_clip(cr);
  mascot(cr, pose, x, y, side);
  cairo_restore(cr);
  shape_path(cr, x, y, side, side, corners);
  stroke_outline(cr, kBlue, stroke, false);
  cairo_restore(cr);
}

}  // namespace doodle
