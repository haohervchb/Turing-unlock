#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
flags=(-O1 -g -Wall -Wextra -Werror -DTX_EXPERIMENT_V4)
efi_flags=(-fshort-wchar -mno-red-zone -DGNU_EFI_USE_MS_ABI -I/usr/include/efi -I/usr/include/efi/x86_64)
blobs=(build/experiment-v4/tx_native.o build/experiment-v4/tx_generic.o build/experiment-v4/tx_booter.o
       build/experiment-v4/tx_signature.o build/experiment-v4/tx_boot.o)
for variant in plain sanitized; do
    sanitizer=()
    if [[ $variant == sanitized ]]; then sanitizer=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
    gcc "${flags[@]}" "${sanitizer[@]}" tools/test_tu102_experiment.c -o "build/experiment-v4/core-$variant"
    "build/experiment-v4/core-$variant"
    gcc "${flags[@]}" "${efi_flags[@]}" "${sanitizer[@]}" tools/test_direct_dma.c "${blobs[@]}" \
        -Wl,-z,noexecstack -L/usr/lib -lefi -o "build/experiment-v4/direct-$variant"
    "build/experiment-v4/direct-$variant"
done
