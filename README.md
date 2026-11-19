# Frame Camera Access

Lets apps on the **Steam Frame** see through the headset's four tracking cameras.

Apps can only take camera data without changing anything, and the headset asks you before an app sees the cameras for the first
time.

This is an early prototype. It has been tested on one headset, so expect rough edges!

## Before you install

- **You install and use this at your own risk.** It is an unofficial hobby project and comes with no warranty.
  It adds a background service with administrator rights to your headset.
- **This is not a Valve product.** The project and the mascot (that Frame anime girl) it shows in the headset are not connected to,
  affiliated with or endorsed by Valve.

## Install

If you are installing Frame Hand Tracking, skip this: its installer sets this up for you.

1. Open a terminal on the headset (Konsole in Desktop mode, or connect over SSH).
2. Paste this line and press Enter:

   ```sh
   curl -fsSL https://raw.githubusercontent.com/ZzzDevXR/Steam-Frame-Camera-Access/main/install.sh | sudo bash
   ```

3. Wait until it says "Frame Camera Access is installed". It takes a few seconds and needs internet.

## Update

Run the install line again. It replaces the old version with the newest one.

## Uninstall

```sh
sudo bash ~/Steam-Frame-Camera-Access/scripts/uninstall-daemon.sh
```

After that you can delete the `Steam-Frame-Camera-Access` folder in your home folder. Apps that use the cameras,
such as Frame Hand Tracking, stop working without it.

## Support

If this is useful to you, you can [buy me a coffee](https://buymeacoffee.com/zzzdevxr).

## More

- Using the cameras in your own app, and building it yourself: [DEVELOPERS.md](DEVELOPERS.md)
- Made by [ZzzDevXR](https://github.com/ZzzDevXR), under the MIT License ([LICENSE](LICENSE)). The bundled fonts
  and third-party code keep their own licenses, listed in [DEVELOPERS.md](DEVELOPERS.md).
