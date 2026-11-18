#include "server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>

#include "util.h"

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

namespace fca {
namespace {

constexpr int64_t kHandshakeTimeoutNs = 2'000'000'000;
constexpr size_t kMaxClients = 64;
constexpr size_t kMaxHandshakesPerUid = 8;
constexpr int64_t kRefusalLogIntervalNs = 600'000'000'000;
constexpr size_t kMaxRefusalSubjects = 256;

void send_welcome(int fd, int32_t result, uint64_t size, int memfd) {
  fca_welcome welcome{FCA_WELCOME_MAGIC, FCA_PROTOCOL_VERSION, result, 0, size};
  iovec iov{&welcome, sizeof welcome};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
  if (memfd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    cmsghdr* header = CMSG_FIRSTHDR(&msg);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &memfd, sizeof memfd);
  }
  sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
}

}  // namespace

Server::Server(const std::vector<std::string>& socket_names, std::vector<UidRange> allowed, Access* access)
    : allowed_(std::move(allowed)), access_(access) {
  if (allowed_.empty()) allowed_.push_back({geteuid(), geteuid()});
  try {
    for (const std::string& name : socket_names) listen_on(name);
  } catch (...) {
    for (int fd : listen_fds_) close(fd);
    for (const std::string& path : paths_) unlink(path.c_str());
    throw;
  }
}

void Server::listen_on(const std::string& socket_name) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  socklen_t address_length = 0;
  std::string path;
  if (!socket_name.empty() && socket_name[0] == '@') {
    if (socket_name.size() < 2 || socket_name.size() > sizeof address.sun_path)
      throw std::runtime_error("socket name too long");
    std::memcpy(address.sun_path + 1, socket_name.data() + 1, socket_name.size() - 1);
    address_length = socklen_t(offsetof(sockaddr_un, sun_path) + socket_name.size());
  } else {
    if (socket_name.empty() || socket_name.size() >= sizeof address.sun_path)
      throw std::runtime_error("invalid socket path");
    std::memcpy(address.sun_path, socket_name.data(), socket_name.size());
    address_length = socklen_t(offsetof(sockaddr_un, sun_path) + socket_name.size() + 1);
    path = socket_name;
    unlink(path.c_str());
  }
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) throw system_error("socket");
  listen_fds_.push_back(fd);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0)
    throw system_error("bind " + socket_name + " (is another frame-camera-accessd running?)");
  if (!path.empty()) {
    paths_.push_back(path);
    chmod(path.c_str(), 0666);
  }
  if (listen(fd, 16) != 0) throw system_error("listen");
}

Server::~Server() {
  for (const Client& client : clients_) {
    close(client.fd);
    if (client.pidfd >= 0) close(client.pidfd);
  }
  for (int fd : listen_fds_) close(fd);
  for (const std::string& path : paths_) unlink(path.c_str());
}

size_t Server::client_count() const {
  return size_t(std::count_if(clients_.begin(), clients_.end(), [](const Client& client) { return client.welcomed; }));
}

void Server::drop(size_t index) {
  Client& client = clients_[index];
  close(client.fd);
  if (client.pidfd >= 0) close(client.pidfd);
  if (access_ && client.welcomed && !client.app_key.empty()) access_->disconnected(client.app_key, now_raw_ns());
  clients_.erase(clients_.begin() + std::ptrdiff_t(index));
}

void Server::log_refusal(const std::string& subject, const std::string& message) {
  const int64_t now = now_raw_ns();
  const auto logged = refusal_logged_ns_.find(subject);
  if (logged != refusal_logged_ns_.end() && now - logged->second < kRefusalLogIntervalNs) return;
  if (refusal_logged_ns_.size() >= kMaxRefusalSubjects) refusal_logged_ns_.clear();
  refusal_logged_ns_[subject] = now;
  log("%s", message.c_str());
}

void Server::disconnect_all() {
  const size_t welcomed_count = client_count();
  for (size_t i = clients_.size(); i-- > 0;) drop(i);
  if (welcomed_count > 0) log("disconnected %zu clients", welcomed_count);
}

