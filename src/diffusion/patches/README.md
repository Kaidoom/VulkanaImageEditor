# Diffusion engine patches

`../engine-lock.json` records the upstream engine, dependency revisions and downstream patch checksums. Apply the listed patches, in order, to a clean checkout of that baseline before configuring the worker:

```sh
git -C /path/to/stable-diffusion.cpp apply /path/to/VulkanaImageEditor/src/diffusion/patches/0001-int8-large-convrot.patch
git -C /path/to/stable-diffusion.cpp apply /path/to/VulkanaImageEditor/src/diffusion/patches/0002-flux3-fused-qkv.patch
```

The worker build verifies the revisions, patch checksums and that the patches are applied. It does not download or modify engine sources. When upgrading the engine, review and rebase these patches, update the lock and rerun the worker protocol, numerical kernel and real-inference checks.

## Large INT8 ConvRot groups

`0001-int8-large-convrot.patch` adds groups 1024 and 4096 to the CPU and CUDA INT8 activation rotation paths. Some quantized text encoders use these larger groups. The CUDA path processes bounded groups, computes one scale per input row and preserves the packed INT8 format consumed by the existing matrix multiplication. CPU rotation scratch is bounded at 16 KiB per worker thread. Existing small-group paths remain supported; packed INT4 format validation retains its existing group limit.

Unsupported group sizes and widths incompatible with the selected group are rejected during Safetensors loading, with the filename and tensor in the error. Vulkana preserves this diagnostic instead of replacing it with a generic model-load failure.

`tests/diffusion/ConvRotTests.cpp` compares packed quantization, row padding, scales and matrix multiplication against an independent dense rotation reference. CPU tests cover groups 1–4096; CUDA tests cover 1, 64, 256, 1024 and 4096. Other GPU backends have not been validated for the larger groups.

## FLUX.3 fused attention projections

`0002-flux3-fused-qkv.patch` recognizes FLUX.3 checkpoints that combine Q, K and V projections into a single `qkv` weight. It uses one native linear operation and strided views to separate its output into attention heads, preserving the checkpoint's quantization and scales. Both base and distilled checkpoints use the detected layout; existing separate-projection checkpoints retain their original path. No weight files are rewritten.

Unrecognized architectures report the selected diffusion/checkpoint filename instead of an empty path. The worker preserves that diagnostic as a recoverable `unsupported_model_format` error. `tests/diffusion/Flux3LayoutTests.cpp` covers detection, flow-map independence and channel/token/batch layout; worker protocol tests cover both checkpoint and component-file error reporting.
