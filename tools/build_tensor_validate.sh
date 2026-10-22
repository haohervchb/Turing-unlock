#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
cuda_root=${CUDA_ROOT:?Set CUDA_ROOT to a CUDA 13 toolkit directory}
cuda_lib=${CUDA_LIB_DIR:-$cuda_root/lib64}
[[ -d $cuda_lib ]] || cuda_lib="$cuda_root/lib"
mkdir -p build
"$cuda_root/bin/nvcc" -std=c++17 -O3 -arch=sm_75 --cudart shared \
  -Xcompiler=-Wall,-Wextra,-Werror -I"$cuda_root/include" -L"$cuda_lib" \
  -Xlinker -rpath,"$cuda_lib" tools/tensor_validate.cu \
  -l:libcublas.so.13 -o build/tensor_validate
