#include "control_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

std::vector<std::string> split(const std::string& line, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t end = line.find(separator, start);
    parts.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}

}  // namespace

ControlClient::~ControlClient() { disconnect(); }

bool ControlClient::connect(const std::string& socket_name, std::string& error) {
  disconnect();
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_name.size() < 2 || socket_name[0] != '@' || socket_name.size() > sizeof address.sun_path) {
    error = "bad control socket name " + socket_name;
    return false;
  }
  std::memcpy(address.sun_path + 1, socket_name.data() + 1, socket_name.size() - 1);
  const socklen_t address_length = socklen_t(offsetof(sockaddr_un, sun_path) + socket_name.size());
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
    error = std::string("the camera service is not running or has app permissions off (") + std::strerror(errno) + ")";
    close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

void ControlClient::disconnect() {
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
}

bool ControlClient::poll(DaemonState& state, std::vector<std::pair<std::string, std::string>>& requests) {
  if (fd_ < 0) return false;
  bool any = false;
  std::vector<char> buffer(256 * 1024);
  for (;;) {
    const ssize_t received = recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      disconnect();
      state.connected = false;
      return true;
    }
    if (received < 0) break;
    any = true;
    const std::string message(buffer.data(), size_t(received));
    if (message.compare(0, 5, "list\n") == 0) {
      state.apps.clear();
      std::istringstream lines(message.substr(5));
      std::string line;
      while (std::getline(lines, line)) {
        const std::vector<std::string> fields = split(line, '\t');
        if (fields.size() >= 3 && fields[0] == "state") {
          state.state = fields[1];
          state.status = fields[2];
        } else if (fields.size() >= 4) {
          state.apps.push_back({fields[0], fields[1], fields[2], std::atoi(fields[3].c_str())});
        }
      }
      state.connected = true;
    } else if (message.compare(0, 8, "request\t") == 0) {
      const std::vector<std::string> fields = split(message, '\t');
      if (fields.size() >= 3) requests.emplace_back(fields[1], fields[2]);
    }
  }
  return any;
}

bool ControlClient::send_decision(const std::string& decision, const std::string& key) {
  if (fd_ < 0) return false;
  const std::string message = "set\t" + decision + "\t" + key;
  return send(fd_, message.data(), message.size(), MSG_NOSIGNAL) == ssize_t(message.size());
}
