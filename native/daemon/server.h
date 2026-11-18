#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "access.h"
#include "shared_ring.h"

namespace fca {

struct UidRange {
  uid_t first, last;
};

class Server {
 public:
  Server(const std::vector<std::string>& socket_names, std::vector<UidRange> allowed, Access* access = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void poll(int timeout_ms, const SharedRing& ring);
  size_t client_count() const;
  void disconnect_all();

 private:
  struct Client {
    int fd;
    uid_t uid;
    pid_t pid;
    int pidfd;
    bool allowed;
    bool welcomed;
    int64_t accepted_ns;
    std::string app_key;
  };
  void listen_on(const std::string& socket_name);
  void accept_clients(int listen_fd);
  bool handshake(Client& client, const SharedRing& ring);
  void drop(size_t index);
  void log_refusal(const std::string& subject, const std::string& message);

  std::map<std::string, int64_t> refusal_logged_ns_;
  std::vector<int> listen_fds_;
  std::vector<std::string> paths_;
  std::vector<UidRange> allowed_;
  Access* access_ = nullptr;
  std::vector<Client> clients_;
};

}  // namespace fca
