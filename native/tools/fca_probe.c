#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fca.h"

static void usage(void) {
  fputs(
      "usage: fca_probe [--socket NAME] [--frames N] [--timeout-ms MS] [--busy] [--verify-synthetic]\n"
      "                 [--snapshot DIR] [--quiet] [--drop-socket]\n"
      "  --frames N          read N frame sets, then exit (default 100)\n"
      "  --busy              read in a tight loop instead of waiting for each new set (stress test)\n"
      "  --verify-synthetic  check every image against the synthetic source's pattern\n"
      "  --snapshot DIR      write the first set as DIR/<camera>.pgm\n"
      "  --drop-socket       close the connection after connecting, keep reading the shared memory (test: the\n"
      "                      daemon must still cut the reader off when its access is revoked)\n",
      stderr);
}

static int drop_sockets(void) {
  DIR* dir = opendir("/proc/self/fd");
  if (!dir) return 0;
  int dropped = 0;
  const int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  for (const struct dirent* entry; null_fd >= 0 && (entry = readdir(dir)) != NULL;) {
    const int fd = atoi(entry->d_name);
    struct stat st;
    if (fd > 2 && fd != dirfd(dir) && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode) && dup2(null_fd, fd) == fd) ++dropped;
  }
  if (null_fd >= 0) close(null_fd);
  closedir(dir);
  return dropped;
}

static int verify_synthetic(const uint8_t* image, const fca_camera_info* cam, uint64_t set) {
  const uint64_t size = (uint64_t)cam->stride * cam->height;
  uint64_t head, tail;
  memcpy(&head, image, sizeof head);
  memcpy(&tail, image + size - sizeof tail, sizeof tail);
  if (head != set || tail != set) return 0;
  const uint32_t points[3][2] = {{cam->width / 2, cam->height / 2}, {cam->width - 1, 1}, {3, cam->height - 1}};
  for (int i = 0; i < 3; ++i) {
    const uint32_t x = points[i][0], y = points[i][1];
    const uint8_t expected = (uint8_t)(x + y + 4 * set + 64 * cam->index);
    if (image[(uint64_t)y * cam->stride + x] != expected) return 0;
  }
  return 1;
}

static int write_pgm(const char* dir, const fca_camera_info* cam, const uint8_t* image) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s.pgm", dir, cam->name);
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  fprintf(f, "P5\n%u %u\n255\n", cam->width, cam->height);
  const int ok = fwrite(image, 1, (size_t)cam->stride * cam->height, f) == (size_t)cam->stride * cam->height;
  fclose(f);
  if (ok) printf("wrote %s\n", path);
  return ok;
}

