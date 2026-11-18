#ifndef FCA_PROTOCOL_H
#define FCA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FCA_PROTOCOL_VERSION 1u
#define FCA_DEFAULT_SOCKET "@frame-camera-access"
#define FCA_DEFAULT_PATH_SOCKET "/dev/shm/frame-camera-access.sock"

#define FCA_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FCA_SHM_MAGIC FCA_FOURCC('F', 'C', 'A', 'S')
#define FCA_HELLO_MAGIC FCA_FOURCC('F', 'C', 'A', 'H')
#define FCA_WELCOME_MAGIC FCA_FOURCC('F', 'C', 'A', 'W')

#define FCA_MAX_CAMERAS 4u
#define FCA_MAX_SLOTS 8u
#define FCA_NAME_BYTES 32u
#define FCA_STATUS_BYTES 128u
#define FCA_DATA_ALIGN 4096u

typedef struct fca_hello {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t reserved;
} fca_hello;

enum { FCA_WELCOME_OK = 0, FCA_WELCOME_VERSION_MISMATCH = 1, FCA_WELCOME_NOT_ALLOWED = 2, FCA_WELCOME_PENDING = 3 };

typedef struct fca_welcome {
  uint32_t magic;
  uint32_t version;
  int32_t result;
  uint32_t reserved;
  uint64_t shm_size;
} fca_welcome;

typedef struct fca_shm_camera {
  char name[FCA_NAME_BYTES];
  char sensor[FCA_NAME_BYTES];
  uint32_t width, height;
  uint32_t stride;
  uint32_t pixel_format;
  uint64_t image_offset;
  uint64_t image_size;
  uint8_t reserved[32];
} fca_shm_camera;

typedef struct fca_shm_slot {
  uint64_t seq;
  uint64_t set_index;
  int64_t sof_ns[FCA_MAX_CAMERAS];
  int64_t eof_ns[FCA_MAX_CAMERAS];
  int64_t copy_end_ns;
  float mean_gray;
  uint32_t flags;
  uint8_t reserved[32];
} fca_shm_slot;

typedef struct fca_shm_header {
  uint32_t magic, version;
  uint32_t header_size;
  uint32_t camera_count;
  uint32_t slot_count;
  uint32_t state;
  uint64_t total_size;
  uint64_t data_offset;
  uint64_t slot_stride;
  uint64_t latest_set;
  uint32_t frame_counter;
  uint32_t daemon_pid;
  int64_t heartbeat_ns;
  int64_t daemon_start_ns;
  uint64_t sets_published, copy_races, invalid_metadata, dark_skipped;
  uint64_t calibration_offset, calibration_size;
  uint64_t status_seq;
  char source[FCA_NAME_BYTES];
  char status[FCA_STATUS_BYTES];
  uint8_t reserved[216];
  fca_shm_camera cameras[FCA_MAX_CAMERAS];
  fca_shm_slot slots[FCA_MAX_SLOTS];
} fca_shm_header;

#ifdef __cplusplus
#define FCA_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define FCA_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
FCA_STATIC_ASSERT(sizeof(fca_hello) == 16, "fca_hello layout");
FCA_STATIC_ASSERT(sizeof(fca_welcome) == 24, "fca_welcome layout");
FCA_STATIC_ASSERT(sizeof(fca_shm_camera) == 128, "fca_shm_camera layout");
FCA_STATIC_ASSERT(sizeof(fca_shm_slot) == 128, "fca_shm_slot layout");
FCA_STATIC_ASSERT(offsetof(fca_shm_header, cameras) == 512, "fca_shm_header layout");
FCA_STATIC_ASSERT(sizeof(fca_shm_header) == 2048, "fca_shm_header layout");
FCA_STATIC_ASSERT(sizeof(fca_shm_header) <= FCA_DATA_ALIGN, "header fits before the first slot");

#endif
