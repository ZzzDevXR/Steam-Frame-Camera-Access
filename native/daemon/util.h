#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fca {

inline int64_t now_raw_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

inline void sleep_ns(int64_t ns) {
  if (ns <= 0) return;
  timespec ts{time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  std::fprintf(stderr, "frame-camera-accessd: %s\n", buf);
}

inline std::runtime_error system_error(const std::string& what) {
  return std::runtime_error(what + ": " + std::strerror(errno));
}

inline bool read_text(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  out = text.str();
  return true;
}

inline bool read_user_file(const std::string& path, size_t max_bytes, std::string& out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return false;
  struct stat file_status{};
  bool complete = false;
  if (fstat(fd, &file_status) == 0 && S_ISREG(file_status.st_mode) && file_status.st_size >= 0 &&
      size_t(file_status.st_size) <= max_bytes) {
    out.clear();
    char chunk[65536];
    for (;;) {
      const ssize_t count = ::read(fd, chunk, sizeof chunk);
      if (count < 0 && errno == EINTR) continue;
      complete = count == 0;
      if (count <= 0 || out.size() + size_t(count) > max_bytes) break;
      out.append(chunk, size_t(count));
    }
  }
  ::close(fd);
  return complete;
}

inline ino_t mount_namespace(const std::string& pid) {
  struct stat st{};
  return stat(("/proc/" + pid + "/ns/mnt").c_str(), &st) == 0 ? st.st_ino : 0;
}

inline ino_t host_mount_namespace() {
  const ino_t init = mount_namespace("1");
  return init != 0 ? init : mount_namespace("self");
}

inline std::string trim(std::string s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

}  // namespace fca
