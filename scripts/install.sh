#!/usr/bin/env bash
# Single-command installer for fcitx5-koe.
#
# Usage:
#   ./scripts/install.sh [--cloud-only] [--dry-run] [--no-deps] [--prefix DIR]
#
# Plain cmake build with distro packages (Arch, Ubuntu 24.04+, Fedora).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLOUD_ONLY=0
DRY_RUN=0
NO_DEPS=0
PREFIX="$HOME/.local"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --cloud-only) CLOUD_ONLY=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --native) ;;
    --no-deps) NO_DEPS=1 ;;
    --prefix) PREFIX="$2"; shift ;;
    --prefix=*) PREFIX="${1#--prefix=}" ;;
    -h|--help)
      echo "usage: install.sh [--cloud-only] [--dry-run] [--no-deps] [--prefix DIR]"
      echo "  --cloud-only  skip models download and local llama-server (use a cloud API profile)"
      echo "  --dry-run     print what would happen without changing anything"
      echo "  --no-deps     skip distro package install (deps already present)"
      echo "  --prefix DIR  install prefix (default: \$HOME/.local)"
      exit 0
      ;;
    *) echo "unknown flag: $1 (try --help)" >&2; exit 1 ;;
  esac
  shift
done

info() { echo "==> $*"; }
run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: $*"
  else
    "$@"
  fi
}

# ---- native deps (Arch / Ubuntu 24.04+ / Fedora) ----
install_native_deps() {
  if [ "$NO_DEPS" -eq 1 ]; then
    info "skip distro packages (--no-deps)"
    return
  fi
  if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
    echo "need sudo to install packages, or re-run with --no-deps." >&2
    exit 1
  fi
  if command -v pacman >/dev/null 2>&1; then
    run sudo pacman -S --needed --noconfirm \
      fcitx5 extra-cmake-modules pipewire curl nlohmann-json tomlplusplus \
      cmake pkgconf gcc make
  elif command -v apt-get >/dev/null 2>&1; then
    if [ -f /etc/os-release ]; then
      # shellcheck disable=SC1091
      . /etc/os-release
      if [ "${ID:-}" = "ubuntu" ] && [ "${VERSION_ID:-0}" \< "24.04" ]; then
        echo "libtomlplusplus-dev needs Ubuntu 24.04+." >&2
        exit 1
      fi
    fi
    run sudo apt-get update
    run sudo apt-get install -y \
      fcitx5 libfcitx5core-dev fcitx5-modules-dev \
      libpipewire-0.3-dev libcurl4-openssl-dev nlohmann-json3-dev libtomlplusplus-dev \
      extra-cmake-modules cmake pkg-config g++ curl pipewire
  elif command -v dnf >/dev/null 2>&1; then
    run sudo dnf install -y \
      fcitx5 fcitx5-devel pipewire-devel libcurl-devel json-devel tomlplusplus-devel \
      extra-cmake-modules cmake pkgconf-pkg-config gcc-c++ make curl
  else
    echo "no supported package manager (pacman, apt-get, dnf)." >&2
    echo "On NixOS, see docs/5-deployment.md (NixOS module)." >&2
    echo "Install by hand: fcitx5 + dev files, pipewire dev, curl dev," >&2
    echo "nlohmann-json dev, tomlplusplus dev, extra-cmake-modules, cmake, pkg-config, a C++ compiler." >&2
    exit 1
  fi
}

install_native_deps
missing=()
for cmd in curl systemctl fcitx5 cmake g++ pkg-config; do
  command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if [ "${#missing[@]}" -gt 0 ]; then
  echo "still missing: ${missing[*]}. Install them and re-run with --no-deps." >&2
  exit 1
fi

# 1. Models (local mode only).
if [ "$CLOUD_ONLY" -eq 1 ]; then
  info "cloud-only: skip models download"
else
  info "models..."
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: ./scripts/setup-models.sh download qwen"
  else
    "$ROOT/scripts/setup-models.sh" download qwen
  fi
fi

