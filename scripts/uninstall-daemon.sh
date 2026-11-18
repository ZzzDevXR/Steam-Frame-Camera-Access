#!/usr/bin/env bash
set -euo pipefail
prefix=${FCA_PREFIX:-/var/lib/frame-camera-access}
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
systemctl disable --now frame-camera-access.service 2>/dev/null || true
if [[ -n ${SUDO_UID:-} && $SUDO_UID != 0 ]]; then
  user=$(id -nu "$SUDO_UID")
  home=$(getent passwd "$SUDO_UID" | cut -d: -f6)
  sudo -u "$user" XDG_RUNTIME_DIR="/run/user/$SUDO_UID" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$SUDO_UID/bus" \
    systemctl --user disable --now frame-camera-access-prompt.service 2>/dev/null || true
  rm -f "$home/.config/systemd/user/frame-camera-access-prompt.service"
fi
rm -f /etc/systemd/system/frame-camera-access.service
systemctl daemon-reload
rm -f "$prefix/bin/frame-camera-accessd" "$prefix/bin/frame-camera-access-prompt"
rmdir "$prefix/bin" "$prefix" 2>/dev/null || true
echo "removed frame-camera-access"
