#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>

#include "control_client.h"
#include "ui.h"
#include "vr_prompt.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr int kPanelWidth = 1200, kPanelHeight = 810;
constexpr int kPopupWidth = 1000, kPopupHeight = 400;
constexpr int kThumbnailSize = 256;

using Clock = std::chrono::steady_clock;
double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

struct Pointer {
  explicit Pointer(Clock::duration settle_time) : settle(settle_time) {}
  Clock::duration settle;
  int hover = -1;
  std::string pressed;
  std::string layout;
  Clock::time_point armed_at{};

  static std::string id(const Button& button) { return button.decision + '\t' + button.key; }

  void drawn(const std::vector<Button>& buttons, const std::string& subject, Clock::time_point now) {
    std::string current = subject + '\n';
    for (const Button& button : buttons) current += id(button) + '\n';
    if (current == layout) return;
    layout = current;
    pressed.clear();
    armed_at = now + settle;
  }

  int handle(const PointerEvent& event, const std::vector<Button>& buttons, Clock::time_point now, bool& redraw) {
    int hovered = -1;
    for (size_t i = 0; i < buttons.size(); ++i)
      if (buttons[i].contains(event.x, event.y)) hovered = int(i);
    int clicked = -1;
    switch (event.type) {
      case PointerEvent::Type::Move:
        break;
      case PointerEvent::Type::Down:
        pressed = hovered >= 0 && now >= armed_at ? id(buttons[size_t(hovered)]) : std::string();
        break;
      case PointerEvent::Type::Up:
        if (hovered >= 0 && !pressed.empty() && id(buttons[size_t(hovered)]) == pressed) clicked = hovered;
        pressed.clear();
        break;
      case PointerEvent::Type::Leave:
        hovered = -1;
        pressed.clear();
        break;
    }
    if (hovered != hover) {
      hover = hovered;
      redraw = true;
    }
    return clicked;
  }
};

constexpr auto kPopupSettle = std::chrono::milliseconds(1000);
constexpr auto kPanelSettle = std::chrono::milliseconds(500);

