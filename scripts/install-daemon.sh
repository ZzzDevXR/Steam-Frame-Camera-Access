#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${FCA_PREFIX:-/var/lib/frame-camera-access}
unit=/etc/systemd/system/frame-camera-access.service
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
[[ $prefix =~ ^/[A-Za-z0-9/._-]*$ ]] || { echo "FCA_PREFIX must be an absolute path of letters, digits and /._-" >&2; exit 1; }
uid=${SUDO_UID:-}
[[ -n $uid && $uid != 0 ]] || { echo "run through sudo from the user account that runs the apps (SUDO_UID)" >&2; exit 1; }
binary=$root/build/frame-camera-accessd
prompt=$root/build/frame-camera-access-prompt
[[ -x $binary ]] || { echo "build first: scripts/build.sh" >&2; exit 1; }
[[ -x $prompt ]] || { echo "the permission prompt was not built (scripts/build.sh needs the cairo, freetype2 and vulkan development files)" >&2; exit 1; }

install -d -o root -g root -m 0755 "$prefix" "$prefix/bin"
install -o root -g root -m 0755 "$binary" "$prefix/bin/frame-camera-accessd"
install -o root -g root -m 0755 "$prompt" "$prefix/bin/frame-camera-access-prompt"
sed -e "s|@PREFIX@|$prefix|g" -e "s|@UID@|$uid|g" "$root/scripts/frame-camera-access.service.in" > "$unit"
chmod 0644 "$unit"
systemctl daemon-reload
systemctl enable frame-camera-access.service
systemctl restart frame-camera-access.service
sleep 1
systemctl --no-pager --lines 10 status frame-camera-access.service || true

user=$(id -nu "$uid")
home=$(getent passwd "$uid" | cut -d: -f6)
user_units=$home/.config/systemd/user
as_user=(sudo -u "$user" XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus")
"${as_user[@]}" mkdir -p "$user_units"
sed -e "s|@PREFIX@|$prefix|g" "$root/scripts/frame-camera-access-prompt.service.in" |
  "${as_user[@]}" tee "$user_units/frame-camera-access-prompt.service" > /dev/null
"${as_user[@]}" systemctl --user daemon-reload
"${as_user[@]}" systemctl --user enable frame-camera-access-prompt.service
"${as_user[@]}" systemctl --user restart frame-camera-access-prompt.service || true
echo "permission prompt: user service frame-camera-access-prompt (journalctl --user -u frame-camera-access-prompt)"
echo "installed $prefix/bin/frame-camera-accessd (allowed uid $uid). Check: build/fca_probe --frames 100"
