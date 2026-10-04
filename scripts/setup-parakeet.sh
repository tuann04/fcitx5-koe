#!/usr/bin/env bash
# Set up a Parakeet backend for koe: start the sherpa-onnx websocket server
# plus the OpenAI-compatible shim, both as user services.
#
# Usage:
#   ./scripts/setup-parakeet.sh [--dry-run] [--ws-port 6006] [--http-port 8179]
#                               [--model-dir DIR] [--sherpa-bin PATH]
#                               [--threads 4]
#
# Needs sherpa-onnx-offline-websocket-server on PATH (sherpa-onnx package)
# or --sherpa-bin pointing at it, plus a transducer model dir holding
# encoder/decoder/joiner .onnx files and tokens.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS_PORT=6006
HTTP_PORT=8179
MODEL_DIR="$HOME/.local/share/koe/models/parakeet-v2"
SHERPA_BIN="${SHERPA_BIN:-$(command -v sherpa-onnx-offline-websocket-server || true)}"
THREADS=4
DRY_RUN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --ws-port) WS_PORT="$2"; shift ;;
    --ws-port=*) WS_PORT="${1#--ws-port=}" ;;
    --http-port) HTTP_PORT="$2"; shift ;;
    --http-port=*) HTTP_PORT="${1#--http-port=}" ;;
    --model-dir) MODEL_DIR="$2"; shift ;;
    --model-dir=*) MODEL_DIR="${1#--model-dir=}" ;;
    --sherpa-bin) SHERPA_BIN="$2"; shift ;;
    --sherpa-bin=*) SHERPA_BIN="${1#--sherpa-bin=}" ;;
    --threads) THREADS="$2"; shift ;;
    --threads=*) THREADS="${1#--threads=}" ;;
    -h|--help)
      echo "usage: setup-parakeet.sh [--dry-run] [--ws-port 6006] [--http-port 8179]"
      echo "       [--model-dir DIR] [--sherpa-bin PATH] [--threads 4]"
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

if [ -z "$SHERPA_BIN" ] || [ ! -x "$SHERPA_BIN" ]; then
  echo "no sherpa-onnx-offline-websocket-server found." >&2
  echo "Install sherpa-onnx, or pass --sherpa-bin PATH." >&2
  exit 1
fi

for f in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
  if [ ! -f "$MODEL_DIR/$f" ]; then
    echo "missing model file: $MODEL_DIR/$f" >&2
    echo "Get a transducer model, e.g.:" >&2
    echo "  https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2" >&2
    echo "and pass its dir with --model-dir DIR." >&2
    exit 1
  fi
done

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required for the shim." >&2
  exit 1
fi

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
info "install koe-parakeet services in $UNIT_DIR..."
run mkdir -p "$UNIT_DIR"

write_unit() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: write $1"
    return
  fi
  cat > "$1"
}

LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/koe"
run mkdir -p "$LOG_DIR"

write_unit "$UNIT_DIR/koe-parakeet-sherpa.service" <<EOF
[Unit]
Description=sherpa-onnx websocket server (Parakeet) for koe

[Service]
ExecStart=$SHERPA_BIN --port=$WS_PORT --num-work-threads=$THREADS --tokens=$MODEL_DIR/tokens.txt --encoder=$MODEL_DIR/encoder.int8.onnx --decoder=$MODEL_DIR/decoder.int8.onnx --joiner=$MODEL_DIR/joiner.int8.onnx --log-file=$LOG_DIR/sherpa-ws.log
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical-session.target
EOF

write_unit "$UNIT_DIR/koe-parakeet-shim.service" <<EOF
[Unit]
Description=OpenAI-compatible shim for Parakeet (koe)
After=koe-parakeet-sherpa.service
Wants=koe-parakeet-sherpa.service

[Service]
ExecStart=/usr/bin/env python3 $ROOT/scripts/parakeet-shim.py --port $HTTP_PORT --upstream 127.0.0.1:$WS_PORT
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF

run systemctl --user daemon-reload
run systemctl --user enable --now koe-parakeet-sherpa.service
run systemctl --user enable --now koe-parakeet-shim.service

cat <<EOF

Done. Add this profile to ~/.config/koe/config.toml:

  [profile.parakeet]
  base_url = "http://127.0.0.1:$HTTP_PORT"
  model = "parakeet-tdt-0.6b-v2"
  timeout_sec = 15
  partial_interval_ms = 700
  prompt = ""

Then set active = "parakeet" and restart koe-daemon.
Check: curl -sf http://127.0.0.1:$HTTP_PORT/health
EOF
