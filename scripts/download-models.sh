#!/usr/bin/env bash
# Download Qwen3-ASR model files for koe.
# Models stay outside the Nix store on purpose (about 2.5 GB).
# Usage: ./scripts/download-models.sh [target-dir]
set -euo pipefail

MODELS="${1:-${MODELS_DIR:-$HOME/.local/share/koe/models}}"
BASE="https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main"
FILES=(
  "Qwen3-ASR-1.7B-Q8_0.gguf"
  "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf"
)

mkdir -p "$MODELS"

for f in "${FILES[@]}"; do
  dest="$MODELS/$f"
  if [ -s "$dest" ]; then
    echo "exists, skip: $dest"
    continue
  fi
  echo "downloading $f..."
  curl -fL --progress-bar -o "$dest.tmp" "$BASE/$f"
  mv "$dest.tmp" "$dest"
done

echo "models in $MODELS:"
ls -lh "$MODELS"
