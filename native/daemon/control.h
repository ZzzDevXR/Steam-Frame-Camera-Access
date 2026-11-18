#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "access.h"

namespace fca {

class ControlServer {
 public:
  ControlServer(const std::string& socket_name, std::vector<uid_t> users, std::string prompt_exe);
  ~ControlServer();
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  void poll(Access& access, const std::string& state, const std::string& status, int64_t now_ns);
  size_t prompt_count() const { return prompts_.size(); }

 private:
  bool is_trusted_prompt(int fd) const;
  void send_list(int fd, const Access& access, const std::string& state, const std::string& status) const;

  int listen_fd_ = -1;
  std::vector<uid_t> users_;
  std::string prompt_exe_;
  std::vector<int> prompts_;
  std::string last_state_, last_status_;
};

}  // namespace fca
