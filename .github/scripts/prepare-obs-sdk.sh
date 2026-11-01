#!/usr/bin/env bash
# =============================================================================
# Prepares the OBS Studio SDK the plugin is compiled against.
#
#   .github/scripts/prepare-obs-sdk.sh [--install-deps] [DEST]
#
# Clones OBS Studio at the floor version (32.0.x), configures it and builds
# only the two targets a third-party plugin links against: libobs and
# obs-frontend-api. Used by .github/workflows/release.yml; runs the same way on
# a developer machine (Linux, macOS, or Git Bash on Windows).
#
#   --install-deps   Linux only: apt-get install the OBS build dependencies
#                    of Ubuntu 24.04 (uses sudo).
#   DEST             where to clone OBS. Default: $RUNNER_TEMP/obs-studio in
#                    CI, ./.obs-sdk/obs-studio otherwise.
#
# Prints the variables read by build_plugin.sh / build_plugin_macos.sh /
# build_plugin.bat as KEY=VALUE lines, and appends them to $GITHUB_ENV in CI:
#   OBS_SRC, OBS_BUILD                    (all platforms)
#   OBS_DEPS, OBS_QT6_DEPS                (Windows: obs-deps and its Qt)
#
# WHY 32.0 AND NOT THE LATEST OBS: libobs refuses a module built against a
# NEWER libobs than its own, and OBS changes its Qt within the 32.x line
# (32.0 ships Qt 6.8, 32.2 ships Qt 6.11). Building against the oldest
# supported OBS is what keeps the plugin loadable on every supported OBS. The
# build scripts check this again (FLOOR_OBS / FLOOR_QT) and refuse to build
# otherwise. The patch number does not matter: libobs masks it.
# =============================================================================
set -euo pipefail

OBS_VERSION="${OBS_VERSION:-32.0.4}"
# Commit the tag must point to. A moved tag stops the build instead of
# silently compiling different sources.
OBS_COMMIT="${OBS_COMMIT:-dcdbd2e9048ae66237fdab32dbba72b745b1b9c5}"
NVCODEC_VERSION="${NVCODEC_VERSION:-n12.2.72.0}"
NVCODEC_COMMIT="${NVCODEC_COMMIT:-c69278340ab1d5559c7d7bf0edf615dc33ddbba7}"

INSTALL_DEPS=0
DEST=""
while [ $# -gt 0 ]; do
  case "$1" in
    --install-deps) INSTALL_DEPS=1; shift ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    -*) echo "[ERROR] unknown option: $1" >&2; exit 2 ;;
    *) DEST="$1"; shift ;;
  esac
done

die() { echo "[ERROR] $*" >&2; exit 1; }

case "$(uname -s)" in
  Linux) OS=linux ;;
  Darwin) OS=macos ;;
  MINGW*|MSYS*|CYGWIN*) OS=windows ;;
  *) die "unsupported system: $(uname -s)" ;;
esac

