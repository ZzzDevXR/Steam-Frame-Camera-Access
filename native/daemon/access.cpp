#include "access.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "util.h"

#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

namespace fca {
namespace {

constexpr int64_t kPendingLifetimeNs = 60'000'000'000;
constexpr int64_t kOnceGraceNs = 30'000'000'000;
constexpr size_t kMaxPending = 8;
constexpr size_t kMaxNameChars = 64, kMaxWhereChars = 120;
constexpr size_t kMaxContainerListBytes = 16u << 20;

std::string read_link(const std::string& path) {
  char buffer[4096];
  const ssize_t length = readlink(path.c_str(), buffer, sizeof buffer - 1);
  if (length <= 0) return {};
  std::string target(buffer, size_t(length));
  const std::string deleted = " (deleted)";
  if (target.size() > deleted.size() && target.compare(target.size() - deleted.size(), deleted.size(), deleted) == 0)
    target.resize(target.size() - deleted.size());
  return target;
}

std::string basename_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string sanitize(const std::string& text, size_t max = 200) {
  std::string out;
  for (char c : text) {
    if (out.size() >= max) break;
    out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
  }
  return trim(out);
}

std::string sanitize_label_part(const std::string& text, size_t max) {
  std::string out;
  for (char c : text) {
    if (out.size() >= max) break;
    const auto byte = static_cast<unsigned char>(c);
    out += c == '(' ? '[' : c == ')' ? ']' : (byte < 0x20 || byte > 0x7e) ? '?' : c;
  }
  return trim(out);
}

bool is_android_process_name(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == ':' || c == '/' || c == '+' || c == '-';
  });
}

std::string first_argument(const std::string& cmdline) { return cmdline.substr(0, cmdline.find('\0')); }

bool is_interpreter(const std::string& exe) {
  const std::string name = basename_of(exe);
  for (const std::string base : {"python", "node", "perl", "ruby", "bash", "sh", "dash", "zsh", "lua"}) {
    if (name.compare(0, base.size(), base) != 0) continue;
    const std::string version = name.substr(base.size());
    if (version.find_first_not_of("0123456789.") == std::string::npos) return true;
  }
  return false;
}

std::string interpreted_program(const std::string& exe, const std::string& cmdline, const std::string& cwd) {
  const std::string name = basename_of(exe);
  const bool python = name.compare(0, 6, "python") == 0;
  bool shell = false;
  for (const std::string base : {"bash", "sh", "dash", "zsh"}) shell = shell || name.compare(0, base.size(), base) == 0;
  const char code_flag = python || shell ? 'c' : 'e';
  const char* value_flags = python ? "WX" : shell ? "o" : "Ir";
  std::vector<std::string> arguments;
  for (size_t at = 0; at < cmdline.size();) {
    const size_t end = cmdline.find('\0', at);
    arguments.push_back(cmdline.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos) break;
    at = end + 1;
  }
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "-m" && i + 1 < arguments.size()) return "-m " + arguments[i + 1];
    if (argument.size() >= 2 && argument[0] == '-' && argument[1] != '-' && argument.back() == code_flag &&
        std::strchr(value_flags, argument[1]) == nullptr)
      return {};
    if (argument == "--eval" || argument == "--print" || argument == "-p") return {};
    if (argument == "-W" || argument == "-X") {
      ++i;
      continue;
    }
    if (argument.empty() || argument[0] == '-') continue;
    return argument[0] == '/' || cwd.empty() ? argument : cwd + "/" + argument;
  }
  return {};
}

bool parse_decision(const std::string& text, Access::Decision& decision) {
  if (text == "allow")
    decision = Access::Decision::Allow;
  else if (text == "deny")
    decision = Access::Decision::Deny;
  else
    return false;
  return true;
}

}  // namespace

const char* decision_name(Access::Decision decision) {
  switch (decision) {
    case Access::Decision::Allow:
      return "allow";
    case Access::Decision::Once:
      return "once";
    case Access::Decision::Deny:
      return "deny";
    default:
      return "pending";
  }
}

Access::Access(std::string policy_path, std::vector<std::string> homes)
    : path_(std::move(policy_path)), homes_(std::move(homes)), host_mnt_(host_mount_namespace()) {
  reload();
}

