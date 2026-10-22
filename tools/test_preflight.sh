#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build
flags=(-O1 -g -Wall -Wextra -Werror -fshort-wchar -mno-red-zone
       -DGNU_EFI_USE_MS_ABI -I/usr/include/efi -I/usr/include/efi/x86_64)
case ${TEST_PRIVATE_ROM:-0} in
    0) ;;
    1) flags+=(-DTU102_TEST_PRIVATE_ROM) ;;
    *) echo 'TEST_PRIVATE_ROM must be 0 or 1' >&2; exit 2 ;;
esac
gcc "${flags[@]}" tools/test_preflight_startup.c -L/usr/lib -lefi -o build/test_preflight_v4
./build/test_preflight_v4
gcc "${flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    tools/test_preflight_startup.c -L/usr/lib -lefi -o build/test_preflight_v4_sanitized
./build/test_preflight_v4_sanitized
