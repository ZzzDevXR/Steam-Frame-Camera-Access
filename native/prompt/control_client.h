#pragma once

#include <string>
#include <utility>
#include <vector>

struct AppEntry {
  std::string decision;
  std::string key, label;
  int connections = 0;
};

struct DaemonState {
  std::vector<AppEntry> apps;
  std::string state, status;
  bool connected = false;
};

class ControlClient {
 public:
  ~ControlClient();
  bool connect(const std::string& socket_name, std::string& error);
  void disconnect();
  bool connected() const { return fd_ >= 0; }

  bool poll(DaemonState& state, std::vector<std::pair<std::string, std::string>>& requests);
  bool send_decision(const std::string& decision, const std::string& key);

 private:
  int fd_ = -1;
};