int main(int argc, char** argv) {
  const char *socket_name = NULL, *snapshot = NULL;
  long frames = 100, timeout_ms = 3000;
  int busy = 0, verify = 0, quiet = 0, drop_socket = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!strcmp(a, "--socket") && i + 1 < argc)
      socket_name = argv[++i];
    else if (!strcmp(a, "--frames") && i + 1 < argc)
      frames = strtol(argv[++i], NULL, 10);
    else if (!strcmp(a, "--timeout-ms") && i + 1 < argc)
      timeout_ms = strtol(argv[++i], NULL, 10);
    else if (!strcmp(a, "--snapshot") && i + 1 < argc)
      snapshot = argv[++i];
    else if (!strcmp(a, "--busy"))
      busy = 1;
    else if (!strcmp(a, "--verify-synthetic"))
      verify = 1;
    else if (!strcmp(a, "--quiet"))
      quiet = 1;
    else if (!strcmp(a, "--drop-socket"))
      drop_socket = 1;
    else {
      usage();
      return 2;
    }
  }
  if (frames < 1 || timeout_ms < 1) {
    usage();
    return 2;
  }

  fca_client* client = NULL;
  int32_t r = fca_connect(socket_name, &client);
  if (r != FCA_OK) {
    fprintf(stderr, "fca_probe: connect: %s (%d)\n", fca_result_string(r), r);
    return 1;
  }
  if (drop_socket && drop_sockets() != 1) {
    fprintf(stderr, "fca_probe: --drop-socket: expected exactly one socket\n");
    return 1;
  }
  const uint32_t v = fca_version();
  fca_status status;
  fca_get_status(client, &status);
  printf("library %u.%u.%u, protocol %u; daemon pid %u, source %s, state %u: %s\n", v >> 16, (v >> 8) & 0xff, v & 0xff,
         fca_protocol_version(), status.daemon_pid, status.source, status.state, status.status);

  const int count = fca_camera_count(client);
  fca_camera_info cams[4];
  uint8_t* buffers[4] = {0};
  uint64_t capacities[4] = {0};
  for (int i = 0; i < count; ++i) {
    fca_get_camera_info(client, (uint32_t)i, &cams[i]);
    capacities[i] = (uint64_t)cams[i].stride * cams[i].height;
    buffers[i] = malloc((size_t)capacities[i]);
    if (!buffers[i]) {
      for (int j = 0; j < i; ++j) free(buffers[j]);
      return 1;
    }
    printf("camera %d: %-12s %4ux%-4u gray8  sensor %s\n", i, cams[i].name, cams[i].width, cams[i].height,
           cams[i].sensor);
  }

  long read = 0, skipped = 0, torn = 0, bad = 0;
  uint64_t last = 0;
  int64_t window_start = fca_now_ns(), age_sum = 0;
  long window_sets = 0;
  int exit_code = 0;
  while (read < frames) {
    if (!busy) {
      r = fca_wait_for_frame(client, last, (int32_t)timeout_ms);
      if (r != FCA_OK) {
        fca_get_status(client, &status);
        fprintf(stderr, "fca_probe: waiting for frames: %s (daemon state %u: %s)\n", fca_result_string(r), status.state,
                status.status);
        exit_code = 1;
        break;
      }
    }
    fca_frame_info info;
    r = fca_read_latest(client, buffers, capacities, (uint32_t)count, &info);
    if (r == FCA_ERR_TORN) {
      ++torn;
      continue;
    }
    if (r == FCA_ERR_NO_FRAME && busy) continue;
    if (r != FCA_OK) {
      fprintf(stderr, "fca_probe: read: %s (%d)\n", fca_result_string(r), r);
      exit_code = 1;
      break;
    }
    if (info.set_index <= last) continue;
    if (last && info.set_index > last + 1) skipped += (long)(info.set_index - last - 1);
    last = info.set_index;
    ++read;
    ++window_sets;
    age_sum += info.read_ns - info.sof_ns[0];
    if (verify) {
      if (!(info.flags & FCA_FRAME_FLAG_SYNTHETIC)) {
        fprintf(stderr, "fca_probe: --verify-synthetic, but the daemon is not running the synthetic source\n");
        exit_code = 1;
        break;
      }
      for (int i = 0; i < count; ++i)
        if (!verify_synthetic(buffers[i], &cams[i], info.set_index)) {
          ++bad;
          fprintf(stderr, "fca_probe: set %" PRIu64 " camera %s does not match the pattern\n", info.set_index,
                  cams[i].name);
        }
    }
    if (snapshot && read == 1)
      for (int i = 0; i < count; ++i)
        if (!write_pgm(snapshot, &cams[i], buffers[i])) {
          fprintf(stderr, "fca_probe: cannot write snapshot\n");
          exit_code = 1;
        }
    const int64_t now = fca_now_ns();
    if (now - window_start >= 1000000000 || read == frames) {
      if (!quiet)
        printf("set %" PRIu64 ": %.1f sets/s, age %.1f ms (camera start of frame -> read), mean gray %.1f%s\n",
               info.set_index, (double)window_sets * 1e9 / (double)(now - window_start),
               (double)age_sum / (double)window_sets / 1e6, info.mean_gray,
               info.flags & FCA_FRAME_FLAG_DARK ? " (dark)" : "");
      window_start = now;
      window_sets = 0;
      age_sum = 0;
    }
  }
  fca_get_status(client, &status);
  printf("read %ld sets, skipped %ld, torn retries %ld, pattern errors %ld; daemon: published %" PRIu64
         ", copy races %" PRIu64 ", invalid metadata %" PRIu64 ", dark skipped %" PRIu64 "\n",
         read, skipped, torn, bad, status.sets_published, status.copy_races, status.invalid_metadata,
         status.dark_skipped);
  for (int i = 0; i < count; ++i) free(buffers[i]);
  fca_disconnect(client);
  if (bad) exit_code = 1;
  return exit_code;
}
