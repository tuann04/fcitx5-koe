#!/usr/bin/env bash
# Manage ASR model files on this machine. All models live under
# ~/.local/share/koe/models (Qwen GGUFs at the top, one dir per backend
# otherwise). Models stay out of the Nix store on purpose (gigabytes).
#
# Usage:
#   ./scripts/setup-models.sh [list]
#   ./scripts/setup-models.sh download <name> [--dry-run] [--models-dir DIR]
#   ./scripts/setup-models.sh remove <name> [--dry-run] [--models-dir DIR]
#
# Names: qwen | whisper | parakeet
set -euo pipefail

MODELS_DIR="$HOME/.local/share/koe/models"
DRY_RUN=0
CMD="list"
NAME=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    list|download|remove) CMD="$1" ;;
    --dry-run) DRY_RUN=1 ;;
    --models-dir) MODELS_DIR="$2"; shift ;;
    --models-dir=*) MODELS_DIR="${1#--models-dir=}" ;;
    -h|--help)
      echo "usage: setup-models.sh [list] | download <name> | remove <name>"
      echo "  names: qwen | whisper | parakeet"
      echo "  flags: [--dry-run] [--models-dir DIR]"
      exit 0
      ;;
    -*)
      echo "unknown flag: $1 (try --help)" >&2; exit 1 ;;
    *)
      if [ -z "$NAME" ]; then
        NAME="$1"
      else
        echo "unexpected argument: $1 (try --help)" >&2; exit 1
      fi
      ;;
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

fetch() { # fetch <url> <dest>
  if [ -s "$2" ]; then
    info "exists, skip: $2"
    return
  fi
  info "downloading $(basename "$2")..."
  run curl -fL --progress-bar -o "$2.tmp" "$1"
  if [ "$DRY_RUN" -eq 0 ]; then
    mv "$2.tmp" "$2"
  fi
}

cmd_list() {
  if [ ! -d "$MODELS_DIR" ]; then
    echo "no models dir: $MODELS_DIR"
    return
  fi
  info "models in $MODELS_DIR:"
  run du -sh "$MODELS_DIR"/* 2>/dev/null || true
}

cmd_download_qwen() {
  local base="https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main"
  run mkdir -p "$MODELS_DIR"
  fetch "$base/Qwen3-ASR-1.7B-Q8_0.gguf" "$MODELS_DIR/Qwen3-ASR-1.7B-Q8_0.gguf"
  fetch "$base/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf" \
    "$MODELS_DIR/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf"
}

cmd_download_whisper() {
  local dir="$MODELS_DIR/whisper"
  local model="${WHISPER_MODEL:-large-v3-turbo}"
  run mkdir -p "$dir"
  local dest="$dir/ggml-$model.bin"
  if [ -s "$dest" ]; then
    info "exists, skip: $dest"
    return
  fi
  local bin="${WHISPER_BIN:-$(command -v whisper-server || true)}"
  local dl=""
  [ -n "$bin" ] && dl="$(dirname "$bin")/whisper-cpp-download-ggml-model"
  if [ -n "$dl" ] && [ -x "$dl" ]; then
    info "downloading $model..."
    if [ "$DRY_RUN" -eq 1 ]; then
      echo "dry-run: (cd $dir && $dl $model)"
    else
      (cd "$dir" && "$dl" "$model")
    fi
  else
    fetch "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$model.bin" \
      "$dest"
  fi
}

cmd_download_parakeet() {
  local dir="$MODELS_DIR/parakeet-v2"
  local need=0
  for f in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
    [ -f "$dir/$f" ] || need=1
  done
  if [ "$need" -eq 0 ]; then
    info "exists, skip: $dir"
    return
  fi
  local url="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2"
  run mkdir -p "$dir"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry-run: curl $url | tar -xj -C $dir (flattened)"
    return
  fi
  local tmp
  tmp="$(mktemp -d)"
  # shellcheck disable=SC2064
  trap "rm -rf $tmp" EXIT
  curl -fL --progress-bar -o "$tmp/model.tar.bz2" "$url"
  tar -xjf "$tmp/model.tar.bz2" -C "$tmp"
  local src
  src="$(find "$tmp" -maxdepth 2 -name tokens.txt | head -1 | xargs dirname)"
  cp "$src"/encoder.int8.onnx "$src"/decoder.int8.onnx \
     "$src"/joiner.int8.onnx "$src"/tokens.txt "$dir/"
  if [ -d "$src/test_wavs" ]; then
    cp -r "$src/test_wavs" "$dir/"
  fi
  rm -rf "$tmp"
  trap - EXIT
}

cmd_remove() {
  case "$1" in
    qwen)
      run rm -f "$MODELS_DIR/Qwen3-ASR-1.7B-Q8_0.gguf" \
                  "$MODELS_DIR/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf" ;;
    whisper) run rm -rf "$MODELS_DIR/whisper" ;;
    parakeet) run rm -rf "$MODELS_DIR/parakeet-v2" ;;
    *)
      echo "unknown model: $1 (want qwen, whisper, or parakeet)" >&2
      exit 1 ;;
  esac
}

case "$CMD" in
  list) cmd_list ;;
  download)
    [ -n "$NAME" ] || { echo "download needs a name (try --help)" >&2; exit 1; }
    case "$NAME" in
      qwen) cmd_download_qwen ;;
      whisper) cmd_download_whisper ;;
      parakeet) cmd_download_parakeet ;;
      *)
        echo "unknown model: $NAME (want qwen, whisper, or parakeet)" >&2
        exit 1 ;;
    esac ;;
  remove)
    [ -n "$NAME" ] || { echo "remove needs a name (try --help)" >&2; exit 1; }
    cmd_remove "$NAME" ;;
esac
