#!/usr/bin/env bash
# Set up the Qwen backend for koe: download the models, install and start
# a llama-server user service speaking POST /v1/audio/transcriptions.
#
# Usage:
#   ./scripts/setup-qwen.sh [--dry-run] [--port 8178] [--models-dir DIR]
#                           [--llama-bin PATH] [--no-download]
#
# Needs llama-server on PATH (llama.cpp with CUDA) or --llama-bin pointing
# at it. Model download is delegated to setup-models.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT=8178
MODELS_DIR="$HOME/.local/share/koe/models"
LLAMA_BIN="${LLAMA_BIN:-}"
if [ -z "$LLAMA_BIN" ]; then
  for cand in "$HOME/.local/share/koe/llama-cpp-cuda/bin/llama-server" \
              "$(command -v llama-server || true)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then
      LLAMA_BIN="$cand"
      break
    fi
  done
fi
DRY_RUN=0
DOWNLOAD=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --port) PORT="$2"; shift ;;
    --port=*) PORT="${1#--port=}" ;;
    --models-dir) MODELS_DIR="$2"; shift ;;
    --models-dir=*) MODELS_DIR="${1#--models-dir=}" ;;
    --llama-bin) LLAMA_BIN="$2"; shift ;;
    --llama-bin=*) LLAMA_BIN="${1#--llama-bin=}" ;;
    --no-download) DOWNLOAD=0 ;;
    -h|--help)
      echo "usage: setup-qwen.sh [--dry-run] [--port 8178] [--models-dir DIR]"
      echo "       [--llama-bin PATH] [--no-download]"
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

if [ -z "$LLAMA_BIN" ] || [ ! -x "$LLAMA_BIN" ]; then
  echo "no llama-server found." >&2
  echo "Install llama.cpp with CUDA, or pass --llama-bin PATH." >&2
  exit 1
fi

if [ "$DOWNLOAD" -eq 1 ]; then
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: $ROOT/scripts/setup-models.sh download qwen --models-dir $MODELS_DIR"
  else
    "$ROOT/scripts/setup-models.sh" download qwen --models-dir "$MODELS_DIR"
  fi
fi

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
info "install koe-llama-server.service in $UNIT_DIR..."
run mkdir -p "$UNIT_DIR"

write_unit() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: write $1"
    return
  fi
  cat > "$1"
}

write_unit "$UNIT_DIR/koe-llama-server.service" <<EOF
[Unit]
Description=llama-server running Qwen3-ASR for koe

[Service]
ExecStart=$LLAMA_BIN -m $MODELS_DIR/Qwen3-ASR-1.7B-Q8_0.gguf --mmproj $MODELS_DIR/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf -ngl 99 --host 127.0.0.1 --port $PORT -np 1 -c 4096
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

run systemctl --user daemon-reload
run systemctl --user enable --now koe-llama-server.service

cat <<EOF

Done. Add this profile to ~/.config/koe/config.toml:

  [profile.local]
  base_url = "http://127.0.0.1:$PORT"
  model = "qwen3-asr-1.7b"
  timeout_sec = 15
  partial_interval_ms = 700
  prompt = ""

Then set active = "local" and restart koe-daemon.
Check: curl -sf http://127.0.0.1:$PORT/health
EOF