void Server::accept_clients(int listen_fd) {
  for (;;) {
    const int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    ucred credentials{};
    socklen_t credentials_length = sizeof credentials;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0) {
      close(fd);
      continue;
    }
    if (clients_.size() >= kMaxClients) {
      log_refusal("full", "refused a client: " + std::to_string(clients_.size()) + " clients connected");
      close(fd);
      continue;
    }
    const auto handshaking_count = std::count_if(clients_.begin(), clients_.end(), [&](const Client& client) {
      return !client.welcomed && client.uid == credentials.uid;
    });
    if (size_t(handshaking_count) >= kMaxHandshakesPerUid) {
      close(fd);
      continue;
    }
    const bool allowed = std::any_of(allowed_.begin(), allowed_.end(), [&](const UidRange& range) {
      return credentials.uid >= range.first && credentials.uid <= range.last;
    });
    int pidfd = -1;
    socklen_t pidfd_len = sizeof pidfd;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &pidfd_len) != 0) pidfd = -1;
    clients_.push_back({fd, credentials.uid, credentials.pid, pidfd, allowed, false, now_raw_ns(), {}});
  }
}

bool Server::handshake(Client& client, const SharedRing& ring) {
  fca_hello hello{};
  const ssize_t received = recv(client.fd, &hello, sizeof hello, MSG_DONTWAIT);
  if (received != ssize_t(sizeof hello) || hello.magic != FCA_HELLO_MAGIC) return false;
  if (!client.allowed) {
    const std::string uid = std::to_string(client.uid);
    log_refusal("uid:" + uid,
                "refused a client of uid " + uid + " (not an --allow-uid user or one of its container uids)");
    send_welcome(client.fd, FCA_WELCOME_NOT_ALLOWED, 0, -1);
    return false;
  }
  if (hello.version != FCA_PROTOCOL_VERSION) {
    send_welcome(client.fd, FCA_WELCOME_VERSION_MISMATCH, 0, -1);
    return false;
  }
  std::string label;
  if (access_) {
    AppIdentity app;
    if (!access_->identify(client.pid, client.pidfd, app)) return false;
    if (!app.refused.empty()) {
      log_refusal("unnamed:" + app.key,
                  "refused pid " + std::to_string(client.pid) + ": " + app.refused + " (" + app.key + ")");
      send_welcome(client.fd, FCA_WELCOME_NOT_ALLOWED, 0, -1);
      return false;
    }
    switch (access_->check(app, now_raw_ns())) {
      case Access::Verdict::Deny:
        log_refusal("denied:" + app.key, "refused " + app.label + " (denied by the user)");
        send_welcome(client.fd, FCA_WELCOME_NOT_ALLOWED, 0, -1);
        return false;
      case Access::Verdict::Pending:
        send_welcome(client.fd, FCA_WELCOME_PENDING, 0, -1);
        return false;
      case Access::Verdict::Allow:
        break;
    }
    client.app_key = app.key;
    label = app.label;
    access_->connected(app.key);
  }
  send_welcome(client.fd, FCA_WELCOME_OK, ring.size(), ring.readonly_fd());
  client.welcomed = true;
  log("client connected (%s, uid %u, %zu connected)", label.empty() ? "per-app permissions off" : label.c_str(),
      unsigned(client.uid), client_count());
  return true;
}

void Server::poll(int timeout_ms, const SharedRing& ring) {
  std::vector<pollfd> poll_fds;
  for (const Client& client : clients_) poll_fds.push_back({client.fd, POLLIN, 0});
  for (int fd : listen_fds_) poll_fds.push_back({fd, POLLIN, 0});
  if (::poll(poll_fds.data(), poll_fds.size(), timeout_ms) < 0) return;
  const int64_t now = now_raw_ns();
  const size_t polled_clients = clients_.size();
  for (size_t i = polled_clients; i-- > 0;) {
    Client& client = clients_[i];
    const short revents = poll_fds[i].revents;
    if (!client.welcomed) {
      if (revents & POLLIN) {
        if (!handshake(client, ring)) drop(i);
      } else if ((revents & (POLLHUP | POLLERR)) || now - client.accepted_ns > kHandshakeTimeoutNs) {
        drop(i);
      }
      continue;
    }
    if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
      drop(i);
      log("client disconnected (%zu connected)", client_count());
    } else if (revents & POLLIN) {
      char buffer[64];
      if (recv(client.fd, buffer, sizeof buffer, MSG_DONTWAIT) == 0) {
        drop(i);
        log("client disconnected (%zu connected)", client_count());
      }
    }
  }
  for (size_t i = 0; i < listen_fds_.size(); ++i)
    if (poll_fds[polled_clients + i].revents & POLLIN) accept_clients(listen_fds_[i]);
}

}  // namespace fca
