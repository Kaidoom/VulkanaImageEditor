#!/usr/bin/env bash
# Fresh, never-tuned quality split. Clean images are scoring-only fixtures.
set -euo pipefail
if [[ $# != 2 ]]; then
    printf 'Usage: %s BENCHMARK_EXECUTABLE OUTPUT_DIRECTORY\n' "$0" >&2; exit 2
fi
spot_binary=$(realpath "$1")
spot_output=$(realpath -m "$2")
spot_repo=$(git -C "$(dirname "${BASH_SOURCE[0]}")/.." rev-parse --show-toplevel)
mkdir -p "$spot_output"
QT_QPA_PLATFORM=offscreen "$spot_binary" --fresh-heldout --out "$spot_output/synthetic" >"$spot_output/synthetic.log" 2>&1
for spot_source in raw retouch; do
    spot_args=()
    if [[ "$spot_source" = retouch ]]; then spot_args+=(--retouch); fi
    for spot_case in skin cloth stone; do
        spot_image="$spot_repo/research/spot-heal-photographs/KSC-99pp0310.jpg"
        case "$spot_case" in
            skin) spot_x=1165; spot_y=735; spot_length=0 ;;
            cloth) spot_x=570; spot_y=2200; spot_length=90 ;;
            stone) spot_x=500; spot_y=350; spot_length=80
                spot_image="$spot_repo/research/spot-heal-photographs/PIA20323.jpg" ;;
        esac
        spot_directory="$spot_output/photos/$spot_case-$spot_source"
        mkdir -p "$spot_directory"
        QT_QPA_PLATFORM=offscreen "$spot_binary" --perf-child --photo "$spot_image" \
            --x "$spot_x" --y "$spot_y" --length "$spot_length" --brush-size 32 \
            --dump "$spot_directory" "${spot_args[@]}" >"$spot_directory/run.log" 2>&1
        printf '%s-%s: ' "$spot_case" "$spot_source"
        rg '^photographic_unknown_linear_mae=' "$spot_directory/run.log"
    done
done
