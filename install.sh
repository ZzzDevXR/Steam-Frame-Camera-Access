#!/usr/bin/env bash
# Frame Camera Access installer for the Steam Frame. In a console on the headset:
#
#   curl -fsSL https://raw.githubusercontent.com/ZzzDevXR/Steam-Frame-Camera-Access/main/install.sh | sudo bash
#
# It downloads the prebuilt release to ~/Steam-Frame-Camera-Access (FRAME_INSTALL_DIR sets another parent
# folder) and installs the camera service as root. Run it again to update.
#   --build   clone the source with git and compile it on the headset instead (... | sudo bash -s -- --build)
# From a folder you already have: sudo bash install.sh (compiles first if the folder has no binaries).
# FCA_BUNDLE=<file or URL> uses that release bundle instead of the latest one (sudo env FCA_BUNDLE=... bash).
set -euo pipefail

repository=https://github.com/ZzzDevXR/Steam-Frame-Camera-Access
asset=frame-camera-access-aarch64.tar.gz
self=${BASH_SOURCE[0]:-}
scratch=

die() { echo "error: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
cleanup() { if [[ -n $scratch && -d $scratch ]]; then rm -rf -- "$scratch"; fi; }
trap cleanup EXIT

main() {
  local build=0
  while (($#)); do
    case $1 in
      --build) build=1; shift ;;
      -h | --help) sed -n '2,10s/^# \{0,1\}//p' "$self" 2> /dev/null || echo "see $repository"; return 0 ;;
      *) die "Unknown argument: $1" ;;
    esac
  done
  [[ $EUID -eq 0 ]] || die "run it with sudo (the camera service is installed as root)"
  local uid=${SUDO_UID:-}
  [[ -n $uid && $uid != 0 ]] || die "run it through sudo from your own account, not from a root shell"
  local user home
  user=$(id -nu "$uid")
  home=$(getent passwd "$uid" | cut -d: -f6)
  [[ -d $home ]] || die "no home folder for $user"
  [[ -d /run/user/$uid ]] || die "$user has no running session (/run/user/$uid)"
  as_user() {
    sudo -u "$user" env HOME="$home" XDG_RUNTIME_DIR="/run/user/$uid" \
      DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" GIT_TERMINAL_PROMPT=0 "$@"
  }
  [[ $(uname -m) = aarch64 ]] || die "this runs on the Steam Frame (Linux aarch64)"
  [[ -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]] || die "SteamVR not found (/opt/steamvr)"

  require() { # tools...
    local missing=() tool
    for tool in "$@"; do command -v "$tool" > /dev/null || missing+=("$tool"); done
    if ((${#missing[@]})); then
      printf 'Missing on this headset:\n' >&2
      printf '  %s\n' "${missing[@]}" >&2
      die "install them and run this command again"
    fi
  }
  require_compilers() {
    require cmake make gcc g++ pkg-config
    pkg-config --exists cairo freetype2 vulkan || die "the cairo, freetype2 and vulkan development files are missing"
  }
  clone() { # url folder
    if [[ -d $2/.git ]]; then
      step "Updating $2"
      as_user git -C "$2" pull --quiet --ff-only || die "could not update $2 (local changes?)"
    elif [[ -e $2 ]]; then
      die "$2 exists but is not a git checkout; move it away or set FRAME_INSTALL_DIR"
    else
      step "Downloading the source to $2"
      as_user git clone --quiet --depth 1 "$1" "$2"
    fi
  }
  unpack() { # bundle-file-or-url folder: replaces the folder with the release bundle
    local source=$1 folder=$2 expected actual
    [[ ! -e $folder || -f $folder/RELEASE ]] ||
      die "$folder exists and was not put there by this installer; move it away or set FRAME_INSTALL_DIR"
    as_user mkdir -p "$(dirname -- "$folder")"
    scratch=$(as_user mktemp -d "$(dirname -- "$folder")/.frame-install.XXXXXX")
    step "Downloading $(basename -- "$folder")"
    if [[ -f $source ]]; then
      as_user cp "$source" "$scratch/bundle.tar.gz"
      as_user cp "$source.sha256" "$scratch/bundle.sha256" || die "no $source.sha256 next to the bundle"
    else
      as_user curl -fsSL --retry 3 -o "$scratch/bundle.tar.gz" "$source" || die "could not download $source"
      as_user curl -fsSL --retry 3 -o "$scratch/bundle.sha256" "$source.sha256" || die "could not download $source.sha256"
    fi
    expected=$(cut -d' ' -f1 "$scratch/bundle.sha256")
    actual=$(sha256sum "$scratch/bundle.tar.gz" | cut -d' ' -f1)
    [[ -n $expected && $expected == "$actual" ]] || die "the download does not match its SHA-256 ($source)"
    as_user mkdir "$scratch/new"
    as_user tar -xzf "$scratch/bundle.tar.gz" -C "$scratch/new"
    [[ -f $scratch/new/RELEASE ]] || die "$source is not a release bundle"
    if [[ -e $folder ]]; then as_user rm -rf -- "$folder"; fi
    as_user mv "$scratch/new" "$folder"
    cleanup
    echo "release $(cat "$folder/RELEASE")"
  }

  local root
  if [[ -n $self && -f $self && -f $(dirname -- "$self")/CMakeLists.txt ]]; then
    root=$(cd -- "$(dirname -- "$self")" && pwd)
  else
    root=${FRAME_INSTALL_DIR:-$home}/Steam-Frame-Camera-Access
    cd "$home"
    if ((build)); then
      require git
      require_compilers
      clone "$repository" "$root"
    else
      require curl tar sha256sum
      unpack "${FCA_BUNDLE:-$repository/releases/latest/download/$asset}" "$root"
    fi
  fi

  if ((build)) || [[ ! -f $root/build/frame-camera-accessd ]]; then
    require_compilers
    step "Building"
    as_user bash "$root/scripts/build.sh"
  fi
  step "Installing the camera service"
  bash "$root/scripts/install-daemon.sh"
  printf '\nFrame Camera Access is installed. Check it: %s/build/fca_probe --frames 100\n' "$root"
  printf 'Remove it: sudo bash %s/scripts/uninstall-daemon.sh\n' "$root"
}

main "$@"
