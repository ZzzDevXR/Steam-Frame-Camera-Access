#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fca.h"
#include "fca_protocol.h"

#define LOAD_ACQUIRE(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LOAD_RELAXED(p) __atomic_load_n((p), __ATOMIC_RELAXED)

enum { READ_ATTEMPTS = 8, HANDSHAKE_TIMEOUT_MS = 500, WAIT_SLICE_MS = 100 };

struct fca_client {
  int sock;
  int shm_fd;
  const fca_shm_header* header;
  uint64_t size;
  int disconnected;
};

int64_t fca_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

uint32_t fca_version(void) { return (FCA_VERSION_MAJOR << 16) | (FCA_VERSION_MINOR << 8) | FCA_VERSION_PATCH; }
uint32_t fca_protocol_version(void) { return FCA_PROTOCOL_VERSION; }

const char* fca_result_string(int32_t result) {
  switch (result) {
    case FCA_OK:
      return "ok";
    case FCA_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case FCA_ERR_CONNECT:
      return "cannot connect to frame-camera-accessd (not installed or not running)";
    case FCA_ERR_UNTRUSTED_DAEMON:
      return "the camera socket is owned by an untrusted user";
    case FCA_ERR_REJECTED:
      return "camera access denied";
    case FCA_ERR_VERSION:
      return "frame-camera-accessd uses a different protocol version";
    case FCA_ERR_PROTOCOL:
      return "malformed response from frame-camera-accessd";
    case FCA_ERR_DISCONNECTED:
      return "frame-camera-accessd went away, reconnect";
    case FCA_ERR_NO_FRAME:
      return "no camera frame published yet";
    case FCA_ERR_TORN:
      return "the newest camera frame kept changing while it was copied";
    case FCA_ERR_BUFFER_TOO_SMALL:
      return "destination buffer too small";
    case FCA_ERR_TIMEOUT:
      return "timed out (no new camera frame or frame-camera-accessd did not answer)";
    case FCA_ERR_SYSTEM:
      return "system error";
    case FCA_ERR_PENDING:
      return "waiting for camera permission: allow this app in the prompt in the headset";
    default:
      return "unknown result";
  }
}

static int socket_address(const char* name, struct sockaddr_un* address, socklen_t* address_length) {
  const size_t name_length = strlen(name);
  memset(address, 0, sizeof *address);
  address->sun_family = AF_UNIX;
  if (name[0] == '@') {
    if (name_length < 2 || name_length > sizeof address->sun_path) return -1;
    memcpy(address->sun_path + 1, name + 1, name_length - 1);
    *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length);
  } else {
    if (name_length == 0 || name_length >= sizeof address->sun_path) return -1;
    memcpy(address->sun_path, name, name_length);
    *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length + 1);
  }
  return 0;
}

static int in_user_namespace(void) {
  unsigned long inside = 0, outside = 0, count = 0;
  FILE* uid_map = fopen("/proc/self/uid_map", "r");
  if (!uid_map) return 0;
  const int fields_read = fscanf(uid_map, "%lu %lu %lu", &inside, &outside, &count);
  fclose(uid_map);
  return !(fields_read == 3 && inside == 0 && outside == 0 && count == 4294967295UL);
}

static unsigned long overflow_uid(void) {
  unsigned long uid = 65534;
  FILE* file = fopen("/proc/sys/kernel/overflowuid", "r");
  if (file) {
    if (fscanf(file, "%lu", &uid) != 1) uid = 65534;
    fclose(file);
  }
  return uid;
}

static int is_trusted_daemon(uid_t uid) {
  if (uid == 0 || uid == getuid()) return 1;
  const char* configured = getenv("FCA_TRUSTED_DAEMON_UID");
  if (configured && *configured) {
    char* end = NULL;
    const unsigned long trusted = strtoul(configured, &end, 10);
    if (end && *end == '\0' && trusted == (unsigned long)uid) return 1;
  }
  return in_user_namespace() && (unsigned long)uid == overflow_uid();
}

static int is_terminated(const char* text, size_t capacity) { return memchr(text, '\0', capacity) != NULL; }

