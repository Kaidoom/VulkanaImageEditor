#!/usr/bin/env bash
# Build and stage an optional CUDA worker; no sources or models are downloaded.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
engine_dir='' cuda_root='' build_dir='' destination='' jobs=4 tests=OFF
usage() {
    echo 'Usage: build-cuda.sh --engine DIR --cuda DIR --build DIR --destination DIR [--jobs N] [--tests]'
}
while (($#)); do
    case "$1" in
        --engine|--cuda|--build|--destination|--jobs)
            (($# >= 2)) || { usage >&2; exit 2; }
            case "$1" in
                --engine) engine_dir=$2;; --cuda) cuda_root=$2;; --build) build_dir=$2;;
                --destination) destination=$2;; --jobs) jobs=$2;;
            esac
            shift 2;;
        --tests) tests=ON; shift;;
        --help|-h) usage; exit 0;;
        *) usage >&2; exit 2;;
    esac
done
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'Use an x86-64 Linux build environment.' >&2; exit 1; }
[[ -n $engine_dir && -n $cuda_root && -n $build_dir && -n $destination && $jobs =~ ^[1-9][0-9]?$ ]] || { usage >&2; exit 2; }
for program in cmake ninja git python3 strip readelf; do
    command -v "$program" >/dev/null || { echo "Missing build tool: $program" >&2; exit 1; }
done
engine_dir=$(realpath -- "$engine_dir")
cuda_root=$(realpath -- "$cuda_root")
build_dir=$(realpath -m -- "$build_dir")
destination=$(realpath -m -- "$destination")
[[ -x $cuda_root/bin/nvcc ]] || { echo 'CUDA directory must contain bin/nvcc.' >&2; exit 1; }
[[ ! -e $destination && ! -L $destination ]] || { echo 'Destination already exists; choose a new bundle directory.' >&2; exit 1; }
[[ -d $(dirname -- "$destination") ]] || { echo 'Destination parent must already exist.' >&2; exit 1; }
cmake -S "$source_root/src/diffusion/worker" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DVULKANA_DIFFUSION_RELEASE=ON \
    "-DVULKANA_DIFFUSION_ENGINE_DIR=$engine_dir" \
    "-DCUDAToolkit_ROOT=$cuda_root" "-DCMAKE_CUDA_COMPILER=$cuda_root/bin/nvcc" \
    '-DCMAKE_CUDA_ARCHITECTURES=86-real;89-real;120a-real;90-virtual' \
    -DSD_CUDA=ON -DSD_VULKAN=OFF -DGGML_CPU_ALL_VARIANTS=OFF \
    "-DBUILD_TESTING=$tests"
cmake --build "$build_dir" --parallel "$jobs"
worker="$build_dir/vulkana-diffusion-worker"
[[ -x $worker ]] || { echo 'Worker executable was not produced.' >&2; exit 1; }
scratch=$(mktemp -d -- "$(dirname -- "$destination")/.vulkana-diffusion-XXXXXX")
trap '[[ -z ${scratch:-} ]] || rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/bin" "$scratch/lib/vulkana-diffusion/cuda"
cp -- "$worker" "$scratch/bin/vulkana-diffusion-worker-cuda"
strip --strip-unneeded "$scratch/bin/vulkana-diffusion-worker-cuda"
# Copy the SDK's redistributable bytes unchanged. Driver libraries and stubs
# are supplied by the installed NVIDIA driver, never by this bundle.
for library in libcudart.so.13 libcublas.so.13 libcublasLt.so.13; do
    input="$cuda_root/targets/x86_64-linux/lib/$library"
    [[ -f $input ]] || { echo "Missing CUDA redistributable: $library" >&2; exit 1; }
    cp -L -- "$input" "$scratch/lib/vulkana-diffusion/cuda/$library"
done
python3 "$source_root/tools/diffusion/prepare_bundle.py" \
    --engine "$engine_dir" --build "$build_dir" --prefix "$scratch" \
    --cuda-root "$cuda_root" --platform linux --minimum-glibc 2.39
python3 "$source_root/tools/diffusion/prepare_bundle.py" --verify "$scratch"
if [[ $tests == ON ]]; then
    # Test helpers run from the build tree; only this subprocess receives the
    # staged runtime path. Installed workers use their own relative RPATH.
    LD_LIBRARY_PATH="$scratch/lib/vulkana-diffusion/cuda${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ctest --test-dir "$build_dir" --output-on-failure --parallel 1
fi
mv -T --no-clobber -- "$scratch" "$destination"
[[ ! -d $scratch ]] || { echo 'Destination appeared during the build; existing files were preserved.' >&2; exit 1; }
scratch=''
echo "Diffusion worker bundle: $destination"
