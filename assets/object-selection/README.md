# Bundled Object Selection inputs

`mobilesam-v1/` contains the quality-tested MobileSAM ONNX encoder/decoder and
ONNX Runtime CPU 1.30.0 for Linux x86-64. These files are ordinary Git-visible
files, not LFS pointers, first-use downloads or dependencies on the research tree.
Their combined size is approximately 71 MiB, including licenses.

The exact payload hashes are pinned in `cmake/ObjectSelectionBundle.cmake` and
the runtime manifest. CMake checks the repository bytes; packaging must preserve
them without stripping, rewriting RPATHs, optimizing or re-exporting the graphs.
Model/runtime upgrades require new quality testing and an explicit hash update.

Source inputs:

- [MobileSAM revision f706ad9](https://github.com/ChaoningZhang/MobileSAM/tree/f706ad9c4eb7f219c00d9050e46328518ffb65d2), `weights/mobile_sam.pt`, SHA-256 `6dbb90523a35330fedd7f1d3dfc66f995213d81b29a5ca8108dbcdd4e37d6c2f`.
- [ONNX Runtime 1.30.0](https://github.com/microsoft/onnxruntime/tree/v1.30.0), `libonnxruntime.so.1.30.0` from the official `onnxruntime-1.30.0-cp314-cp314-manylinux_2_28_x86_64` wheel. Only the CPU shared library is included, renamed `libonnxruntime.so`; no Python extension or Python dependency is needed.
- C API declarations: ONNX Runtime 1.20.1, retained in `src/ui/third_party/onnxruntime/`.

The model exporter is `tools/prepare-object-selection-pack.py`; it is a
development-only script, never invoked by the build or installed application.
PyTorch 2.14.1+cpu, torchvision 0.29.1+cpu, timm 1.0.30 and ONNX 1.23.2 were used
for the tested opset-17 export. Re-exporting reproduced both ONNX files exactly.

The ONNX library is already stripped. Installation copies it with data-file
permissions, so RPM's executable-only comment stripping must not rewrite it.
Portable packages retain the existing C++ runtime closure and load this library
after the application's C++ runtime. Its glibc requirement is 2.28, below the
application's existing portable baseline. CPU-dispatched kernels are retained;
there is no CUDA or Vulkan inference dependency.

`MobileSAM-LICENSE.txt`, `ONNXRuntime-LICENSE.txt`,
`ONNXRuntime-ThirdPartyNotices.txt` and `ObjectSelection-NOTICE.txt` accompany the
payload. The same material appears in Vulkana's offline and website notices.
Private test images, comparison files and the original development environment
are not part of this directory or any install rule.
