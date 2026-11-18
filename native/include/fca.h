#ifndef FCA_H
#define FCA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define FCA_API
#else
#define FCA_API __attribute__((visibility("default")))
#endif

#define FCA_VERSION_MAJOR 0
#define FCA_VERSION_MINOR 1
#define FCA_VERSION_PATCH 0

typedef enum fca_result {
  FCA_OK = 0,
  FCA_ERR_INVALID_ARGUMENT = -1,
  FCA_ERR_CONNECT = -2,
  FCA_ERR_UNTRUSTED_DAEMON = -3,
  FCA_ERR_REJECTED = -4,
  FCA_ERR_VERSION = -5,
  FCA_ERR_PROTOCOL = -6,
  FCA_ERR_DISCONNECTED = -7,
  FCA_ERR_NO_FRAME = -8,
  FCA_ERR_TORN = -9,
  FCA_ERR_BUFFER_TOO_SMALL = -10,
  FCA_ERR_TIMEOUT = -11,
  FCA_ERR_SYSTEM = -12,
  FCA_ERR_PENDING = -13
} fca_result;

typedef enum fca_state {
  FCA_STATE_STARTING = 0,
  FCA_STATE_UNAVAILABLE = 1,
  FCA_STATE_WAITING = 2,
  FCA_STATE_STREAMING = 3,
  FCA_STATE_STOPPED = 4
} fca_state;

enum { FCA_CAMERA_SLAM_RIGHT = 0, FCA_CAMERA_SLAM_LEFT = 1, FCA_CAMERA_UPPER_LEFT = 2, FCA_CAMERA_UPPER_RIGHT = 3 };

enum { FCA_PIXEL_FORMAT_GRAY8 = 1 };

enum { FCA_FRAME_FLAG_DARK = 1u << 0, FCA_FRAME_FLAG_SYNTHETIC = 1u << 1 };

typedef struct fca_camera_info {
  uint32_t index;
  uint32_t width, height;
  uint32_t stride;
  uint32_t pixel_format;
  char name[32];
  char sensor[32];
} fca_camera_info;

typedef struct fca_frame_info {
  uint64_t set_index;
  int64_t sof_ns[4];
  int64_t eof_ns[4];
  int64_t copy_end_ns;
  int64_t read_ns;
  float mean_gray;
  uint32_t flags;
} fca_frame_info;

typedef struct fca_status {
  uint32_t state;
  uint32_t connected;
  uint32_t daemon_pid;
  uint32_t reserved;
  int64_t heartbeat_age_ns;
  uint64_t sets_published, copy_races, invalid_metadata, dark_skipped;
  char source[32];
  char status[128];
} fca_status;

typedef struct fca_client fca_client;

FCA_API uint32_t fca_version(void);
FCA_API uint32_t fca_protocol_version(void);
FCA_API const char* fca_result_string(int32_t result);
FCA_API int64_t fca_now_ns(void);

FCA_API int32_t fca_connect(const char* socket_name, fca_client** out_client);
FCA_API void fca_disconnect(fca_client* client);

FCA_API int32_t fca_camera_count(const fca_client* client);
FCA_API int32_t fca_get_camera_info(const fca_client* client, uint32_t index, fca_camera_info* out_info);
FCA_API int32_t fca_get_status(fca_client* client, fca_status* out_status);

FCA_API int32_t fca_latest_set_index(fca_client* client, uint64_t* out_set_index);

FCA_API int32_t fca_wait_for_frame(fca_client* client, uint64_t after_set_index, int32_t timeout_ms);

FCA_API int32_t fca_read_latest(fca_client* client, uint8_t* const* destinations, const uint64_t* capacities,
                                uint32_t count, fca_frame_info* out_info);

#ifdef __cplusplus
}
#endif

#endif
