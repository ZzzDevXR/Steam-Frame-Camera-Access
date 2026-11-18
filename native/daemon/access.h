#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fca {

struct AppIdentity {
  std::string key;
  std::string label;
  std::string refused;
};

class Access {
 public:
  enum class Verdict { Allow, Deny, Pending };
  enum class Decision { Allow, Once, Deny, Pending };

  struct App {
    std::string key, label;
    Decision decision;
    int connections;
  };

  Access(std::string policy_path, std::vector<std::string> homes);

  bool identify(pid_t pid, int pidfd, AppIdentity& out) const;

  Verdict check(const AppIdentity& app, int64_t now_ns);
  void connected(const std::string& key);
  void disconnected(const std::string& key, int64_t now_ns);

  void decide(const std::string& key, const std::string& decision, int64_t now_ns);
  void reload();
  void expire(int64_t now_ns);

  std::vector<App> apps() const;
  std::vector<AppIdentity> take_new_requests();
  bool take_changed();
  std::vector<std::string> take_revoked();
  void ring_replaced();

 private:
  struct Entry {
    std::string label;
    Decision decision = Decision::Pending;
    int connections = 0;
    int64_t expires_ns = 0;
    bool saved = false;
    bool holds_ring = false;
  };
  void save() const;
  void end_access(Entry& entry, const std::string& key);
  std::string container_name(const std::string& id) const;

  std::string path_;
  std::vector<std::string> homes_;
  ino_t host_mnt_ = 0;
  std::map<std::string, Entry> entries_;
  std::vector<AppIdentity> new_requests_;
  std::vector<std::string> revoked_;
  bool changed_ = true;
};

const char* decision_name(Access::Decision decision);

}  // namespace fca
