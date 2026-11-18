#include "ui.h"

#include <algorithm>
#include <cmath>

using namespace doodle;

namespace {

std::pair<std::string, std::string> split_label(const std::string& label) {
  const size_t where_start = label.rfind(" (");
  if (where_start == std::string::npos || label.back() != ')') return {label, ""};
  return {label.substr(0, where_start), label.substr(where_start + 2, label.size() - where_start - 3)};
}

}  // namespace

void draw_popup(Canvas& canvas, const PopupContent& content, std::vector<Button>& buttons, int hover) {
  cairo_t* cr = canvas.cr();
  buttons.clear();
  canvas.clear();
  double box_x = 0, box_y = 0, box_w = 0, box_h = 0;
  modal(cr, canvas.width(), canvas.height(), kOrange, box_x, box_y, box_w, box_h);

  const double pad = 29, mascot_size = 290;
  cairo_save(cr);
  shape_path(cr, box_x + kStroke, box_y + kStroke, box_w - 2 * kStroke, box_h - 2 * kStroke, kCornerPanelAlt);
  cairo_clip(cr);
  mascot(cr, Pose::Thinking, box_x + 14, box_y + box_h - kStroke - mascot_size, mascot_size);
  cairo_restore(cr);

  const double x = box_x + 14 + mascot_size + 24, right = box_x + box_w - pad, w = right - x;
  double y = box_y + pad + 36;
  const double title_w = text(cr, "Camera access?", x, y, kDisplayLg, Weight::Display, kChalk);
  chip(cr, "Request", Chip::Warn, x + title_w + 14, y - 31);

  const auto [name, where] = split_label(content.label);
  y += 46;
  text(cr, name, x, y, kLabelLg, Weight::Black, kChalk, w);
  y += 30;
  text(cr, "Runs in: " + (where.empty() ? std::string("this headset") : where), x, y, kBody, Weight::Bold, kChalkMuted,
       w);
  y += 34;
  y += 27 * paragraph(cr, "It wants to see the four tracking cameras while it runs.", x, y, w, kBody, Weight::Bold,
                      kChalkMuted, 27, 2);

  const double gap = 16, button_w = (w - 2 * gap) / 3, button_y = y - 6;
  button(cr, buttons, hover, "Deny", Style::Outline, x, button_y, button_w, kControlLg, "deny", "");
  button(cr, buttons, hover, "Allow once", Style::Outline, x + button_w + gap, button_y, button_w, kControlLg, "once",
         "", true);
  button(cr, buttons, hover, "Always allow", Style::Outline, x + 2 * (button_w + gap), button_y, button_w, kControlLg,
         "allow", "");

  y = button_y + kControlLg + 34;
  double note_w = w;
  if (content.more_waiting > 0)
    note_w -= 20 + text(cr,
                        "+" + std::to_string(content.more_waiting) +
                            (content.more_waiting == 1 ? " more app waiting" : " more apps waiting"),
                        right, y, kCaption, Weight::ExtraBold, kOrange, 0, 1);
  text(cr, "Change it later: dashboard › Camera access", x, y, kCaption, Weight::Bold, kChalkMuted, note_w);
}

