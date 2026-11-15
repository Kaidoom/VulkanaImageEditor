#!/usr/bin/env bash
# Build and stage a bundled Vulkan worker; no sources or models are downloaded.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
engine_dir='' vulkan_headers='/usr/include' spirv_headers='/usr/include' spirv_license='' shader_compiler=''  build_dir='' destination='' jobs=4 tests=OFF
usage() {
    echo 'Usage: build-vulkan.sh --engine DIR --spirv-license FILE [--vulkan-headers DIR] [--spirv-headers DIR] [--glslc FILE] --build DIR --destination DIR [--jobs N] [--tests]'
}
while (($#)); do
    case "$1" in
        --engine|--vulkan-headers|--spirv-headers|--spirv-license|--glslc|--build|--destination|--jobs)
            (($# >= 2)) || { usage >&2; exit 2; }
            case "$1" in
                --engine) engine_dir=$2;; --vulkan-headers) vulkan_headers=$2;; --spirv-headers) spirv_headers=$2;; --spirv-license) spirv_license=$2;; --glslc) shader_compiler=$2;; --build) build_dir=$2;;
                --destination) destination=$2;; --jobs) jobs=$2;;
            esac
            shift 2;;
        --tests) tests=ON; shift;;
        --help|-h) usage; exit 0;;
        *) usage >&2; exit 2;;
    esac
done
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'Use an x86-64 Linux build environment.' >&2; exit 1; }
[[ -n $engine_dir && -n $spirv_license && -n $build_dir && -n $destination && $jobs =~ ^[1-9][0-9]?$ ]] || { usage >&2; exit 2; }
for program in cmake ninja git python3 strip readelf; do
    command -v "$program" >/dev/null || { echo "Missing build tool: $program" >&2; exit 1; }
done
engine_dir=$(realpath -- "$engine_dir")
vulkan_headers=$(realpath -- "$vulkan_headers")
spirv_headers=$(realpath -- "$spirv_headers")
spirv_license=$(realpath -- "$spirv_license")
if [[ -z $shader_compiler && -x $vulkan_headers/../bin/glslc ]]; then
    shader_compiler=$vulkan_headers/../bin/glslc
fi
shader_compiler=$(realpath -- "${shader_compiler:-$(command -v glslc)}")
build_dir=$(realpath -m -- "$build_dir")
destination=$(realpath -m -- "$destination")
[[ -f $spirv_license ]] || { echo 'SPIR-V license file is missing.' >&2; exit 1; }
[[ ! -e $destination && ! -L $destination ]] || { echo 'Destination already exists; choose a new bundle directory.' >&2; exit 1; }
[[ -d $(dirname -- "$destination") ]] || { echo 'Destination parent must already exist.' >&2; exit 1; }
cmake -S "$source_root/src/diffusion/worker" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DVULKANA_DIFFUSION_RELEASE=ON \
    "-DVULKANA_DIFFUSION_ENGINE_DIR=$engine_dir" \
    "-DVulkan_GLSLC_EXECUTABLE=$shader_compiler" "-DVulkan_INCLUDE_DIR=$vulkan_headers" "-DVULKANA_SPIRV_INCLUDE_DIR=$spirv_headers" \
    -DSD_CUDA=OFF -DSD_VULKAN=ON -DGGML_CPU_ALL_VARIANTS=OFF \
    "-DBUILD_TESTING=$tests"
cmake --build "$build_dir" --parallel "$jobs"
worker="$build_dir/vulkana-diffusion-worker"
[[ -x $worker ]] || { echo 'Worker executable was not produced.' >&2; exit 1; }
scratch=$(mktemp -d -- "$(dirname -- "$destination")/.vulkana-diffusion-XXXXXX")
trap '[[ -z ${scratch:-} ]] || rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/bin" "$scratch/lib/vulkana-diffusion/vulkan"
cp -- "$worker" "$scratch/bin/vulkana-diffusion-worker-vulkan"
strip --strip-unneeded "$scratch/bin/vulkana-diffusion-worker-vulkan"
python3 "$source_root/tools/diffusion/prepare_bundle.py" \
    --engine "$engine_dir" --build "$build_dir" --prefix "$scratch" \
    --backend vulkan --shader-compiler "$shader_compiler" --vulkan-headers "$vulkan_headers" --spirv-license "$spirv_license" \
    --platform linux --minimum-glibc 2.39
python3 "$source_root/tools/diffusion/prepare_bundle.py" --verify "$scratch"
if [[ $tests == ON ]]; then
    ctest --test-dir "$build_dir" --output-on-failure --parallel 1
fi
mv -T --no-clobber -- "$scratch" "$destination"
[[ ! -d $scratch ]] || { echo 'Destination appeared during the build; existing files were preserved.' >&2; exit 1; }
scratch=''
echo "Diffusion worker bundle: $destination"
