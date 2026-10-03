#!/usr/bin/env bash
# Start llama-server, koe-daemon and fcitx5 (with the koe addon) for manual testing.
# Ctrl+C stops what this script started and restarts fcitx5 without the addon.
#
# Usage: ./scripts/dev-run.sh
# Advanced: ./scripts/dev-run.sh --nix (uses nix-build instead of cmake).
set -euo pipefail

NATIVE=1
for arg in "$@"; do
  case "$arg" in
    --native) NATIVE=1 ;;
    --nix) NATIVE=0 ;;
    -h|--help)
      echo "usage: dev-run.sh [--native] [--nix]"
      exit 0
      ;;
    *) echo "unknown flag: $arg (try --help)" >&2; exit 1 ;;
  esac
done
if ! command -v nix-build >/dev/null 2>&1; then
  NATIVE=1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODELS="$HOME/.local/share/koe/models"
LLAMA="$HOME/.local/share/koe/llama-cpp-cuda/bin/llama-server"
[ -x "$LLAMA" ] || LLAMA="$(command -v llama-server || true)"
PORT=8178
SOCK="${XDG_RUNTIME_DIR:-/tmp}/koe.sock"
LOGS="${XDG_RUNTIME_DIR:-/tmp}/koe-dev"
mkdir -p "$LOGS"

pids=()
cleanup() {
    echo "stopping..."
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    fcitx5 -rd >/dev/null 2>&1 || true
    echo "fcitx5 restarted without the koe addon"
}
trap cleanup EXIT
trap 'exit 0' INT TERM

echo "building..."
if [ "$NATIVE" -eq 1 ]; then
    DEVROOT="$LOGS/root"
    cmake -B "$ROOT/build" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build "$ROOT/build" >/dev/null
    rm -rf "$DEVROOT"
    cmake --install "$ROOT/build" --prefix "$DEVROOT" >/dev/null
    SO="$(find "$DEVROOT" -name 'libkoe.so' | head -1)"
    [ -n "$SO" ] || { echo "libkoe.so missing after install, build failed." >&2; exit 1; }
    ADDON_LIBDIR="$(dirname "$SO")"
    ADDON_SHARE="$DEVROOT/share"
    DAEMON_BIN="$DEVROOT/bin/koe-daemon"
else
    nix-build "$ROOT" -o "$ROOT/result" >/dev/null
    ADDON_LIBDIR="$ROOT/result/lib/fcitx5"
    ADDON_SHARE="$ROOT/result/share"
    DAEMON_BIN="$ROOT/result/bin/koe-daemon"
fi

if [ -z "$LLAMA" ]; then
    echo "no llama-server found (tried the local CUDA build and PATH)." >&2
    echo "Build llama.cpp, install your distro's llama.cpp package, or test against a cloud endpoint." >&2
    exit 1
fi

if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; then
    echo "llama-server already running on :$PORT"
else
    echo "starting llama-server..."
    "$LLAMA" -m "$MODELS/Qwen3-ASR-1.7B-Q8_0.gguf" \
        --mmproj "$MODELS/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf" \
        -ngl 99 --host 127.0.0.1 --port "$PORT" -np 1 -c 4096 \
        >"$LOGS/llama.log" 2>&1 &
    pids+=($!)
    until curl -sf "http://127.0.0.1:$PORT/health" >/dev/null; do
        kill -0 "${pids[-1]}" 2>/dev/null || { echo "llama-server died, see $LOGS/llama.log"; exit 1; }
        sleep 1
    done
fi

if pgrep -x koe-daemon >/dev/null; then
    echo "koe-daemon already running"
else
    echo "starting koe-daemon..."
    "$DAEMON_BIN" -v >"$LOGS/daemon.log" 2>&1 &
    pids+=($!)
    until [ -S "$SOCK" ]; do
        kill -0 "${pids[-1]}" 2>/dev/null || { echo "koe-daemon died, see $LOGS/daemon.log"; exit 1; }
        sleep 0.3
    done
fi

echo "restarting fcitx5 with the koe addon..."
FCITX_ADDON_DIRS="$ADDON_LIBDIR" \
XDG_DATA_DIRS="$ADDON_SHARE:${XDG_DATA_DIRS:-}" \
    fcitx5 -rd --verbose 'koe=5' >"$LOGS/fcitx5.log" 2>&1

echo
echo "ready: hold Right Ctrl in a text field to dictate. Ctrl+C to stop."
echo "logs: $LOGS"
echo
tail -F "$LOGS/daemon.log"