static int is_valid_shm(const fca_shm_header* header, uint64_t size) {
  if (size < sizeof *header) return 0;
  if (header->magic != FCA_SHM_MAGIC || header->version != FCA_PROTOCOL_VERSION ||
      header->header_size != sizeof *header)
    return 0;
  if (header->total_size != size || header->camera_count == 0 || header->camera_count > FCA_MAX_CAMERAS) return 0;
  if (header->slot_count < 2 || header->slot_count > FCA_MAX_SLOTS || header->slot_stride == 0) return 0;
  if (header->data_offset < sizeof *header || header->data_offset > size) return 0;
  if ((size - header->data_offset) / header->slot_stride < header->slot_count) return 0;
  if (!is_terminated(header->source, sizeof header->source) || !is_terminated(header->status, sizeof header->status))
    return 0;
  for (uint32_t i = 0; i < header->camera_count; ++i) {
    const fca_shm_camera* camera = &header->cameras[i];
    if (!is_terminated(camera->name, sizeof camera->name) || !is_terminated(camera->sensor, sizeof camera->sensor))
      return 0;
    if (camera->pixel_format != FCA_PIXEL_FORMAT_GRAY8 || camera->width == 0 || camera->height == 0) return 0;
    if (camera->width > 8192 || camera->height > 8192 || camera->stride != camera->width) return 0;
    if (camera->image_size != (uint64_t)camera->stride * camera->height) return 0;
    if (camera->image_offset > header->slot_stride || camera->image_size > header->slot_stride - camera->image_offset)
      return 0;
  }
  return 1;
}

static void set_timeouts(int sock, int milliseconds) {
  struct timeval timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

static int32_t receive_welcome(int sock, fca_welcome* welcome, int* fd) {
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(4 * sizeof(int))];
  } control;
  struct iovec iov = {welcome, sizeof *welcome};
  struct msghdr message;
  memset(&message, 0, sizeof message);
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control.buf;
  message.msg_controllen = sizeof control.buf;
  *fd = -1;
  const ssize_t received_bytes = recvmsg(sock, &message, MSG_CMSG_CLOEXEC);
  if (received_bytes < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? FCA_ERR_TIMEOUT : FCA_ERR_PROTOCOL;
  for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
    const size_t descriptor_count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < descriptor_count; ++i) {
      int descriptor;
      memcpy(&descriptor, CMSG_DATA(header) + i * sizeof(int), sizeof descriptor);
      if (*fd < 0)
        *fd = descriptor;
      else
        close(descriptor);
    }
  }
  if (received_bytes != (ssize_t)sizeof *welcome || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      welcome->magic != FCA_WELCOME_MAGIC) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
    return FCA_ERR_PROTOCOL;
  }
  return FCA_OK;
}

static int32_t connect_to(const char* name, fca_client** out_client) {
  struct sockaddr_un address;
  socklen_t address_length;
  if (socket_address(name, &address, &address_length) != 0) return FCA_ERR_INVALID_ARGUMENT;

  const int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) return FCA_ERR_SYSTEM;
  int32_t result = FCA_ERR_PROTOCOL;
  int fd = -1;
  void* mapping = MAP_FAILED;
  struct stat shm_status;
  fca_welcome welcome;
  set_timeouts(sock, HANDSHAKE_TIMEOUT_MS);
  if (connect(sock, (const struct sockaddr*)&address, address_length) != 0) {
    result = errno == EAGAIN || errno == EWOULDBLOCK ? FCA_ERR_TIMEOUT : FCA_ERR_CONNECT;
    goto fail;
  }
  {
    struct ucred credentials;
    socklen_t credentials_length = sizeof credentials;
    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0) {
      result = FCA_ERR_SYSTEM;
      goto fail;
    }
    if (!is_trusted_daemon(credentials.uid)) {
      result = FCA_ERR_UNTRUSTED_DAEMON;
      goto fail;
    }
  }
  {
    const fca_hello hello = {FCA_HELLO_MAGIC, FCA_PROTOCOL_VERSION, 0, 0};
    if (send(sock, &hello, sizeof hello, MSG_NOSIGNAL) != (ssize_t)sizeof hello) {
      result = FCA_ERR_CONNECT;
      goto fail;
    }
  }
  result = receive_welcome(sock, &welcome, &fd);
  if (result != FCA_OK) goto fail;
  result = FCA_ERR_PROTOCOL;
  if (welcome.result == FCA_WELCOME_NOT_ALLOWED) {
    result = FCA_ERR_REJECTED;
    goto fail;
  }
  if (welcome.result == FCA_WELCOME_PENDING) {
    result = FCA_ERR_PENDING;
    goto fail;
  }
  if (welcome.result == FCA_WELCOME_VERSION_MISMATCH || welcome.version != FCA_PROTOCOL_VERSION) {
    result = FCA_ERR_VERSION;
    goto fail;
  }
  if (welcome.result != FCA_WELCOME_OK || fd < 0) goto fail;
  if (fstat(fd, &shm_status) != 0 || shm_status.st_size < 0 || (uint64_t)shm_status.st_size != welcome.shm_size)
    goto fail;
  mapping = mmap(NULL, (size_t)welcome.shm_size, PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    result = FCA_ERR_SYSTEM;
    goto fail;
  }
  if (!is_valid_shm((const fca_shm_header*)mapping, welcome.shm_size)) goto fail;
  {
    fca_client* client = calloc(1, sizeof *client);
    if (!client) {
      result = FCA_ERR_SYSTEM;
      goto fail;
    }
    client->sock = sock;
    client->shm_fd = fd;
    client->header = (const fca_shm_header*)mapping;
    client->size = welcome.shm_size;
    *out_client = client;
    return FCA_OK;
  }