void Access::reload() {
  std::map<std::string, std::pair<Decision, std::string>> saved_decisions;
  std::ifstream permission_file(path_);
  std::string line;
  while (std::getline(permission_file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t first_tab = line.find('\t'),
                 second_tab = first_tab == std::string::npos ? first_tab : line.find('\t', first_tab + 1);
    Decision decision;
    if (first_tab == std::string::npos || !parse_decision(line.substr(0, first_tab), decision)) continue;
    const std::string key =
        line.substr(first_tab + 1, second_tab == std::string::npos ? std::string::npos : second_tab - first_tab - 1);
    saved_decisions[key] = {decision, second_tab == std::string::npos ? key : line.substr(second_tab + 1)};
  }
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& entry = it->second;
    if (entry.saved && saved_decisions.count(it->first) == 0) {
      end_access(entry, it->first);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [key, value] : saved_decisions) {
    Entry& entry = entries_[key];
    if (value.first == Decision::Deny) end_access(entry, key);
    entry.decision = value.first;
    entry.label = value.second;
    entry.saved = true;
    entry.expires_ns = 0;
  }
  changed_ = true;
  log("permissions: %zu saved decisions (%s)", saved_decisions.size(), path_.c_str());
}

void Access::save() const {
  const std::string temporary_path = path_ + ".tmp";
  std::ofstream permission_file(temporary_path, std::ios::trunc);
  if (permission_file) {
    chmod(temporary_path.c_str(), 0600);
    permission_file
        << "# frame-camera-accessd app permissions: decision<TAB>app<TAB>label. Edit, then: systemctl kill -s HUP "
           "frame-camera-access\n";
    for (const auto& [key, entry] : entries_)
      if (entry.saved) permission_file << decision_name(entry.decision) << '\t' << key << '\t' << entry.label << '\n';
    permission_file.close();
  }
  if (!permission_file) {
    log("cannot write the permission file %s", temporary_path.c_str());
    unlink(temporary_path.c_str());
    return;
  }
  if (rename(temporary_path.c_str(), path_.c_str()) != 0) log("cannot replace the permission file %s", path_.c_str());
}

void Access::end_access(Entry& entry, const std::string& key) {
  if (entry.holds_ring) revoked_.push_back(key);
  entry.holds_ring = false;
}

std::string Access::container_name(const std::string& id) const {
  for (const std::string& home : homes_) {
    std::string container_list;
    if (!read_user_file(home + "/.local/share/containers/storage/overlay-containers/containers.json",
                        kMaxContainerListBytes, container_list))
      continue;
    const size_t entry_at = container_list.find("\"id\":\"" + id + "\"");
    if (entry_at == std::string::npos) continue;
    const size_t next_entry_at = container_list.find("\"id\":\"", entry_at + 6);
    const std::string marker = "\"names\":[\"";
    const size_t names_at = container_list.find(marker, entry_at);
    if (names_at == std::string::npos || (next_entry_at != std::string::npos && names_at > next_entry_at)) continue;
    const size_t name_start = names_at + marker.size(), name_end = container_list.find('"', name_start);
    if (name_end != std::string::npos) return container_list.substr(name_start, name_end - name_start);
  }
  return {};
}

bool Access::identify(pid_t pid, int pidfd, AppIdentity& out) const {
  const std::string proc_dir = "/proc/" + std::to_string(pid);
  std::string cgroup, cmdline;
  read_text(proc_dir + "/cgroup", cgroup);
  read_text(proc_dir + "/cmdline", cmdline);
  const std::string exe = read_link(proc_dir + "/exe");
  const std::string process = sanitize(first_argument(cmdline));

  std::string container;
  const size_t libpod_at = cgroup.find("libpod-");
  if (libpod_at != std::string::npos) {
    const size_t start = libpod_at + 7, end = cgroup.find(".scope", start);
    if (end != std::string::npos) container = cgroup.substr(start, end - start);
  }
  if (!container.empty()) {
    const std::string name = container_name(container);
    if (name.compare(0, 7, "lepton-") == 0) {
      const std::string context = name.substr(7);
      const std::string package = process.empty() ? basename_of(exe) : process;
      if (!is_android_process_name(package)) out.refused = "an Android process whose name is not a package name";
      out.key = sanitize("lepton:" + context + ":" + package, 400);
      std::string where = "Lepton " + context;
      if (context.compare(0, 12, "steamlaunch-") == 0) {
        const std::string id = context.substr(12);
        where = id.size() >= 10 && std::strtoull(id.c_str(), nullptr, 10) >= 0x80000000ull ? "Steam library shortcut"
                                                                                           : "Steam app " + id;
      }
      out.label = sanitize_label_part(package, kMaxNameChars) + " (" + sanitize_label_part(where, kMaxWhereChars) + ")";
    } else {
      const std::string shown = name.empty() ? container.substr(0, 12) : name;
      out.key = sanitize("container:" + shown + ":" + exe, 400);
      out.label = sanitize_label_part(basename_of(exe), kMaxNameChars) + " (" +
                  sanitize_label_part("container " + shown, kMaxWhereChars) + ")";
    }
  } else {
    const ino_t peer_mount_ns = mount_namespace(std::to_string(pid));
    const bool on_host = peer_mount_ns != 0 && peer_mount_ns == host_mnt_;
    const std::string prefix = on_host ? "exe:" : "sandbox:", sandboxed = on_host ? "" : "sandboxed, ";
    if (is_interpreter(exe)) {
      const std::string program = interpreted_program(exe, cmdline, read_link(proc_dir + "/cwd"));
      if (program.empty()) out.refused = "an interpreter running inline code (-c, -e or standard input)";
      const bool is_module = program.compare(0, 3, "-m ") == 0;
      out.key = sanitize(prefix + exe + " " + program, 400);
      out.label =
          sanitize_label_part(is_module ? program.substr(3) : basename_of(program), kMaxNameChars) + " (" +
          sanitize_label_part(sandboxed + basename_of(exe) + (is_module ? "" : ", " + program), kMaxWhereChars) + ")";
    } else {
      out.key = sanitize(prefix + exe, 400);
      out.label = sanitize_label_part(basename_of(exe), kMaxNameChars) + " (" +
                  sanitize_label_part(sandboxed + exe, kMaxWhereChars) + ")";
    }
  }
  if (exe.empty() && process.empty()) return false;
  if (pidfd >= 0 && syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) != 0 && errno == ESRCH) return false;
  return true;
}

