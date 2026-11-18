#pragma once

#include <string>
#include <vector>

#include "control_client.h"
#include "doodle_style.h"

using doodle::Button;
using doodle::Canvas;

struct PopupContent {
  std::string label;
  int more_waiting = 0;
};

void draw_popup(Canvas& canvas, const PopupContent& content, std::vector<Button>& buttons, int hover);

inline const std::string kPreviousPageAction = "previous-page", kNextPageAction = "next-page";

void draw_panel(Canvas& canvas, const DaemonState& state, const std::string& problem, std::vector<Button>& buttons,
                int hover, int& page_index);
void draw_thumbnail(Canvas& canvas);