fail:
  if (mapping != MAP_FAILED) munmap(mapping, (size_t)welcome.shm_size);
  if (fd >= 0) close(fd);
  close(sock);
  return result;
}

int32_t fca_connect(const char* socket_name, fca_client** out_client) {
  if (!out_client) return FCA_ERR_INVALID_ARGUMENT;
  *out_client = NULL;
  const char* name = socket_name && *socket_name ? socket_name : getenv("FCA_SOCKET");
  if (name && *name) return connect_to(name, out_client);
  const int32_t result = connect_to(FCA_DEFAULT_SOCKET, out_client);
  return result == FCA_ERR_CONNECT ? connect_to(FCA_DEFAULT_PATH_SOCKET, out_client) : result;
}

void fca_disconnect(fca_client* client) {
  if (!client) return;
  munmap((void*)client->header, (size_t)client->size);
  close(client->shm_fd);
  close(client->sock);
  free(client);
}

static int is_connected(fca_client* client) {
  if (client->disconnected) return 0;
  struct pollfd poll_fd = {client->sock, POLLIN, 0};
  if (poll(&poll_fd, 1, 0) > 0) {
    if (poll_fd.revents & (POLLHUP | POLLERR | POLLNVAL))
      client->disconnected = 1;
    else if (poll_fd.revents & POLLIN) {
      char buffer[64];
      if (recv(client->sock, buffer, sizeof buffer, MSG_DONTWAIT) == 0) client->disconnected = 1;
    }
  }
  if (LOAD_ACQUIRE(&client->header->state) == FCA_STATE_STOPPED) client->disconnected = 1;
  return !client->disconnected;
}

int32_t fca_camera_count(const fca_client* client) {
  return client ? (int32_t)client->header->camera_count : FCA_ERR_INVALID_ARGUMENT;
}

int32_t fca_get_camera_info(const fca_client* client, uint32_t index, fca_camera_info* out_info) {
  if (!client || !out_info || index >= client->header->camera_count) return FCA_ERR_INVALID_ARGUMENT;
  const fca_shm_camera* camera = &client->header->cameras[index];
  memset(out_info, 0, sizeof *out_info);
  out_info->index = index;
  out_info->width = camera->width;
  out_info->height = camera->height;
  out_info->stride = camera->stride;
  out_info->pixel_format = camera->pixel_format;
  memcpy(out_info->name, camera->name, sizeof out_info->name);
  memcpy(out_info->sensor, camera->sensor, sizeof out_info->sensor);
  return FCA_OK;
}

int32_t fca_get_status(fca_client* client, fca_status* out_status) {
  if (!client || !out_status) return FCA_ERR_INVALID_ARGUMENT;
  const fca_shm_header* header = client->header;
  memset(out_status, 0, sizeof *out_status);
  out_status->connected = (uint32_t)is_connected(client);
  out_status->state = LOAD_ACQUIRE(&header->state);
  out_status->daemon_pid = header->daemon_pid;
  out_status->heartbeat_age_ns = fca_now_ns() - LOAD_ACQUIRE(&header->heartbeat_ns);
  out_status->sets_published = LOAD_RELAXED(&header->sets_published);
  out_status->copy_races = LOAD_RELAXED(&header->copy_races);
  out_status->invalid_metadata = LOAD_RELAXED(&header->invalid_metadata);
  out_status->dark_skipped = LOAD_RELAXED(&header->dark_skipped);
  for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt) {
    const uint64_t sequence_before = LOAD_ACQUIRE(&header->status_seq);
    if (sequence_before & 1) continue;
    memcpy(out_status->source, header->source, sizeof out_status->source);
    memcpy(out_status->status, header->status, sizeof out_status->status);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (LOAD_RELAXED(&header->status_seq) == sequence_before) break;
  }
  out_status->source[sizeof out_status->source - 1] = '\0';
  out_status->status[sizeof out_status->status - 1] = '\0';
  return FCA_OK;
}

