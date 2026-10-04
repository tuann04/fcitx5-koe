#!/usr/bin/env bash
# Remove what install.sh and the setup-*.sh scripts installed:
# user services, the fcitx5 env file, and koe files under the prefix.
# Config (~/.config/koe) and models (~/.local/share/koe/models) are kept
# unless asked otherwise.
#
# Usage:
#   ./scripts/uninstall.sh [--dry-run] [--prefix DIR]
#                          [--with-models] [--with-config]
set -euo pipefail

PREFIX="$HOME/.local"
DRY_RUN=0
WITH_MODELS=0
WITH_CONFIG=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --prefix) PREFIX="$2"; shift ;;
    --prefix=*) PREFIX="${1#--prefix=}" ;;
    --with-models) WITH_MODELS=1 ;;
    --with-config) WITH_CONFIG=1 ;;
    -h|--help)
      echo "usage: uninstall.sh [--dry-run] [--prefix DIR] [--with-models] [--with-config]"
      echo "  --with-models  also delete ~/.local/share/koe/models"
      echo "  --with-config  also delete ~/.config/koe"
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

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

info "stop and remove koe user services..."
for unit in koe-daemon koe-llama-server koe-whisper \
            koe-parakeet-sherpa koe-parakeet-shim; do
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: systemctl --user disable --now $unit.service (if present)"
  else
    systemctl --user disable --now "$unit.service" 2>/dev/null || true
  fi
  run rm -f "$UNIT_DIR/$unit.service"
done
run systemctl --user daemon-reload

info "remove fcitx5 env file..."
run rm -f "$HOME/.config/environment.d/koe-fcitx5.conf"

info "remove installed files under $PREFIX..."
run rm -f "$PREFIX/bin/koe-daemon"
for libdir in "$PREFIX/lib/fcitx5" "$PREFIX/lib/x86_64-linux-gnu/fcitx5" \
             "$PREFIX/lib64/fcitx5"; do
  run rm -f "$libdir/libkoe.so"
done
run rm -f "$PREFIX/share/fcitx5/addon/koe.conf"
run rm -rf "$PREFIX/share/doc/fcitx5-koe"

if [ "$WITH_MODELS" -eq 1 ]; then
  info "remove models..."
  run rm -rf "$HOME/.local/share/koe/models"
else
  info "keep models in ~/.local/share/koe/models (use --with-models to delete)"
fi

if [ "$WITH_CONFIG" -eq 1 ]; then
  info "remove config..."
  run rm -rf "$HOME/.config/koe"
else
  info "keep config in ~/.config/koe (use --with-config to delete)"
fi

cat <<EOF

Done. Log out and back in so fcitx5 forgets the addon path.
EOF
