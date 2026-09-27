#!/usr/bin/env bash
# Identical workloads for a frozen baseline or current optimized executable.
# Keep profile/allocation/validation runs separate from final wall-time runs.
set -euo pipefail
if [[ $# -lt 2 ]]; then
    printf 'Usage: %s BENCHMARK_EXECUTABLE OUTPUT_DIRECTORY [benchmark options...]\n' "$0" >&2; exit 2
fi
spot_binary=$(realpath "$1")
spot_output=$(realpath -m "$2")
shift 2
spot_repo=$(git -C "$(dirname "${BASH_SOURCE[0]}")/.." rev-parse --show-toplevel)
mkdir -p "$spot_output"
spot_run() {
    local name=$1
    shift
    if [[ -n "${SPOT_HEAL_CASE:-}" && "$name" != "$SPOT_HEAL_CASE" ]]; then return; fi
    mkdir -p "$spot_output/$name"
    QT_QPA_PLATFORM=offscreen /usr/bin/time -v "$spot_binary" --perf-child \
        --dump "$spot_output/$name" "$@" \
        >"$spot_output/$name/run.log" 2>"$spot_output/$name/process.log"
    printf '%s: ' "$name"
    head -n 1 "$spot_output/$name/run.log"
}
for spot_source in raw retouch; do
    spot_args=()
    if [[ "$spot_source" = retouch ]]; then spot_args+=(--retouch); fi
    for spot_size in 14 32 64 100; do
        spot_run "spot-$spot_size-$spot_source" --brush-size "$spot_size" "${spot_args[@]}" "$@"
    done
    spot_run "short-$spot_source" --brush-size 32 --length 150 "${spot_args[@]}" "$@"
    spot_run "long-$spot_source" --brush-size 20 --length 1000 "${spot_args[@]}" "$@"
    spot_run "photo-fabric-$spot_source" --photo "$spot_repo/research/spot-heal-photographs/KSC-99pp0310.jpg" \
        --x 410 --y 1620 --brush-size 64 --length 120 "${spot_args[@]}" "$@"
    spot_run "4k-small-$spot_source" --width 3840 --height 2160 "${spot_args[@]}" "$@"
    spot_run "4k-long-$spot_source" --width 3840 --height 2160 --long "${spot_args[@]}" "$@"
done
