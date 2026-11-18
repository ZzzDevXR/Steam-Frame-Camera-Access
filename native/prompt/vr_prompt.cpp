#include "vr_prompt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "openvr.h"

namespace {

const char* const kPanelKey = "framecameraaccess.permissions";
const char* const kPanelName = "Camera access";
const char* const kPopupKey = "framecameraaccess.request";
const char* const kPopupName = "Camera access request";
constexpr float kPanelWidthM = 2.4f;
constexpr float kPopupWidthM = 0.55f;
constexpr double kPopupDistanceM = 0.75;
constexpr double kPopupDropM = 0.08;
constexpr double kFollowStartRad = 20.0 * M_PI / 180.0;
constexpr double kFollowStopRad = 2.0 * M_PI / 180.0;
constexpr double kFollowStartDistanceM = 0.3;
constexpr double kFollowTimeS = 0.25;

struct V3 {
  double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalized(V3 a) {
  const double l = length(a);
  return l > 1e-9 ? a * (1.0 / l) : V3{0, 0, 1};
}
V3 column(const vr::HmdMatrix34_t& m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }
double angle_between(V3 a, V3 b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0, 1.0)); }

}  // namespace

VrPrompt::~VrPrompt() { shutdown(); }

bool VrPrompt::check(const char* what, int error) const {
  if (error == vr::VROverlayError_None) return true;
  std::fprintf(stderr, "[VR] %s -> %s\n", what,
               vr::VROverlay()->GetOverlayErrorNameFromEnum(vr::EVROverlayError(error)));
  return false;
}

VrPrompt::Connect VrPrompt::connect(int panel_width, int panel_height, int popup_width, int popup_height,
                                    std::string& message) {
  if (connected_) return Connect::Ok;
  vr::EVRInitError error = vr::VRInitError_None;
  vr::VR_Init(&error, vr::VRApplication_Background);
  if (error != vr::VRInitError_None) {
    message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
    return error == vr::VRInitError_Init_NoServerForBackgroundApp ? Connect::NotRunning : Connect::Error;
  }
  vr::VR_Shutdown();
  vr::VR_Init(&error, vr::VRApplication_Overlay);
  if (error != vr::VRInitError_None) {
    message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
    return Connect::Error;
  }
  connected_ = true;
  if (!vulkan_.init(message)) {
    message = "Vulkan: " + message;
    shutdown();
    return Connect::Error;
  }
  vr::IVROverlay* overlay = vr::VROverlay();
  vr::VROverlayHandle_t panel = 0, thumbnail = 0, popup = 0;
  const bool panel_made =
      check("CreateDashboardOverlay", overlay->CreateDashboardOverlay(kPanelKey, kPanelName, &panel, &thumbnail));
  if (panel_made) {
    panel_ = panel;
    thumbnail_ = thumbnail;
  }
  const bool popup_made =
      panel_made && check("CreateOverlay(popup)", overlay->CreateOverlay(kPopupKey, kPopupName, &popup));
  if (popup_made) popup_ = popup;
  if (!popup_made) {
    message = "cannot create the overlays (is another copy of the prompt running?)";
    shutdown();
    return Connect::Error;
  }
  panel_height_ = panel_height;
  popup_height_ = popup_height;

  check("SetOverlayWidthInMeters(panel)", overlay->SetOverlayWidthInMeters(panel, kPanelWidthM));
  check("SetOverlayInputMethod(panel)", overlay->SetOverlayInputMethod(panel, vr::VROverlayInputMethod_Mouse));
  const vr::HmdVector2_t panel_scale = {{float(panel_width), float(panel_height)}};
  check("SetOverlayMouseScale(panel)", overlay->SetOverlayMouseScale(panel, &panel_scale));

  check("SetOverlayWidthInMeters(popup)", overlay->SetOverlayWidthInMeters(popup, kPopupWidthM));
  check("SetOverlayInputMethod(popup)", overlay->SetOverlayInputMethod(popup, vr::VROverlayInputMethod_Mouse));
  const vr::HmdVector2_t popup_scale = {{float(popup_width), float(popup_height)}};
  check("SetOverlayMouseScale(popup)", overlay->SetOverlayMouseScale(popup, &popup_scale));
  check("SetOverlayFlag(MakeOverlaysInteractiveIfVisible)",
        overlay->SetOverlayFlag(popup, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true));
  check("SetOverlaySortOrder(popup)", overlay->SetOverlaySortOrder(popup, 100));

  if (!panel_texture_.create(vulkan_, panel_width, panel_height, message) ||
      !popup_texture_.create(vulkan_, popup_width, popup_height, message)) {
    message = "textures: " + message;
    shutdown();
    return Connect::Error;
  }
  std::fprintf(stderr, "[VR] connected: dashboard panel \"%s\" and the request popup\n", kPanelName);
  return Connect::Ok;
}

void VrPrompt::shutdown() {
  if (!connected_) return;
  vr::IVROverlay* overlay = vr::VROverlay();
  for (uint64_t handle : {popup_, panel_, thumbnail_}) {
    if (handle == 0) continue;
    overlay->HideOverlay(handle);
    overlay->ClearOverlayTexture(handle);
  }
  if (popup_ != 0) overlay->DestroyOverlay(popup_);
  if (panel_ != 0) overlay->DestroyOverlay(panel_);
  popup_ = panel_ = thumbnail_ = 0;
  popup_shown_ = placed_ = following_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  vr::VR_Shutdown();
  panel_texture_.destroy();
  thumbnail_texture_.destroy();
  popup_texture_.destroy();
  vulkan_.destroy();
  connected_ = false;
}

