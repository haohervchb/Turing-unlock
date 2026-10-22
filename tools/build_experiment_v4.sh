#!/usr/bin/env bash
# Build only: no installation, boot selection, sudo or GPU access.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
out=build/experiment-v4
mkdir -p "$out"
check() { [[ $(sha256sum "$1" | cut -d' ' -f1) == "$2" ]]; }
blobdir=third_party/cmp50hx-unlock/efi-unlock/blobs
check artifacts/2080ti-fwsec-from-v4/fwsec_50hx_prod.bin 584b5fe0826c40aaa8731d70f4fe5fb505380075dca77f7a94d31cfa09d20f6e
check "$blobdir/bl_gsp_tu102.bin" f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9
check "$blobdir/booter_ucode_prod.bin" e0f0fc93da097b5b68e5af3adb58b2235ed1fad5fce0b23592d3895af44cb743
check "$blobdir/v67_payload.bin" de7288bcfb80c2d4b259a59e823c926ba0a9adcab594219cc6802c94257ce271
check "$blobdir/gsp_rm_boot_dbg.bin" fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471
check artifacts/dell-a14-audit/PciRootBridge.pe eac8ed469fcb4056ec4197bf5506dba2bc163b83866c314bf6570726cf1408ca
embed() {
    local path="$1" symbol="$2" name="${1##*/}" directory="${1%/*}"
    local obj="$PWD/$out/$symbol.o" base="${name//./_}"
    (cd "$directory" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        --redefine-sym "_binary_${base}_start=$symbol" \
        --redefine-sym "_binary_${base}_end=${symbol}_end" \
        --redefine-sym "_binary_${base}_size=${symbol}_size" \
        --rename-section .data=.rodata,alloc,load,readonly,data,contents "$name" "$obj")
}
embed artifacts/2080ti-fwsec-from-v4/fwsec_50hx_prod.bin tx_native
embed "$blobdir/bl_gsp_tu102.bin" tx_generic
embed "$blobdir/booter_ucode_prod.bin" tx_booter
embed "$blobdir/v67_payload.bin" tx_signature
embed "$blobdir/gsp_rm_boot_dbg.bin" tx_boot
gcc -c -O2 -Wall -Wextra -Werror -fpic -ffreestanding \
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -fno-builtin \
    -DTX_EXPERIMENT_V4 -DGNU_EFI_USE_MS_ABI -I/usr/include/efi -I/usr/include/efi/x86_64 \
    tools/tu102_experiment.c -o "$out/main.o"
ld -nostdlib -znocombreloc -z noexecstack -T /usr/lib/elf_x86_64_efi.lds -shared -Bsymbolic \
    /usr/lib/crt0-efi-x86_64.o "$out/main.o" \
    "$out/tx_native.o" "$out/tx_generic.o" "$out/tx_booter.o" \
    "$out/tx_signature.o" "$out/tx_boot.o" \
    -L/usr/lib -lgnuefi -lefi -o "$out/experiment.so"
[[ -z $(nm -u "$out/experiment.so") ]]
objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
    -j .reloc -j .rel -j .rela -j '.rel.*' -j '.rela.*' \
    --target=efi-app-x86_64 "$out/experiment.so" "$out/TU102EX4.EFI"
sha256sum "$out/TU102EX4.EFI"