Access::Verdict Access::check(const AppIdentity& app, int64_t now_ns) {
  auto it = entries_.find(app.key);
  if (it != entries_.end()) {
    Entry& entry = it->second;
    switch (entry.decision) {
      case Decision::Allow:
        return Verdict::Allow;
      case Decision::Deny:
        return Verdict::Deny;
      case Decision::Once:
        if (entry.connections > 0 || entry.expires_ns == 0 || now_ns < entry.expires_ns) return Verdict::Allow;
        end_access(entry, it->first);
        entries_.erase(it);
        changed_ = true;
        break;
      case Decision::Pending:
        entry.expires_ns = now_ns + kPendingLifetimeNs;
        return Verdict::Pending;
    }
  }
  const auto pending_count = std::count_if(
      entries_.begin(), entries_.end(), [](const auto& entry) { return entry.second.decision == Decision::Pending; });
  if (size_t(pending_count) >= kMaxPending) return Verdict::Pending;
  Entry& entry = entries_[app.key];
  entry.label = app.label;
  entry.decision = Decision::Pending;
  entry.expires_ns = now_ns + kPendingLifetimeNs;
  new_requests_.push_back(app);
  changed_ = true;
  log("app %s asks for the cameras: waiting for the user (%s)", app.label.c_str(), app.key.c_str());
  return Verdict::Pending;
}

void Access::connected(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  ++it->second.connections;
  it->second.holds_ring = true;
  if (it->second.decision == Decision::Once) it->second.expires_ns = 0;
  changed_ = true;
}

void Access::disconnected(const std::string& key, int64_t now_ns) {
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.connections == 0) return;
  if (--it->second.connections == 0 && it->second.decision == Decision::Once)
    it->second.expires_ns = now_ns + kOnceGraceNs;
  changed_ = true;
}

void Access::decide(const std::string& key, const std::string& decision, int64_t now_ns) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  Entry& entry = it->second;
  bool allowed = false;
  if (decision == "allow") {
    entry.decision = Decision::Allow;
    entry.saved = true;
    allowed = true;
  } else if (decision == "once") {
    entry.decision = Decision::Once;
    entry.saved = false;
    entry.expires_ns = entry.connections > 0 ? 0 : now_ns + kOnceGraceNs;
    allowed = true;
  } else if (decision == "deny") {
    entry.decision = Decision::Deny;
    entry.saved = true;
  } else if (decision == "forget") {
    end_access(entry, key);
    const bool saved = entry.saved;
    log("permissions: forgot %s", entry.label.c_str());
    entries_.erase(it);
    if (saved) save();
    changed_ = true;
    return;
  } else {
    return;
  }
  if (!allowed) end_access(entry, key);
  log("permissions: %s %s", decision_name(entry.decision), entry.label.c_str());
  save();
  changed_ = true;
}

void Access::expire(int64_t now_ns) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& entry = it->second;
    const bool lapsed = entry.expires_ns != 0 && now_ns > entry.expires_ns && entry.connections == 0 &&
                        (entry.decision == Decision::Pending || entry.decision == Decision::Once);
    if (lapsed) {
      end_access(entry, it->first);
      it = entries_.erase(it);
      changed_ = true;
    } else {
      ++it;
    }
  }
}

std::vector<Access::App> Access::apps() const {
  std::vector<App> list;
  for (const auto& [key, entry] : entries_) list.push_back({key, entry.label, entry.decision, entry.connections});
  std::stable_sort(list.begin(), list.end(), [](const App& left, const App& right) {
    return (left.decision == Decision::Pending) > (right.decision == Decision::Pending);
  });
  return list;
}

std::vector<AppIdentity> Access::take_new_requests() {
  std::vector<AppIdentity> out;
  out.swap(new_requests_);
  return out;
}

bool Access::take_changed() {
  const bool was_changed = changed_;
  changed_ = false;
  return was_changed;
}

std::vector<std::string> Access::take_revoked() {
  std::vector<std::string> out;
  out.swap(revoked_);
  return out;
}

void Access::ring_replaced() {
  for (auto& [key, entry] : entries_) entry.holds_ring = false;
}

}  // namespace fca
