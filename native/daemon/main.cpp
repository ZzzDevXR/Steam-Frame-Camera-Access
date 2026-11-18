#include <pwd.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <memory>

#include "access.h"
#include "control.h"
#include "fca.h"
#include "frame_dma_source.h"
#include "server.h"
#include "shared_ring.h"
#include "synthetic_source.h"
#include "util.h"

namespace {

volatile std::sig_atomic_t g_stop = 0, g_reload = 0;
void on_stop_signal(int) { g_stop = 1; }
void on_reload_signal(int) { g_reload = 1; }

#define DEFAULT_POLICY "/var/lib/frame-camera-access/state/permissions"
#define DEFAULT_PROMPT "/var/lib/frame-camera-access/bin/frame-camera-access-prompt"

const char kUsage[] =
    "usage: frame-camera-accessd [options]\n"
    "  --source frame|synthetic  frame: the headset's tracking cameras (default); synthetic: test pattern\n"
    "  --socket NAME             listening socket, '@name' = abstract (default " FCA_DEFAULT_SOCKET
    ")\n"
    "  --allow-uid UID           a user allowed to connect; repeatable (default: the daemon's own uid)\n"
    "  --slots N                 frame sets kept in the ring, 2-8 (default 4)\n"
    "  --phase bright|all        bright: skip short-exposure sets (default); all: publish them flagged dark\n"
    "  --settle-ms MS|off        take a set MS after its end of frame (default 2); off: wait for a newer frame\n"
    "  --min-mean V              sampled slam_right mean below V is a dark set (default 10)\n"
    "  --xrservice-log PATH      XRService log (default: ~/.local/share/Steam/logs/xrservice.txt of the first\n"
    "                            --allow-uid user)\n"
    "  --app-permissions on|off  ask the user before each app may read the cameras (default on);\n"
    "                            frame-camera-access-prompt asks in the headset. off (every process of an\n"
    "                            allowed user may read) only with --source synthetic, for tests\n"
    "  --policy PATH             the user's decisions (default " DEFAULT_POLICY
    ")\n"
    "  --prompt PATH             the only program allowed to answer (default " DEFAULT_PROMPT
    ")\n"
    "  --control-socket NAME     the prompt's socket (default @frame-camera-access-control)\n"
    "  --fps F                   synthetic source rate (default 33)\n"
    "  --run-for SECONDS         exit after this long (tests; default: until SIGTERM)\n";

struct Options {
  std::string source = "frame", phase = "bright", xrservice_log;
  bool app_permissions = true;
  std::string policy = DEFAULT_POLICY, prompt = DEFAULT_PROMPT, control_socket = "@frame-camera-access-control";
  std::vector<std::string> sockets;
  std::vector<uid_t> allowed;
  bool container_uids = true;
  int slots = 4;
  double settle_ms = 2, min_mean = 10, fps = 33, run_for = 0;
};

[[noreturn]] void usage_error(const std::string& message) {
  std::fprintf(stderr, "frame-camera-accessd: %s\n%s", message.c_str(), kUsage);
  std::exit(2);
}

double parse_number(const std::string& flag, const std::string& value, double minimum, double maximum) {
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (value.empty() || *end != '\0' || !(parsed >= minimum && parsed <= maximum))
    usage_error(flag + " needs a number in [" + std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
  return parsed;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "-h" || flag == "--help") {
      std::fputs(kUsage, stdout);
      std::exit(0);
    }
    if (i + 1 >= argc) usage_error("missing value for " + flag);
    const std::string value = argv[++i];
    if (flag == "--source")
      options.source = value;
    else if (flag == "--socket")
      options.sockets.push_back(value);
    else if (flag == "--allow-uid")
      options.allowed.push_back(uid_t(parse_number(flag, value, 0, 4294967294.0)));
    else if (flag == "--container-uids") {
      if (value != "on" && value != "off") usage_error("--container-uids must be on or off");
      options.container_uids = value == "on";
    } else if (flag == "--app-permissions") {
      if (value != "on" && value != "off") usage_error("--app-permissions must be on or off");
      options.app_permissions = value == "on";
    } else if (flag == "--policy")
      options.policy = value;
    else if (flag == "--prompt")
      options.prompt = value;
    else if (flag == "--control-socket")
      options.control_socket = value;
    else if (flag == "--slots")
      options.slots = int(parse_number(flag, value, 2, FCA_MAX_SLOTS));
    else if (flag == "--phase")
      options.phase = value;
    else if (flag == "--settle-ms")
      options.settle_ms = value == "off" ? -1 : parse_number(flag, value, 0, 50);
    else if (flag == "--min-mean")
      options.min_mean = parse_number(flag, value, 0, 255);
    else if (flag == "--xrservice-log")
      options.xrservice_log = value;
    else if (flag == "--fps")
      options.fps = parse_number(flag, value, 1, 2000);
    else if (flag == "--run-for")
      options.run_for = parse_number(flag, value, 0, 1e9);
    else
      usage_error("unknown option " + flag);
  }
  if (options.sockets.empty()) options.sockets = {FCA_DEFAULT_SOCKET, FCA_DEFAULT_PATH_SOCKET};
  if (options.source != "frame" && options.source != "synthetic") usage_error("--source must be frame or synthetic");
  if (options.phase != "bright" && options.phase != "all") usage_error("--phase must be bright or all");
  if (!options.app_permissions && options.source != "synthetic")
    usage_error("--app-permissions off is only for --source synthetic: no app sees the cameras without the user");
  if (options.xrservice_log.empty()) {
    const passwd* account = getpwuid(options.allowed.empty() ? geteuid() : options.allowed.front());
    if (account && account->pw_dir)
      options.xrservice_log = std::string(account->pw_dir) + "/.local/share/Steam/logs/xrservice.txt";
  }
  return options;
}