int32_t fca_latest_set_index(fca_client* client, uint64_t* out_set_index) {
  if (!client || !out_set_index) return FCA_ERR_INVALID_ARGUMENT;
  if (!is_connected(client)) return FCA_ERR_DISCONNECTED;
  *out_set_index = LOAD_ACQUIRE(&client->header->latest_set);
  return FCA_OK;
}

int32_t fca_wait_for_frame(fca_client* client, uint64_t after_set_index, int32_t timeout_ms) {
  if (!client || timeout_ms < 0) return FCA_ERR_INVALID_ARGUMENT;
  const int64_t deadline = fca_now_ns() + (int64_t)timeout_ms * 1000000;
  uint32_t* futex_word = (uint32_t*)&client->header->frame_counter;
  for (;;) {
    if (!is_connected(client)) return FCA_ERR_DISCONNECTED;
    const uint32_t counter = LOAD_ACQUIRE(futex_word);
    if (LOAD_ACQUIRE(&client->header->latest_set) > after_set_index) return FCA_OK;
    const int64_t remaining = deadline - fca_now_ns();
    if (remaining <= 0) return FCA_ERR_TIMEOUT;
    const int64_t slice = remaining < WAIT_SLICE_MS * 1000000LL ? remaining : WAIT_SLICE_MS * 1000000LL;
    struct timespec slice_time = {(time_t)(slice / 1000000000), (long)(slice % 1000000000)};
    if (syscall(SYS_futex, futex_word, FUTEX_WAIT, counter, &slice_time, NULL, 0) != 0 && errno != EAGAIN &&
        errno != ETIMEDOUT && errno != EINTR) {
      struct timespec pause = {0, 1000000};
      nanosleep(&pause, NULL);
    }
  }
}

int32_t fca_read_latest(fca_client* client, uint8_t* const* destinations, const uint64_t* capacities, uint32_t count,
                        fca_frame_info* out_info) {
  if (!client || (count && (!destinations || !capacities))) return FCA_ERR_INVALID_ARGUMENT;
  const fca_shm_header* header = client->header;
  if (count > header->camera_count) return FCA_ERR_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < count; ++i)
    if (destinations[i] && capacities[i] < header->cameras[i].image_size) return FCA_ERR_BUFFER_TOO_SMALL;
  if (!is_connected(client)) return FCA_ERR_DISCONNECTED;

  for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt) {
    const uint64_t set = LOAD_ACQUIRE(&header->latest_set);
    if (set == 0) return FCA_ERR_NO_FRAME;
    const uint32_t index = (uint32_t)(set % header->slot_count);
    const fca_shm_slot* slot = &header->slots[index];
    const uint64_t sequence_before = LOAD_ACQUIRE(&slot->seq);
    if (sequence_before & 1) continue;
    fca_shm_slot slot_copy;
    memcpy(&slot_copy, slot, sizeof slot_copy);
    if (slot_copy.set_index != set) continue;
    const uint8_t* slot_images = (const uint8_t*)header + header->data_offset + (uint64_t)index * header->slot_stride;
    for (uint32_t i = 0; i < count; ++i)
      if (destinations[i])
        memcpy(destinations[i], slot_images + header->cameras[i].image_offset, (size_t)header->cameras[i].image_size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (LOAD_RELAXED(&slot->seq) != sequence_before) continue;
    if (out_info) {
      memset(out_info, 0, sizeof *out_info);
      out_info->set_index = slot_copy.set_index;
      for (uint32_t i = 0; i < 4; ++i) {
        out_info->sof_ns[i] = slot_copy.sof_ns[i];
        out_info->eof_ns[i] = slot_copy.eof_ns[i];
      }
      out_info->copy_end_ns = slot_copy.copy_end_ns;
      out_info->read_ns = fca_now_ns();
      out_info->mean_gray = slot_copy.mean_gray;
      out_info->flags = slot_copy.flags;
    }
    return FCA_OK;
  }
  return FCA_ERR_TORN;
}
