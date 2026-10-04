#!/usr/bin/env bash
# Set up a Whisper backend for koe: download the model, install and start
# a whisper-server user service speaking POST /v1/audio/transcriptions.
#
# Usage:
#   ./scripts/setup-whisper.sh [--dry-run] [--port 8180] [--model large-v3-turbo]
#                              [--models-dir DIR] [--whisper-bin PATH] [--no-download]
#
# Needs whisper-server on PATH (whisper.cpp) or --whisper-bin pointing at it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT=8180
MODEL="large-v3-turbo"
MODELS_DIR="$HOME/.local/share/koe/models/whisper"
WHISPER_BIN="${WHISPER_BIN:-$(command -v whisper-server || true)}"
DRY_RUN=0
DOWNLOAD=1

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --port) PORT="$2"; shift ;;
    --port=*) PORT="${1#--port=}" ;;
    --model) MODEL="$2"; shift ;;
    --model=*) MODEL="${1#--model=}" ;;
    --models-dir) MODELS_DIR="$2"; shift ;;
    --models-dir=*) MODELS_DIR="${1#--models-dir=}" ;;
    --whisper-bin) WHISPER_BIN="$2"; shift ;;
    --whisper-bin=*) WHISPER_BIN="${1#--whisper-bin=}" ;;
    --no-download) DOWNLOAD=0 ;;
    -h|--help)
      echo "usage: setup-whisper.sh [--dry-run] [--port 8180] [--model large-v3-turbo]"
      echo "       [--models-dir DIR] [--whisper-bin PATH] [--no-download]"
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

if [ -z "$WHISPER_BIN" ] || [ ! -x "$WHISPER_BIN" ]; then
  echo "no whisper-server found." >&2
  echo "Install whisper.cpp, or pass --whisper-bin PATH." >&2
  exit 1
fi

MODEL_FILE="$MODELS_DIR/ggml-$MODEL.bin"
if [ "$DOWNLOAD" -eq 1 ]; then
  info "models in $MODELS_DIR..."
  run mkdir -p "$MODELS_DIR"
  if [ -s "$MODEL_FILE" ]; then
    info "exists, skip: $MODEL_FILE"
  else
    DL_SCRIPT="$(dirname "$WHISPER_BIN")/whisper-cpp-download-ggml-model"
    if [ -x "$DL_SCRIPT" ]; then
      info "downloading $MODEL..."
      if [ "$DRY_RUN" -eq 1 ]; then
        echo "dry-run: (cd $MODELS_DIR && $DL_SCRIPT $MODEL)"
      else
        (cd "$MODELS_DIR" && "$DL_SCRIPT" "$MODEL")
      fi
    else
      info "downloading $MODEL via curl..."
      run curl -fL --progress-bar -o "$MODEL_FILE" \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"
    fi
  fi
fi

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
info "install koe-whisper.service in $UNIT_DIR..."
run mkdir -p "$UNIT_DIR"

write_unit() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: write $1"
    return
  fi
  cat > "$1"
}

write_unit "$UNIT_DIR/koe-whisper.service" <<EOF
[Unit]
Description=whisper-server for koe speech-to-text

[Service]
ExecStart=$WHISPER_BIN -m $MODEL_FILE --host 127.0.0.1 --port $PORT --inference-path /v1/audio/transcriptions
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF

run systemctl --user daemon-reload
run systemctl --user enable --now koe-whisper.service

cat <<EOF

Done. Add this profile to ~/.config/koe/config.toml:

  [profile.whisper]
  base_url = "http://127.0.0.1:$PORT"
  model = "$MODEL"
  timeout_sec = 30
  partial_interval_ms = 0

Then set active = "whisper" and restart koe-daemon.
Partials are off: this build runs on CPU, too slow for live preview.
Check: curl -sf http://127.0.0.1:$PORT/ ; systemctl --user status koe-whisper
EOF
