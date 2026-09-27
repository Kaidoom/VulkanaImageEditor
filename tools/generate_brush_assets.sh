#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
source_dir="$project_dir/assets/brush"
output_dir="$source_dir/packaged/v1"

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick 7 is required to generate brush assets." >&2
    exit 1
fi

mkdir -p "$output_dir/tips" "$output_dir/grains"

check_source() {
    local expected="$1"
    local path="$2"
    local actual
    actual="$(sha256sum "$path" | cut -d' ' -f1)"
    if [[ "$actual" != "$expected" ]]; then
        echo "Unexpected source digest for $path" >&2
        echo "expected $expected" >&2
        echo "actual   $actual" >&2
        exit 1
    fi
}

make_tip() {
    local expected_source="$1"
    local source_name="$2"
    local output_name="$3"
    check_source "$expected_source" "$source_dir/Tips/$source_name"
    magick "$source_dir/Tips/$source_name" \
        -depth 8 -strip \
        "PNG32:$output_dir/tips/$output_name"
}

make_grain() {
    local expected_source="$1"
    local source_name="$2"
    local output_name="$3"
    check_source "$expected_source" "$source_dir/grains/$source_name"
    magick "$source_dir/grains/$source_name" \
        -colorspace Gray -depth 8 -filter Lanczos -resize 512x512\! -strip \
        "$output_dir/grains/$output_name"
}

# Preserve preset identities/order while replacing their coverage with the
# owner's cleaned transparent revision. RGB contains real bristle tones;
# the registry uses luminance-times-alpha (not alpha-only).
make_tip 6351cdbb680721c18a20d034370eca14e09e58fd5647ce700cd76922575e81ad Tip2.png t_ast_07.png
make_tip c6f29636c4f296dbadc04f48327dd23da33588a26015a07082e9fcf0174b4aad Tip1.png t_ast_103.png
make_tip 57a6ba3e39e97354b92a71442350732d6d51639db048627d5dc60a986023c249 Tip4.png t_ast_119.png
make_tip 46d5a6044fe8b6252f87f4b500cff649c45180ae23df1f5b4b23d12e4876777d Tip0.png t_ast_49.png
make_tip fb7fbb7ef2e2db92e2457e5f59075c5b40e1b13a817ec06ce0a34504b13848ca Tip3.png t_ast_78.png

make_grain 9562c779d1d1248f8d061517b8c729bff8aaa112475ceeaca50ac15c38ee1306 T_Cloudy_Noise.PNG cloudy_noise.png
make_grain d3400bf3182b3a06ac9173d1d02f49490db86ec5a74be4db3eed4f00dec922e2 T_Noise_125.PNG noise_125.png
make_grain c4116e37baa538a6ea3f32a1d048fa89fef1534945b3c75ae555a7f02662e700 T_Noise_148.PNG noise_148.png
make_grain 4f694845a240d6cafc3459ce80e719b1644e7bb54fec68966a294e7b60f3505c T_Noise_151.PNG noise_151.png
make_grain d6ec1bbb6eda8d336ede6bdbf6b3f188be5b2c1544249c064f595ccf66a05466 T_Noise_178.PNG noise_178.png
make_grain 57e4534435e9c9936084716e57a89a308abd7bc25d3fce300717d17558d03a6c T_Noise_231.PNG noise_231.png
make_grain add92ac4ae057da599ed2e1288151430cf23bd6253b1d849314e10db18a12a27 T_Noise_370.PNG noise_370.png
make_grain 99f5900af829ca72f410e9e949abc0043ba84b0a3d79448ee3a0834f36791585 T_Perlin_Noise_M.PNG perlin_noise_m.png
make_grain 77c942f8ccdca4aabbc4c74024a6740756f5c14052ed913845dc60f542c169fa T_blend_noise_a.PNG multiscale-mottle.png

echo "Brush derivatives regenerated under $output_dir"
echo "Run the brush asset tests to verify registry digests and goldens."
