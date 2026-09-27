#!/usr/bin/env bash
# Exact engineering comparison, never a golden-regeneration command.
set -euo pipefail
if [[ $# != 2 ]]; then
    printf 'Usage: %s FROZEN_OUTPUT_DIRECTORY CURRENT_OUTPUT_DIRECTORY\n' "$0" >&2; exit 2
fi
spot_before=$(realpath "$1")
spot_after=$(realpath "$2")
spot_count=0
while IFS= read -r spot_log; do
    spot_case=${spot_log#"$spot_before/"}
    spot_case=${spot_case%/run.log}
    spot_current="$spot_after/$spot_case/run.log"
    if [[ ! -f "$spot_current" ]]; then
        printf 'Missing current case: %s\n' "$spot_case" >&2; exit 1
    fi
    # This line contains the complete linear-float candidate, final stored
    # RGBA8 pixels, exact hard-mark mask and its document-space bounds.
    spot_original=$(rg '^candidate_float_sha256=' "$spot_log")
    spot_result=$(rg '^candidate_float_sha256=' "$spot_current")
    if [[ "$spot_original" != "$spot_result" ]]; then
        printf 'Non-identical repair: %s\n' "$spot_case" >&2
        printf 'Frozen: %s\nCurrent: %s\n' "$spot_original" "$spot_result" >&2
        exit 1
    fi
    spot_count=$((spot_count+1))
    printf 'Exact candidate / stored pixels / mask: %s\n' "$spot_case"
done < <(rg --files "$spot_before" | rg '/run\.log$' | sort)
if [[ $spot_count = 0 ]]; then printf '%s\n' 'No reference cases found.' >&2; exit 1; fi
printf 'Exact comparison passed: %s cases\n' "$spot_count"
