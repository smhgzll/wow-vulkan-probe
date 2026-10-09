#!/usr/bin/env bash
set -euo pipefail

effect_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -eq 0 || ${1:-} == --help ]]; then
    cat <<'HELP'
rainbow-wow — merkezden dışarı heartbeat rainbow AO

Steam Launch Options:
  /tam/yol/rainbow-wow/run.sh %command%

Terminal:
  ./run.sh <oyunu başlatan komut> [argümanlar...]

Ayarlar (komutun önüne eklenir):
  DEPTH_PROBE_HEARTBEAT_BPM=72        Tempo: 20–240 BPM
  DEPTH_PROBE_HEARTBEAT_STRENGTH=1    Animasyon yoğunluğu: 0–1
  DEPTH_PROBE_HEARTBEAT=0            Sabit rainbow; animasyon kapalı
  DEPTH_PROBE_LAYER_LOG=/tmp/rainbow-wow.log

Efekti kaldırmak için Steam launch options'tan bu scripti çıkar.
HELP
    if [[ $# -eq 0 ]]; then exit 2; fi
    exit 0
fi

for asset in layer32/libVkLayer_depth_probe.so layer32/VK_LAYER_depth_probe.json \
             shaders/depth.vert.spv shaders/depth.frag.spv; do
    if [[ ! -r "$effect_dir/$asset" ]]; then
        printf 'rainbow-wow: eksik dosya: %s\nÖnce bu klasörde build.sh çalıştır.\n' "$asset" >&2
        exit 1
    fi
done

# Use the saved layer, even if the main project later changes its shader ABI.
# VK_LAYER_PATH overrides VK_ADD_LAYER_PATH, so handle either existing setup.
if [[ -n ${VK_LAYER_PATH:-} ]]; then
    export VK_LAYER_PATH="$effect_dir/layer32:$VK_LAYER_PATH"
else
    export VK_ADD_LAYER_PATH="$effect_dir/layer32${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
fi
layer_name=VK_LAYER_depth_probe
case ":${VK_INSTANCE_LAYERS:-}:" in
    *":$layer_name:"*) ;;
    *) export VK_INSTANCE_LAYERS="$layer_name${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}" ;;
esac

export DEPTH_PROBE_SHADER_DIR="$effect_dir/shaders"
export DEPTH_PROBE_DEPTH_PRESENT=1
export DEPTH_PROBE_CLEAR_PRESENT=0
export DEPTH_PROBE_HEARTBEAT="${DEPTH_PROBE_HEARTBEAT:-1}"
export DEPTH_PROBE_HEARTBEAT_BPM="${DEPTH_PROBE_HEARTBEAT_BPM:-72}"
export DEPTH_PROBE_HEARTBEAT_STRENGTH="${DEPTH_PROBE_HEARTBEAT_STRENGTH:-1}"
export DEPTH_PROBE_LAYER_LOG="${DEPTH_PROBE_LAYER_LOG:-/tmp/rainbow-wow.log}"

# Preserve Proton/Steam's command and arguments, including paths with spaces.
exec "$@"
