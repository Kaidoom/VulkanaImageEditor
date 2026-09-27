#!/usr/bin/env bash
# Research fixtures only: no network access or photograph dependency at runtime.
set -euo pipefail
spot_repo=$(git -C "$(dirname "${BASH_SOURCE[0]}")/.." rev-parse --show-toplevel)
spot_dir="$spot_repo/research/spot-heal-photographs"
git -C "$spot_repo" check-ignore -q research/spot-heal-photographs/probe || {
    printf '%s\n' 'Photographic research directory must be ignored by git.' >&2; exit 1;
}
mkdir -p "$spot_dir"
spot_fetch() {
    local name=$1 digest=$2 url=$3
    if [[ ! -f "$spot_dir/$name" ]]; then
        curl --fail --location --max-time 60 "$url" --output "$spot_dir/$name"
    fi
    printf '%s  %s\n' "$digest" "$spot_dir/$name" | sha256sum --check --status || {
        printf 'Photograph checksum mismatch: %s\n' "$name" >&2; exit 1;
    }
}
spot_fetch KSC-99pp0310.jpg 15cf6d87ada0dfb401835eb8a58cfb9cedcd3c80da46252ad437686bd99b4a4c \
    'https://images-assets.nasa.gov/image/KSC-99pp0310/KSC-99pp0310~orig.jpg'
spot_fetch PIA20323.jpg 1a8caf35da248c1dfa2135706ec94f4d9db2d03f0ac77571bd98b3ed8f33f853 \
    'https://www.nasa.gov/wp-content/uploads/2023/03/pia20323_1277mh0001970010404560c00_dxxx.jpg'
printf 'Verified benchmark photographs: %s\n' "$spot_dir"
printf '%s\n' 'Credit NASA and NASA/JPL-Caltech/MSSS; informational comparison only, no endorsement. See docs/SPOT_HEAL_PHOTOGRAPHS.md.'