# Windows: cmake.exe is a native program. Hand it C:/style paths instead of
# relying on the MSYS path conversion of Git Bash.
native() { if [ "$OS" = windows ]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
winpath() { if [ "$OS" = windows ]; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }

if [ -z "$DEST" ]; then
  if [ -n "${RUNNER_TEMP:-}" ]; then
    base="$RUNNER_TEMP"
    [ "$OS" = windows ] && base="$(cygpath -u "$RUNNER_TEMP")"
    DEST="$base/obs-studio"
  else
    DEST="$PWD/.obs-sdk/obs-studio"
  fi
fi

emit() {
  printf '%s=%s\n' "$1" "$2"
  if [ -n "${GITHUB_ENV:-}" ]; then printf '%s=%s\n' "$1" "$2" >> "$GITHUB_ENV"; fi
}

# --- Linux build dependencies (Ubuntu 24.04) ---------------------------------
#
# The plugin must use the Qt of the OBS that loads it, so on Linux it is built
# on the distribution it targets: Ubuntu 24.04 LTS, Qt 6.4.2. CMake evaluates
# every find_package() of OBS at configure time, even for targets that are not
# built, hence the full list. libsimde-dev and extra-cmake-modules fail the
# configuration with messages that do not name the package.
install_linux_deps() {
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config ca-certificates \
    extra-cmake-modules curl file binutils \
    libavcodec-dev libavdevice-dev libavfilter-dev libavformat-dev \
    libavutil-dev libswresample-dev libswscale-dev \
    libx264-dev libcurl4-openssl-dev libmbedtls-dev \
    libgl1-mesa-dev libglvnd-dev libgles2-mesa-dev libegl1-mesa-dev \
    libjansson-dev libluajit-5.1-dev python3-dev swig \
    libx11-dev libx11-xcb-dev libxcb1-dev libxcb-randr0-dev libxcb-shm0-dev \
    libxcb-xinerama0-dev libxcb-composite0-dev libxcb-xfixes0-dev \
    libxcomposite-dev libxinerama-dev libxss-dev libxkbcommon-dev \
    libwayland-dev libdrm-dev libpci-dev libudev-dev libv4l-dev \
    libpulse-dev libasound2-dev libjack-jackd2-dev libspeexdsp-dev \
    libpipewire-0.3-dev libva-dev libvpl-dev \
    librist-dev libsrt-openssl-dev libssl-dev \
    qt6-base-dev qt6-base-private-dev libqt6svg6-dev qt6-wayland \
    qt6-image-formats-plugins \
    nlohmann-json3-dev libwebsocketpp-dev libasio-dev uthash-dev \
    libqrcodegencpp-dev libcmocka-dev \
    libgbm-dev libdbus-1-dev libsystemd-dev \
    libxcb-util-dev libxcb-icccm4-dev libxcb-cursor-dev libxcb-keysyms1-dev \
    libsimde-dev libfdk-aac-dev

  # FFnvcodec headers: required by the OBS configuration even though obs-nvenc
  # is never built here. Headers only, pinned commit.
  local tmp
  tmp="$(mktemp -d)"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$NVCODEC_VERSION" \
    https://github.com/FFmpeg/nv-codec-headers.git "$tmp/nv-codec-headers"
  [ "$(git -C "$tmp/nv-codec-headers" rev-parse HEAD)" = "$NVCODEC_COMMIT" ] \
    || die "nv-codec-headers $NVCODEC_VERSION does not point to $NVCODEC_COMMIT"
  sudo make -C "$tmp/nv-codec-headers" install PREFIX=/usr/local
  rm -rf "$tmp"
}

if [ "$INSTALL_DEPS" = 1 ]; then
  [ "$OS" = linux ] || die "--install-deps is only for Linux (Ubuntu 24.04)"
  install_linux_deps
fi

# --- OBS sources ---------------------------------------------------------------
#
# Submodules are required even with the browser disabled: OBS 32 checks for
# obs-browser before ENABLE_BROWSER is read, and win-dshow needs its own
# submodule on Windows. They are source repositories, a few megabytes; CEF is a
# separate download that ENABLE_BROWSER=OFF skips.
if [ ! -d "$DEST/.git" ]; then
  mkdir -p "$(dirname "$DEST")"
  git -c advice.detachedHead=false -c core.longpaths=true clone --quiet \
    --depth 1 --branch "$OBS_VERSION" --recurse-submodules --shallow-submodules \
    https://github.com/obsproject/obs-studio.git "$DEST"
fi
HEAD_COMMIT="$(git -C "$DEST" rev-parse HEAD)"
[ "$HEAD_COMMIT" = "$OBS_COMMIT" ] \
  || die "OBS $OBS_VERSION checkout is $HEAD_COMMIT, expected $OBS_COMMIT"

SRC="$(native "$DEST")"

# CMake 4 dropped compatibility with projects declaring a minimum below 3.5;
# a vendored submodule doing so would stop the configuration. Ignored by CMake 3.
export CMAKE_POLICY_VERSION_MINIMUM="${CMAKE_POLICY_VERSION_MINIMUM:-3.5}"

# --- Configure and build libobs + obs-frontend-api ------------------------------
case "$OS" in
  linux)
    # Same options as the container recipe of the plugin: no browser (no CEF),
    # no WebRTC (avoids the libdatachannel/OpenSSL configuration trap).
    BUILD_DIR="$DEST/build"
    cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DENABLE_BROWSER=OFF \
      -DENABLE_WEBRTC=OFF \
      -DENABLE_AJA=OFF \
      -DENABLE_VLC=OFF \
      -DENABLE_VST=OFF \
      -DENABLE_WAYLAND=ON \
      -DENABLE_PIPEWIRE=ON \
      -DENABLE_UNIT_TESTS=OFF \
      -DBUILD_FOR_DISTRIBUTION=OFF
    cmake --build "$BUILD_DIR" --parallel --target libobs obs-frontend-api
    ;;
  macos)
    # A full Xcode is required by the OBS configuration; DEVELOPER_DIR avoids
    # changing the machine-wide xcode-select setting.
    if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app ]; then
      export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    fi
    # Universal: OBS ships an Apple Silicon and an Intel build, and a
    # single-architecture module loads in only one of them.
    BUILD_DIR="$DEST/build_macos"
    (cd "$DEST" && cmake --preset macos \
      -DENABLE_BROWSER=OFF \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64")
    cmake --build "$BUILD_DIR" --config RelWithDebInfo \
      --target libobs obs-frontend-api -- -quiet
    ;;
  windows)
    # The OBS preset pins Visual Studio 2022 and Windows SDK 10.0.22621, the
    # same toolchain build_plugin.bat uses ("Visual Studio 17 2022").
    BUILD_DIR="$DEST/build_x64"
    (cd "$DEST" && cmake --preset windows-x64 \
      -DENABLE_BROWSER=OFF \
      -DENABLE_SCRIPTING=OFF)
    cmake --build "$(native "$BUILD_DIR")" --config RelWithDebInfo --parallel \
      --target libobs obs-frontend-api
    ;;
esac

[ -f "$BUILD_DIR/libobs/libobsConfig.cmake" ] \
  || die "libobsConfig.cmake missing in $BUILD_DIR/libobs"
[ -f "$BUILD_DIR/frontend/api/obs-frontend-apiConfig.cmake" ] \
  || die "obs-frontend-apiConfig.cmake missing in $BUILD_DIR/frontend/api"

echo "--- OBS SDK (libobs API version) ---"
grep -E 'define LIBOBS_API_(MAJOR|MINOR|PATCH)_VER' "$DEST/libobs/obs-config.h"
echo "------------------------------------"

emit OBS_SRC "$(winpath "$DEST")"
emit OBS_BUILD "$(winpath "$BUILD_DIR")"

if [ "$OS" = windows ]; then
  deps=""; qt6=""
  for d in "$DEST"/.deps/obs-deps-20*-x64; do [ -d "$d" ] && deps="$d"; done
  for d in "$DEST"/.deps/obs-deps-qt6-*-x64; do [ -d "$d" ] && qt6="$d"; done
  [ -n "$deps" ] || die "obs-deps not found in $DEST/.deps"
  [ -n "$qt6" ] || die "obs-deps Qt 6 not found in $DEST/.deps"
  emit OBS_DEPS "$(winpath "$deps")"
  emit OBS_QT6_DEPS "$(winpath "$qt6")"
fi
