#!/usr/bin/env bash
# Research-only comparison build. Never linked, copied into Vulkana or packaged.
set -euo pipefail
spot_repo=$(git -C "$(dirname "${BASH_SOURCE[0]}")/.." rev-parse --show-toplevel)
spot_dir="$spot_repo/research/spot-heal-ipol"
spot_archive="$spot_dir/Inpainting_ipol_code.tar.gz"
spot_sha=66ca5dba4296f554d2698f23d3ae8bbce3ad60020e796d741047be87c7ac3d50
git -C "$spot_repo" check-ignore -q research/spot-heal-ipol/probe || {
    printf '%s\n' 'Research directory must be ignored by git before downloading.' >&2; exit 1;
}
for spot_tool in curl sha256sum tar make g++ pkg-config; do
    command -v "$spot_tool" >/dev/null || { printf 'Missing research build dependency: %s\n' "$spot_tool" >&2; exit 1; }
done
pkg-config --exists libpng libtiff-4 || {
    printf '%s\n' 'Missing research-only libpng/libtiff development files; no installation was attempted.' >&2; exit 1;
}
mkdir -p "$spot_dir"
if [[ ! -f "$spot_archive" ]]; then
    curl --fail --location --max-time 60 \
        https://www.ipol.im/pub/art/2017/189/Inpainting_ipol_code.tar.gz \
        --output "$spot_archive"
fi
printf '%s  %s\n' "$spot_sha" "$spot_archive" | sha256sum --check --status || {
    printf '%s\n' 'Research archive checksum mismatch; refusing extraction/build.' >&2; exit 1;
}
if [[ ! -d "$spot_dir/Inpainting_ipol_code" ]]; then
    tar --no-same-owner --no-same-permissions -xzf "$spot_archive" -C "$spot_dir"
fi
# The unmodified upstream makefile uses C++11, libpng and libtiff. OMP is not
# enabled, keeping the reference run single-threaded and reproducible.
make -C "$spot_dir/Inpainting_ipol_code" -j4
printf '\nResearch-only reference: %s\n' "$spot_dir/Inpainting_ipol_code/bin/inpaint_image"
printf '%s\n' 'License metadata conflict: read docs/SPOT_HEAL_BENCHMARKS.md; do not incorporate this source.'
