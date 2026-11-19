# Frame Camera Access: technical notes

How it works, how to use the cameras from your own app, and how to build it yourself. The short guide is in
[README.md](README.md).

Read-only access to the **Steam Frame**'s four tracking cameras for your own apps. A small root service
(`frame-camera-accessd`) shares the camera images that the headset already captures, and asks you in the headset
before an app sees them for the first time.

```
4 tracking cameras ─► frame-camera-accessd ─► libframe_camera_access.so ─► your app
                      (root service,          (C client library,
                      read-only)              shared memory, no copies over a socket)
```

Status: research prototype, tested on one Steam Frame (SteamOS, SteamVR 2.18).

## Install

Open a console on the headset and run:

```sh
curl -fsSL https://raw.githubusercontent.com/ZzzDevXR/Steam-Frame-Camera-Access/main/install.sh | sudo bash
```

What the command does ([install.sh](install.sh), worth reading before you pipe it into `sudo`):

- downloads the prebuilt release to `~/Steam-Frame-Camera-Access` and checks its SHA-256;
- installs the service and the permission prompt as root in `/var/lib/frame-camera-access`, and starts them.

It takes a few seconds and needs internet access. Run the same command again to update.

Check that frames arrive:

```sh
~/Steam-Frame-Camera-Access/build/fca_probe --frames 100
```

## Use it from your app

Link `build/libframe_camera_access.so` and include [native/include/fca.h](native/include/fca.h). Images are
8-bit grayscale; all four cameras of one moment come as a set.

```c
#include "fca.h"

fca_client* client = NULL;
if (fca_connect(NULL, &client) != FCA_OK) return 1;

int count = fca_camera_count(client);
fca_camera_info cameras[4];
uint8_t* images[4];
uint64_t sizes[4];
for (int i = 0; i < count; ++i) {
  fca_get_camera_info(client, i, &cameras[i]);
  sizes[i] = (uint64_t)cameras[i].stride * cameras[i].height;
  images[i] = malloc(sizes[i]);
}

uint64_t last = 0;
fca_frame_info info;
while (fca_wait_for_frame(client, last, 3000) == FCA_OK) {
  if (fca_read_latest(client, images, sizes, count, &info) != FCA_OK) continue;
  last = info.set_index;
  /* images[FCA_CAMERA_SLAM_LEFT] ... are ready to use */
}
fca_disconnect(client);
```

[native/tools/fca_probe.c](native/tools/fca_probe.c) is a complete example with error handling. The first time an
app connects, the headset shows a prompt naming the app; the answer is remembered.

## Remove

```sh
sudo bash ~/Steam-Frame-Camera-Access/scripts/uninstall-daemon.sh
```

## Build from source

It needs `git`, `cmake`, `make`, `gcc`/`g++` and `pkg-config` with the Cairo, FreeType and Vulkan development
files on the headset.

```sh
git clone https://github.com/ZzzDevXR/Steam-Frame-Camera-Access && cd Steam-Frame-Camera-Access
bash scripts/build.sh
sudo bash scripts/install-daemon.sh
```

`curl ... | sudo bash -s -- --build` does the same in one step. To make the release that the installer downloads,
run `bash scripts/make-release.sh <version>` after the build and attach the two files it writes to `dist/` to a
new GitHub release.

## License

MIT License ([LICENSE](LICENSE)), by [ZzzDevXR](https://github.com/ZzzDevXR). Third-party parts keep their own
licenses: the OpenVR header (BSD-3-Clause, `native/prompt/third_party/openvr/`), frameeyeosc `vk_texture` (MIT,
`native/prompt/third_party/frameeyeosc/`) and the Caveat Brush and Nunito fonts (SIL OFL 1.1,
`native/prompt/assets/`).
