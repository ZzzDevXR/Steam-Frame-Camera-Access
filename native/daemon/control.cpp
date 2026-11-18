#include "control.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>

#include "util.h"

namespace fca {
namespace {

std::string read_link(const std::string& path) {
  char buffer[4096];
  const ssize_t length = readlink(path.c_str(), buffer, sizeof buffer - 1);
  return length > 0 ? std::string(buffer, size_t(length)) : std::string();
}

void send_text(int fd, const std::string& text) { send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }

}  // namespace

ControlServer::ControlServer(const std::string& socket_name, std::vector<uid_t> users, std::string prompt_exe)
    : users_(std::move(users)), prompt_exe_(std::move(prompt_exe)) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_name.size() < 2 || socket_name[0] != '@' || socket_name.size() > sizeof address.sun_path)
    throw std::runtime_error("the control socket must be an abstract name (@name)");
  std::memcpy(address.sun_path + 1, socket_name.data() + 1, socket_name.size() - 1);
  const socklen_t address_length = socklen_t(offsetof(sockaddr_un, sun_path) + socket_name.size());
  listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) throw system_error("control socket");
  if (bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), address_length) != 0 ||
      listen(listen_fd_, 4) != 0) {
    close(listen_fd_);
    throw system_error("bind " + socket_name);
  }
  log("permission prompt socket %s (prompt: %s)", socket_name.c_str(), prompt_exe_.c_str());
}

ControlServer::~ControlServer() {
  for (int fd : prompts_) close(fd);
  if (listen_fd_ >= 0) close(listen_fd_);
}

bool ControlServer::is_trusted_prompt(int fd) const {
  ucred credentials{};
  socklen_t credentials_length = sizeof credentials;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0) return false;
  const std::string pid = std::to_string(credentials.pid);
  if (std::find(users_.begin(), users_.end(), credentials.uid) == users_.end()) {
    log("refused a prompt connection: uid %u is not an allowed user", unsigned(credentials.uid));
    return false;
  }
  const ino_t host_mount_ns = host_mount_namespace();
  const ino_t peer_mount_ns = mount_namespace(pid);
  if (host_mount_ns == 0 || peer_mount_ns != host_mount_ns) {
    log("refused a prompt connection from pid %s: not on the host (a container?)", pid.c_str());
    return false;
  }
  const std::string exe = read_link("/proc/" + pid + "/exe");
  if (exe != prompt_exe_) {
    log("refused a prompt connection from %s: only %s may answer permission requests", exe.c_str(),
        prompt_exe_.c_str());
    return false;
  }
  return true;
}

void ControlServer::send_list(int fd, const Access& access, const std::string& state, const std::string& status) const {
  std::string text = "list\n";
  for (const Access::App& app : access.apps())
    text += std::string(decision_name(app.decision)) + '\t' + app.key + '\t' + app.label + '\t' +
            std::to_string(app.connections) + '\n';
  text += "state\t" + state + '\t' + status + '\n';
  send_text(fd, text);
}

void ControlServer::poll(Access& access, const std::string& state, const std::string& status, int64_t now_ns) {
  for (;;) {
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) break;
    if (!is_trusted_prompt(fd)) {
      close(fd);
      continue;
    }
    prompts_.push_back(fd);
    log("permission prompt connected (%zu)", prompts_.size());
    send_list(fd, access, state, status);
    for (const Access::App& app : access.apps())
      if (app.decision == Access::Decision::Pending) send_text(fd, "request\t" + app.key + '\t' + app.label);
  }
  for (size_t i = prompts_.size(); i-- > 0;) {
    char buffer[1024];
    bool disconnected = false;
    for (;;) {
      const ssize_t received = recv(prompts_[i], buffer, sizeof buffer - 1, MSG_DONTWAIT);
      if (received > 0) {
        const std::string message(buffer, size_t(received));
        const size_t first_tab = message.find('\t'),
                     second_tab = first_tab == std::string::npos ? first_tab : message.find('\t', first_tab + 1);
        if (message.compare(0, first_tab, "set") == 0 && second_tab != std::string::npos)
          access.decide(message.substr(second_tab + 1), message.substr(first_tab + 1, second_tab - first_tab - 1),
                        now_ns);
        continue;
      }
      disconnected = received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
      break;
    }
    if (disconnected) {
      close(prompts_[i]);
      prompts_.erase(prompts_.begin() + std::ptrdiff_t(i));
      log("permission prompt disconnected (%zu)", prompts_.size());
    }
  }
  const std::vector<AppIdentity> requests = access.take_new_requests();
  const bool changed = access.take_changed() || state != last_state_ || status != last_status_;
  last_state_ = state;
  last_status_ = status;
  for (int fd : prompts_) {
    for (const AppIdentity& app : requests) send_text(fd, "request\t" + app.key + '\t' + app.label);
    if (changed) send_list(fd, access, state, status);
  }
  if (!requests.empty() && prompts_.empty())
    log("no permission prompt is running (frame-camera-access-prompt); the app waits until one answers, or until "
        "it is added to the permission file");
}

}  // namespace fca