std::unique_ptr<fca::FrameSource> make_source(const Options& options) {
  if (options.source == "synthetic") return std::make_unique<fca::SyntheticSource>(options.fps);
  fca::FrameDmaSource::Options source_options;
  source_options.settle_ms = options.settle_ms;
  source_options.bright_only = options.phase == "bright";
  source_options.min_mean = options.min_mean;
  source_options.xrservice_log = options.xrservice_log;
  return std::make_unique<fca::FrameDmaSource>(source_options);
}

std::vector<fca::UidRange> allowed_ranges(const Options& options) {
  std::vector<uid_t> users = options.allowed;
  if (users.empty()) users.push_back(geteuid());
  std::vector<fca::UidRange> ranges;
  for (uid_t uid : users) {
    ranges.push_back({uid, uid});
    if (!options.container_uids) continue;
    const passwd* account = getpwuid(uid);
    const std::string name = account && account->pw_name ? account->pw_name : "", uid_text = std::to_string(uid);
    std::ifstream subuid("/etc/subuid");
    std::string line;
    while (std::getline(subuid, line)) {
      const size_t first_colon = line.find(':'),
                   second_colon = first_colon == std::string::npos ? first_colon : line.find(':', first_colon + 1);
      if (second_colon == std::string::npos) continue;
      const std::string owner = line.substr(0, first_colon);
      if (owner != uid_text && (name.empty() || owner != name)) continue;
      char* end = nullptr;
      const unsigned long first = std::strtoul(line.c_str() + first_colon + 1, &end, 10);
      const unsigned long count = std::strtoul(line.c_str() + second_colon + 1, &end, 10);
      if (count == 0 || first == 0 || first + count - 1 > 4294967294UL) continue;
      ranges.push_back({uid_t(first), uid_t(first + count - 1)});
      fca::log("allowing container uids %lu-%lu of uid %u (/etc/subuid)", first, first + count - 1, unsigned(uid));
    }
  }
  return ranges;
}

std::vector<uid_t> users_of(const Options& options) {
  std::vector<uid_t> users = options.allowed;
  if (users.empty()) users.push_back(geteuid());
  return users;
}

std::vector<std::string> homes_of(const std::vector<uid_t>& users) {
  std::vector<std::string> homes;
  for (uid_t uid : users)
    if (const passwd* account = getpwuid(uid); account && account->pw_dir) homes.push_back(account->pw_dir);
  return homes;
}