# 2. Build.
BIN=""
info "build into $PREFIX..."
run cmake -B "$ROOT/build" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$PREFIX"
run cmake --build "$ROOT/build"
run cmake --install "$ROOT/build"
BIN="$PREFIX/bin/koe-daemon"

# 3. Install user services.
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

# Locate the addon dir (multiarch libdir differs per distro).
ADDON_DIR=""
if [ "$DRY_RUN" -eq 0 ]; then
  SO="$(find "$PREFIX" -name 'libkoe.so' 2>/dev/null | head -1)"
  [ -n "$SO" ] || { echo "libkoe.so not found under $PREFIX, install failed." >&2; exit 1; }
  ADDON_DIR="$(dirname "$SO")"
  info "addon dir: $ADDON_DIR"
else
  ADDON_DIR="$PREFIX/lib/fcitx5  (resolved after install)"
fi

info "install user services in $UNIT_DIR..."
run mkdir -p "$UNIT_DIR"

LLAMA_BIN=""
for cand in "$HOME/.local/share/koe/llama-cpp-cuda/bin/llama-server" "$(command -v llama-server || true)"; do
  [ -n "$cand" ] && [ -x "$cand" ] && LLAMA_BIN="$cand" && break
done

if [ "$CLOUD_ONLY" -eq 0 ] && [ -z "$LLAMA_BIN" ]; then
  echo "no llama-server found." >&2
  echo "Install your distro's llama.cpp package, or re-run with --cloud-only" >&2
  echo "and point ~/.config/koe/config.toml at a cloud profile." >&2
  exit 1
fi

daemon_after="pipewire.service"
daemon_wants="pipewire.service"
if [ "$CLOUD_ONLY" -eq 0 ]; then
  daemon_after="$daemon_after koe-llama-server.service"
  daemon_wants="$daemon_wants koe-llama-server.service"
  info "llama-server: $LLAMA_BIN"
fi

write_unit() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: write $1"
    return
  fi
  cat > "$1"
}

write_unit "$UNIT_DIR/koe-daemon.service" <<EOF
[Unit]
Description=koe speech-to-text daemon for fcitx5
After=$daemon_after
Wants=$daemon_wants

[Service]
ExecStart=$BIN
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF

if [ "$CLOUD_ONLY" -eq 0 ]; then
  MODELS="$HOME/.local/share/koe/models"
  write_unit "$UNIT_DIR/koe-llama-server.service" <<EOF
[Unit]
Description=llama-server running Qwen3-ASR for koe

[Service]
ExecStart=$LLAMA_BIN -m $MODELS/Qwen3-ASR-1.7B-Q8_0.gguf --mmproj $MODELS/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf -ngl 99 --host 127.0.0.1 --port 8178 -np 1 -c 4096
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical-session.target
EOF
  run systemctl --user daemon-reload
  run systemctl --user enable --now koe-llama-server.service
fi
run systemctl --user daemon-reload
run systemctl --user enable --now koe-daemon.service

# Installs outside the default search path need env for fcitx5.
ENVD_DIR="$HOME/.config/environment.d"
info "addon env in $ENVD_DIR..."
run mkdir -p "$ENVD_DIR"
if [ "$DRY_RUN" -eq 0 ]; then
  cat > "$ENVD_DIR/koe-fcitx5.conf" <<EOF
FCITX_ADDON_DIRS=$ADDON_DIR
XDG_DATA_DIRS=$PREFIX/share:/usr/local/share:/usr/share
EOF
else
  echo "dry-run: write $ENVD_DIR/koe-fcitx5.conf"
fi
cat <<EOF

Done. Log out and back in so fcitx5 picks up the addon path,
or try it right now without relogin:

  FCITX_ADDON_DIRS="$ADDON_DIR" XDG_DATA_DIRS="$PREFIX/share:\$XDG_DATA_DIRS" fcitx5 -rd

Then hold Right Ctrl in a text field and speak.
Check: systemctl --user status koe-daemon
EOF