bool is_pending(const DaemonState& state, const std::string& key) {
  for (const AppEntry& app : state.apps)
    if (app.key == key) return app.decision == "pending";
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_name = "@frame-camera-access-control", preview_dir;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--control-socket") && i + 1 < argc)
      socket_name = argv[++i];
    else if (!std::strcmp(argv[i], "--preview") && i + 1 < argc)
      preview_dir = argv[++i];
    else {
      std::fprintf(stderr,
                   "usage: frame-camera-access-prompt [--control-socket @NAME] [--preview DIR]\n"
                   "  --preview DIR  draw the popup, panel and icon with sample content as PNGs in DIR, then exit\n");
      return 2;
    }
  }
  if (!preview_dir.empty()) {
    Canvas popup(kPopupWidth, kPopupHeight), panel(kPanelWidth, kPanelHeight), icon(kThumbnailSize, kThumbnailSize);
    std::vector<Button> buttons;
    draw_popup(popup, {"com.example.mixedreality (Steam app 3029110)", 1}, buttons, 2);
    DaemonState sample;
    sample.state = "streaming";
    sample.apps = {
        {"pending", "lepton:steamlaunch-3029110:com.example.mixedreality",
         "com.example.mixedreality (Steam app 3029110)", 0},
        {"allow", "lepton:dev:com.framecameraaccess.tagtracking", "com.framecameraaccess.tagtracking (Lepton dev)", 1},
        {"once", "exe:/home/steamos/tools/recorder", "recorder (/home/steamos/tools/recorder)", 0},
        {"deny", "lepton:steamlaunch-1408230:com.other.game", "com.other.game (Steam app 1408230)", 0}};
    int sample_page = 0;
    draw_panel(panel, sample, "", buttons, 0, sample_page);
    draw_thumbnail(icon);
    for (const auto& [name, canvas] :
         {std::pair<const char*, Canvas*>{"popup", &popup}, {"panel", &panel}, {"icon", &icon}}) {
      const std::string path = preview_dir + "/" + name + ".png";
      cairo_surface_write_to_png(cairo_get_target(canvas->cr()), path.c_str());
      std::printf("wrote %s\n", path.c_str());
    }
    return 0;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  Canvas panel(kPanelWidth, kPanelHeight), popup(kPopupWidth, kPopupHeight), thumbnail(kThumbnailSize, kThumbnailSize);
  std::vector<Button> panel_buttons, popup_buttons;
  Pointer panel_pointer(kPanelSettle), popup_pointer(kPopupSettle);
  std::string popup_key;
  int panel_page = 0;
  ControlClient control;
  DaemonState state;
  std::deque<std::pair<std::string, std::string>> queue;
  VrPrompt vr;
  std::string problem = "Connecting to the camera service...", last_logged;
  bool panel_dirty = true, popup_dirty = true, panel_was_visible = false;
  Clock::time_point next_daemon_try{}, next_vr_try{}, previous_tick = Clock::now();

  const auto log_once = [&](const std::string& message) {
    if (message != last_logged) std::fprintf(stderr, "%s\n", message.c_str());
    last_logged = message;
  };

  while (!g_stop) {
    const Clock::time_point now = Clock::now();
    const double elapsed_seconds = seconds(now - previous_tick);
    previous_tick = now;

    if (!control.connected() && now >= next_daemon_try) {
      std::string error;
      if (control.connect(socket_name, error)) {
        log_once("connected to the camera service");
      } else {
        problem = "Camera service not reachable: " + error;
        log_once(problem);
        panel_dirty = true;
      }
      next_daemon_try = now + std::chrono::seconds(2);
    }
    if (control.connected() || state.connected) {
      std::vector<std::pair<std::string, std::string>> requests;
      if (control.poll(state, requests)) {
        panel_dirty = true;
        if (!control.connected()) {
          state = DaemonState{};
          queue.clear();
          popup_dirty = true;
          problem = "Lost the camera service (or it refused this program: only the installed copy may answer)";
          log_once(problem);
        } else {
          problem.clear();
        }
      }
      for (const auto& request : requests) {
        bool queued = false;
        for (const auto& waiting : queue) queued = queued || waiting.first == request.first;
        if (!queued) {
          queue.push_back(request);
          popup_dirty = true;
          std::fprintf(stderr, "request: %s\n", request.second.c_str());
        }
      }
    }
    while (!queue.empty() && state.connected && !is_pending(state, queue.front().first)) {
      queue.pop_front();
      popup_dirty = true;
    }

    if (!vr.connected() && now >= next_vr_try) {
      std::string message;
      switch (vr.connect(kPanelWidth, kPanelHeight, kPopupWidth, kPopupHeight, message)) {
        case VrPrompt::Connect::Ok:
          draw_thumbnail(thumbnail);
          vr.set_thumbnail(thumbnail.rgba(), kThumbnailSize);
          panel_dirty = popup_dirty = true;
          break;
        case VrPrompt::Connect::NotRunning:
          log_once("waiting for SteamVR");
          next_vr_try = now + std::chrono::seconds(3);
          break;
        case VrPrompt::Connect::Error:
          log_once("SteamVR: " + message);
          next_vr_try = now + std::chrono::seconds(5);
          break;
      }
    }
    if (vr.connected()) {
      const VrInput input = vr.poll();
      if (input.quit) {
        vr.shutdown();
        next_vr_try = now + std::chrono::seconds(3);
        continue;
      }
      for (const PointerEvent& event : input.popup) {
        const int clicked = popup_pointer.handle(event, popup_buttons, now, popup_dirty);
        if (clicked >= 0 && !queue.empty() && queue.front().first == popup_key) {
          control.send_decision(popup_buttons[size_t(clicked)].decision, queue.front().first);
          std::fprintf(stderr, "%s: %s\n", popup_buttons[size_t(clicked)].decision.c_str(),
                       queue.front().second.c_str());
          queue.pop_front();
          popup_dirty = true;
        }
      }
      for (const PointerEvent& event : input.panel) {
        const int clicked = panel_pointer.handle(event, panel_buttons, now, panel_dirty);
        if (clicked < 0) continue;
        const Button& pressed = panel_buttons[size_t(clicked)];
        if (pressed.decision == kPreviousPageAction || pressed.decision == kNextPageAction) {
          panel_page += pressed.decision == kNextPageAction ? 1 : -1;
          panel_dirty = true;
        } else {
          control.send_decision(pressed.decision, pressed.key);
          std::fprintf(stderr, "%s (dashboard): %s\n", pressed.decision.c_str(), pressed.key.c_str());
        }
      }
      if (!queue.empty()) {
        if (popup_dirty) {
          draw_popup(popup, {queue.front().second, int(queue.size()) - 1}, popup_buttons, popup_pointer.hover);
          vr.set_popup(popup.rgba());
          popup_key = queue.front().first;
          popup_pointer.drawn(popup_buttons, popup_key, now);
          popup_dirty = false;
        }
        vr.show_popup();
        vr.follow(elapsed_seconds);
      } else {
        vr.hide_popup();
        popup_key.clear();
        popup_pointer = Pointer(kPopupSettle);
      }
      const bool visible = vr.panel_visible();
      if (visible && !panel_was_visible) panel_dirty = true;
      panel_was_visible = visible;
      if (panel_dirty) {
        draw_panel(panel, state, problem, panel_buttons, panel_pointer.hover, panel_page);
        vr.set_panel(panel.rgba());
        panel_pointer.drawn(panel_buttons, "", now);
        panel_dirty = false;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(vr.popup_shown() ? 11 : 50));
  }
  vr.shutdown();
  return 0;
}