VrInput VrPrompt::poll() {
  VrInput input;
  if (!connected_) return input;
  vr::VREvent_t event{};
  while (vr::VRSystem()->PollNextEvent(&event, sizeof event)) {
    if (event.eventType == vr::VREvent_Quit) {
      vr::VRSystem()->AcknowledgeQuit_Exiting();
      input.quit = true;
    }
  }
  const auto read = [&](uint64_t handle, int height, std::vector<PointerEvent>& out) {
    while (handle != 0 && vr::VROverlay()->PollNextOverlayEvent(handle, &event, sizeof event)) {
      const double x = event.data.mouse.x, y = height - event.data.mouse.y;
      switch (event.eventType) {
        case vr::VREvent_MouseMove:
          out.push_back({PointerEvent::Type::Move, x, y});
          break;
        case vr::VREvent_MouseButtonDown:
          if (event.data.mouse.button == vr::VRMouseButton_Left) out.push_back({PointerEvent::Type::Down, x, y});
          break;
        case vr::VREvent_MouseButtonUp:
          if (event.data.mouse.button == vr::VRMouseButton_Left) out.push_back({PointerEvent::Type::Up, x, y});
          break;
        case vr::VREvent_FocusLeave:
          out.push_back({PointerEvent::Type::Leave, 0, 0});
          break;
        default:
          break;
      }
    }
  };
  read(popup_, popup_height_, input.popup);
  read(panel_, panel_height_, input.panel);
  return input;
}

bool VrPrompt::panel_visible() const { return connected_ && panel_ != 0 && vr::VROverlay()->IsOverlayVisible(panel_); }

void VrPrompt::open_panel() {
  if (connected_) vr::VROverlay()->ShowDashboard(kPanelKey);
}

bool VrPrompt::set_panel(const uint8_t* rgba) {
  std::string error;
  if (!connected_ || !panel_texture_.update(panel_, rgba, error)) {
    if (!error.empty()) std::fprintf(stderr, "[VR] panel: %s\n", error.c_str());
    return false;
  }
  return true;
}

bool VrPrompt::set_thumbnail(const uint8_t* rgba, int size) {
  std::string error;
  if (!connected_) return false;
  if (!thumbnail_texture_.ready() && !thumbnail_texture_.create(vulkan_, size, size, error)) {
    std::fprintf(stderr, "[VR] thumbnail texture: %s\n", error.c_str());
    return false;
  }
  return thumbnail_texture_.update(thumbnail_, rgba, error);
}

bool VrPrompt::set_popup(const uint8_t* rgba) {
  std::string error;
  if (!connected_ || !popup_texture_.update(popup_, rgba, error)) {
    if (!error.empty()) std::fprintf(stderr, "[VR] popup: %s\n", error.c_str());
    return false;
  }
  return true;
}

void VrPrompt::show_popup() {
  if (!connected_ || popup_shown_) return;
  placed_ = false;
  follow(0);
  if (check("ShowOverlay(popup)", vr::VROverlay()->ShowOverlay(popup_))) popup_shown_ = true;
}

void VrPrompt::hide_popup() {
  if (!connected_ || !popup_shown_) return;
  vr::VROverlay()->HideOverlay(popup_);
  popup_shown_ = false;
}

void VrPrompt::follow(double dt) {
  if (!connected_ || popup_ == 0) return;
  vr::TrackedDevicePose_t pose{};
  vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
  if (!pose.bPoseIsValid) return;
  const V3 head = column(pose.mDeviceToAbsoluteTracking, 3);
  const V3 forward = column(pose.mDeviceToAbsoluteTracking, 2) * -1.0;
  const V3 target = head + normalized(forward) * kPopupDistanceM + V3{0, -kPopupDropM, 0};
  V3 position{position_[0], position_[1], position_[2]};
  if (!placed_) {
    position = target;
    placed_ = true;
    following_ = false;
  }
  const double off_angle = angle_between(position - head, target - head);
  const double off_distance = std::fabs(length(position - head) - kPopupDistanceM);
  if (!following_ && (off_angle > kFollowStartRad || off_distance > kFollowStartDistanceM)) following_ = true;
  if (following_) {
    const double a = dt > 0 ? 1.0 - std::exp(-dt / kFollowTimeS) : 1.0;
    position = position + (target - position) * a;
    if (angle_between(position - head, target - head) < kFollowStopRad &&
        std::fabs(length(position - head) - kPopupDistanceM) < 0.03)
      following_ = false;
  }
  position_[0] = position.x;
  position_[1] = position.y;
  position_[2] = position.z;
  const V3 z = normalized(head - position);
  const V3 x = normalized(cross(V3{0, 1, 0}, z));
  const V3 y = cross(z, x);
  vr::HmdMatrix34_t m{};
  const V3 axes[4] = {x, y, z, position};
  for (int c = 0; c < 4; ++c) {
    m.m[0][c] = float(axes[c].x);
    m.m[1][c] = float(axes[c].y);
    m.m[2][c] = float(axes[c].z);
  }
  vr::VROverlay()->SetOverlayTransformAbsolute(popup_, vr::TrackingUniverseStanding, &m);
}
