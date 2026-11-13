# Supplementary inference dependency notices

The pinned engine vendors these libraries but references their complete license texts externally. These copies accompany the worker inventory; other texts are copied from the pinned engine or selected CUDA SDK during bundle preparation.

- `nlohmann-json-MIT.txt`: [nlohmann/json v3.11.2](https://github.com/nlohmann/json/blob/v3.11.2/LICENSE.MIT), matching the vendored header version. Embedded contributor attributions are also collected from that header.
- `zip-MIT.txt`: [kuba--/zip license](https://github.com/kuba--/zip/blob/master/LICENSE.txt), referenced by the engine's `thirdparty/README.md`. Miniz's incorporated notices are collected separately from its vendored header.
- `CCCL-LICENSE.txt`: [NVIDIA CCCL v3.3.4](https://github.com/NVIDIA/cccl/blob/v3.3.4/LICENSE), identical to v3.3.1. This preserves Thrust, CUB and libcu++ terms, including the LLVM exception, when the selected CUDA 13.3 Linux SDK omits the standalone notice. An SDK-provided copy takes precedence.

Review these records when updating the engine baseline.
