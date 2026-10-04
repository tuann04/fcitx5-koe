#!/usr/bin/env bash
# One entry point for backend setup: pick a model, this script downloads it,
# installs its server service, and points the daemon at it.
#
# Usage:
#   ./scripts/setup.sh [--dry-run] [--model qwen|whisper|parakeet] [--no-set-active]
#
# Without --model it asks. Without --no-set-active it sets active = "<profile>"
# in ~/.config/koe/config.toml (backup at config.toml.bak) and restarts
# koe-daemon if its user service is already running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODEL=""
DRY_RUN=0
SET_ACTIVE=1
EXTRA=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; EXTRA+=(--dry-run) ;;
    --model) MODEL="$2"; shift ;;
    --model=*) MODEL="${1#--model=}" ;;
    --no-set-active) SET_ACTIVE=0 ;;
    -h|--help)
      echo "usage: setup.sh [--dry-run] [--model qwen|whisper|parakeet] [--no-set-active]"
      exit 0
      ;;
    *) echo "unknown flag: $1 (try --help)" >&2; exit 1 ;;
  esac
  shift
done

info() { echo "==> $*"; }

if [ -z "$MODEL" ]; then
  echo "Pick a backend:"
  select MODEL in qwen whisper parakeet; do
    [ -n "$MODEL" ] && break
    echo "pick 1, 2, or 3." >&2
  done
fi

case "$MODEL" in
  qwen) PROFILE="local" ;;
  whisper) PROFILE="whisper" ;;
  parakeet) PROFILE="parakeet" ;;
  *)
    echo "unknown model: $MODEL (want qwen, whisper, or parakeet)" >&2
    exit 1 ;;
esac

info "backend: $MODEL (profile: $PROFILE)"
"$ROOT/scripts/setup-models.sh" download "$MODEL" "${EXTRA[@]}"
"$ROOT/scripts/setup-$MODEL.sh" "${EXTRA[@]}"

if [ "$SET_ACTIVE" -eq 0 ]; then
  info "skip config change (--no-set-active)"
  exit 0
fi

CONFIG="$HOME/.config/koe/config.toml"
if [ "$DRY_RUN" -eq 1 ]; then
  echo "dry-run: set active = \"$PROFILE\" in $CONFIG, restart koe-daemon if running"
  exit 0
fi

mkdir -p "$(dirname "$CONFIG")"
[ -f "$CONFIG" ] || printf 'active = "%s"\n' "$PROFILE" > "$CONFIG"
cp "$CONFIG" "$CONFIG.bak"
if grep -q '^active[[:space:]]*=' "$CONFIG"; then
  sed -i "s/^active[[:space:]]*=.*/active = \"$PROFILE\"/" "$CONFIG"
else
  sed -i "1i active = \"$PROFILE\"" "$CONFIG"
fi
info "active = \"$PROFILE\" in $CONFIG (backup: $CONFIG.bak)"

if systemctl --user is-active --quiet koe-daemon.service 2>/dev/null; then
  systemctl --user restart koe-daemon.service
  info "koe-daemon restarted"
else
  info "koe-daemon service not running; restart it yourself"
  info "(dev-run.sh users: Ctrl+C and re-run dev-run.sh)"
fi