const char* state_name(uint32_t state) {
  switch (state) {
    case FCA_STATE_STARTING:
      return "starting";
    case FCA_STATE_UNAVAILABLE:
      return "unavailable";
    case FCA_STATE_WAITING:
      return "waiting";
    case FCA_STATE_STREAMING:
      return "streaming";
    default:
      return "stopped";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  struct sigaction stop_action{};
  stop_action.sa_handler = on_stop_signal;
  sigaction(SIGINT, &stop_action, nullptr);
  sigaction(SIGTERM, &stop_action, nullptr);
  struct sigaction reload_action{};
  reload_action.sa_handler = on_reload_signal;
  sigaction(SIGHUP, &reload_action, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  try {
    std::unique_ptr<fca::FrameSource> source = make_source(options);
    auto ring = std::make_unique<fca::SharedRing>(source->layout(), uint32_t(options.slots), source->kind());
    std::unique_ptr<fca::Access> access;
    std::unique_ptr<fca::ControlServer> control;
    if (options.app_permissions) {
      access = std::make_unique<fca::Access>(options.policy, homes_of(users_of(options)));
      control = std::make_unique<fca::ControlServer>(options.control_socket, users_of(options), options.prompt);
    }
    fca::Server server(options.sockets, allowed_ranges(options), access.get());
    std::string listening_on;
    for (const std::string& socket_name : options.sockets)
      listening_on += (listening_on.empty() ? "" : ", ") + socket_name;
    fca::log("%s source, listening on %s (protocol %u)", source->kind(), listening_on.c_str(), FCA_PROTOCOL_VERSION);

    const int64_t started = fca::now_raw_ns();
    int64_t retry_at = 0, last_frame = 0, last_heartbeat = 0, last_summary = started;
    uint64_t sets_at_summary = 0, sets = 0;
    bool source_open = false;
    std::string last_status;
    uint32_t last_state = FCA_STATE_STARTING;
    auto set_state = [&](uint32_t state, const std::string& status) {
      if (ring->state() == state && status == last_status) return;
      ring->set_state(state, status);
      fca::log("%s: %s", state_name(state), status.c_str());
      last_status = status;
      last_state = state;
    };
    auto rotate_ring = [&]() {
      if (ring->writing()) ring->abort();
      ring->set_state(FCA_STATE_STOPPED, "camera access revoked; reconnect");
      server.disconnect_all();
      ring = std::make_unique<fca::SharedRing>(source->layout(), uint32_t(options.slots), source->kind());
      ring->set_state(last_state, last_status);
      if (access) access->ring_replaced();
      fca::log("access revoked: new shared memory, clients reconnect");
    };

    while (!g_stop) {
      const int64_t now = fca::now_raw_ns();
      if (options.run_for > 0 && now - started > int64_t(options.run_for * 1e9)) break;
      server.poll(source_open ? 0 : 50, *ring);
      if (access) {
        if (g_reload) {
          g_reload = 0;
          access->reload();
        }
        access->expire(now);
        control->poll(*access, state_name(last_state), last_status, now);
        if (!access->take_revoked().empty()) rotate_ring();
      }
      if (now - last_heartbeat > 50'000'000) {
        ring->heartbeat();
        last_heartbeat = now;
      }
      if (!source_open) {
        if (now < retry_at) continue;
        try {
          source->open();
          source_open = true;
          last_frame = now;
          set_state(FCA_STATE_WAITING, "camera source open, waiting for frames");
        } catch (const std::exception& error) {
          set_state(FCA_STATE_UNAVAILABLE, error.what());
          retry_at = now + 2'000'000'000;
        }
        continue;
      }
      try {
        if (source->next(*ring, 2)) {
          ++sets;
          last_frame = fca::now_raw_ns();
          if (ring->state() != FCA_STATE_STREAMING) set_state(FCA_STATE_STREAMING, "streaming");
        } else if (ring->state() == FCA_STATE_STREAMING && now - last_frame > 1'000'000'000) {
          set_state(FCA_STATE_WAITING, "no new camera frames for 1 s (headset in standby?)");
        }
        const fca::SourceCounters counters = source->counters();
        ring->set_counters(counters.copy_races, counters.invalid_metadata, counters.dark_skipped);
      } catch (const std::exception& error) {
        if (ring->writing()) ring->abort();
        source->close();
        source_open = false;
        set_state(FCA_STATE_UNAVAILABLE, error.what());
        retry_at = now + 2'000'000'000;
      }
      if (now - last_summary > 60'000'000'000LL) {
        const fca::SourceCounters counters = source->counters();
        fca::log("%.1f sets/s, %zu clients, copy races %llu, invalid metadata %llu, dark skipped %llu",
                 double(sets - sets_at_summary) * 1e9 / double(now - last_summary), server.client_count(),
                 (unsigned long long)counters.copy_races, (unsigned long long)counters.invalid_metadata,
                 (unsigned long long)counters.dark_skipped);
        last_summary = now;
        sets_at_summary = sets;
      }
    }
    ring->set_state(FCA_STATE_STOPPED, "daemon stopped");
    source->close();
    fca::log("stopped after %llu sets", (unsigned long long)sets);
  } catch (const std::exception& error) {
    fca::log("fatal: %s", error.what());
    return 1;
  }
  return 0;
}
