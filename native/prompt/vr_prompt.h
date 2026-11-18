#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vk_texture.h"

struct PointerEvent {
  enum class Type { Move, Down, Up, Leave };
  Type type;
  double x = 0, y = 0;
};

struct VrInput {
  bool quit = false;
  std::vector<PointerEvent> popup, panel;
};

class VrPrompt {
 public:
  enum class Connect { Ok, NotRunning, Error };

  ~VrPrompt();
  Connect connect(int panel_width, int panel_height, int popup_width, int popup_height, std::string& message);
  void shutdown();
  bool connected() const { return connected_; }

  VrInput poll();
  bool panel_visible() const;
  void open_panel();

  bool set_panel(const uint8_t* rgba);
  bool set_thumbnail(const uint8_t* rgba, int size);
  bool set_popup(const uint8_t* rgba);
  void show_popup();
  void hide_popup();
  bool popup_shown() const { return popup_shown_; }
  void follow(double dt);

 private:
  bool check(const char* what, int error) const;

  bool connected_ = false;
  VulkanContext vulkan_;
  OverlayTexture panel_texture_, thumbnail_texture_, popup_texture_;
  uint64_t panel_ = 0, thumbnail_ = 0, popup_ = 0;
  int panel_height_ = 0, popup_height_ = 0;
  bool popup_shown_ = false, placed_ = false, following_ = false;
  double position_[3] = {0, 0, 0};
};