void draw_panel(Canvas& canvas, const DaemonState& state, const std::string& problem, std::vector<Button>& buttons,
                int hover, int& page_index) {
  cairo_t* cr = canvas.cr();
  const double w = canvas.width(), h = canvas.height(), margin = 44;
  buttons.clear();
  canvas.clear();
  page(cr, w, h);

  const double title_y = 82;
  const double title_w = text(cr, "Camera access", margin, title_y, kDisplayLg, Weight::Display, kChalk);
  int reading = 0;
  for (const AppEntry& app : state.apps) reading += app.connections > 0 ? 1 : 0;
  Chip kind = Chip::Good;
  std::string word = "Cameras on", detail;
  if (!problem.empty()) {
    kind = Chip::Error;
    word = "Error";
    detail = problem;
  } else if (state.state == "waiting") {
    kind = Chip::Warn;
    word = "Waiting";
    detail = "No pictures from the cameras yet. Is the headset asleep?";
  } else if (state.state == "unavailable") {
    kind = Chip::Error;
    word = "Unavailable";
    detail = state.status;
  } else if (state.state != "streaming") {
    kind = Chip::Neutral;
    word = state.state.empty() ? "Starting" : state.state;
  }
  if (detail.empty())
    detail = reading == 0   ? "No app is reading the cameras right now."
             : reading == 1 ? "1 app is reading the cameras right now."
                            : std::to_string(reading) + " apps are reading the cameras right now.";
  chip(cr, word, kind, margin + title_w + 16, title_y - 32);
  text(cr, detail, margin, 124, kBody, Weight::Bold, kChalkMuted, w - 2 * margin);

  const double top = 150, header_h = 38, row_h = 88, status_x = 540, button_w = 118, gap = 12;
  text(cr, "App", margin, top + 26, kCaption, Weight::ExtraBold, kChalkDim);
  text(cr, "Access", status_x, top + 26, kCaption, Weight::ExtraBold, kChalkDim);
  rule(cr, margin, top + header_h, w - 2 * margin, kChalk);
  const int rows_per_page = int((h - top - header_h - 44) / row_h);
  const int app_count = int(state.apps.size());
  const int page_count = std::max(1, (app_count + rows_per_page - 1) / rows_per_page);
  page_index = std::clamp(page_index, 0, page_count - 1);
  const int first_app = page_index * rows_per_page;
  if (state.apps.empty() && problem.empty())
    text(cr, "No app has asked for the cameras yet.", margin, top + header_h + 50, kBody, Weight::Bold, kChalkMuted);
  for (int row = 0; row < rows_per_page && first_app + row < app_count; ++row) {
    const AppEntry& app = state.apps[size_t(first_app + row)];
    const double y = top + header_h + row * row_h;
    const auto [name, where] = split_label(app.label);
    text(cr, name, margin, y + 38, kLabel, Weight::ExtraBold, kChalk, status_x - margin - 20);
    text(cr, where, margin, y + 64, kCaption, Weight::Bold, kChalkMuted, status_x - margin - 20);

    std::vector<std::pair<std::string, std::string>> actions;
    if (app.decision == "pending")
      actions = {{"Deny", "deny"}, {"Once", "once"}, {"Allow", "allow"}};
    else if (app.decision == "deny")
      actions = {{"Allow", "allow"}, {"Forget", "forget"}};
    else
      actions = {{"Deny", "deny"}, {"Forget", "forget"}};
    const double actions_x = w - margin - (actions.size() * button_w + (actions.size() - 1) * gap);

    const double status_w = actions_x - status_x - 16, chip_y = y + (app.connections > 0 ? 12 : 25);
    if (app.decision == "allow")
      chip(cr, "Allowed", Chip::Good, status_x, chip_y, status_w);
    else if (app.decision == "once")
      chip(cr, "Until it closes", Chip::Good, status_x, chip_y, status_w);
    else if (app.decision == "deny")
      chip(cr, "Denied", Chip::Off, status_x, chip_y, status_w);
    else
      chip(cr, "Waiting for you", Chip::Warn, status_x, chip_y, status_w);
    if (app.connections > 0)
      text(cr, "reading now", status_x + 4, y + 74, kCaption, Weight::ExtraBold, kBlue, status_w);

    double x = actions_x;
    bool alt = false;
    for (const auto& [label, decision] : actions) {
      button(cr, buttons, hover, label, Style::Outline, x, y + (row_h - kControl) / 2, button_w, kControl, decision,
             app.key, alt);
      x += button_w + gap;
      alt = !alt;
    }
    rule(cr, margin, y + row_h, w - 2 * margin);
  }
  if (page_count > 1) {
    const double pager_w = 150, pager_y = h - 28 - kControlMin;
    const double next_x = w - margin - pager_w, previous_x = next_x - gap - pager_w;
    const std::string position = "Page " + std::to_string(page_index + 1) + " of " + std::to_string(page_count);
    const Style previous_style = page_index > 0 ? Style::Outline : Style::Disabled;
    const Style next_style = page_index + 1 < page_count ? Style::Outline : Style::Disabled;
    text(cr, position, previous_x - 20, pager_y + 28, kCaption, Weight::Bold, kChalkMuted, 0, 1);
    button(cr, buttons, hover, "Previous", previous_style, previous_x, pager_y, pager_w, kControlMin,
           kPreviousPageAction);
    button(cr, buttons, hover, "Next", next_style, next_x, pager_y, pager_w, kControlMin, kNextPageAction, "", true);
  }
}

void draw_thumbnail(Canvas& canvas) { sticker_icon(canvas, Pose::Thinking); }
