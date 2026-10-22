# Provenance and licensing

This research adaptation is distributed under copyleft-next 0.3.1; see
[LICENSE](LICENSE). Applicable upstream notices are retained below. Firmware
inputs and dependencies retain their own licenses; this repository does not
relicense or distribute NVIDIA firmware, graphics-card ROMs, Dell BIOS images,
or prebuilt EFI applications.

## Unlock lineage

The pre-OS loader sequence and pinned Booter/V67 input pair were studied from
[xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock) at commit
`df2fe11b370bb52cc314576d0f2f5ae47ce908f8`. Its repository root carries
copyleft-next 0.3.1. Its EFI loader separately carries the CMP40HX MIT notice;
do not describe every file or firmware blob in that repository as MIT.

The upstream EFI loader's attribution is retained here:

> Ported from unlock40x_v70.c of the CMP40HX-Unlock project
> (https://github.com/PZH1gdmu/CMP40HX-Unlock, MIT License, Copyright (c) 2026),
> which itself descends from the CMP 90HX (GA102) unlock work and the V67
> canary exploit published by Jon Pry, "A Canary in the Crypto Mine",
> DOI 10.5281/zenodo.20916112. Their MIT license and this notice carry over.

The complete upstream MIT notice is in
[LICENSES/CMP40HX-MIT.txt](LICENSES/CMP40HX-MIT.txt).

Ordinary TU102 Falcon loading, WPR/FRTS layout and firmware interfaces were
cross-checked against
[NVIDIA/open-gpu-kernel-modules, tag 580.178.04](https://github.com/NVIDIA/open-gpu-kernel-modules/tree/580.178.04).
NVIDIA source licensing varies by component; this archive does not vendor its
driver. GNU-EFI, GRUB, CUDA and cuBLAS are external build/runtime dependencies.

Related research includes [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)
at `bb1088b6b5b8cbdafa1ea69601bd118f7f317158` and
[unitypit71/rtx4070tisuper-tensor-unlock](https://github.com/unitypit71/rtx4070tisuper-tensor-unlock).
Their results do not establish compatibility of this board-specific loader
with other GPU generations.

## Public-source changes

The loader/core/DMA logic is the tested v4 implementation, with its original
board-specific guards retained. For publication, the real ESP GUID was replaced
by an unconfigured all-zero initializer, its log text was generalized, and the
GRUB template's filesystem UUID was replaced by a placeholder. CUDA build
scripts now use environment-supplied toolkit paths. Two private-ROM CRC test
cases are opt-in via TEST_PRIVATE_ROM=1; all other startup simulations run
without firmware. Comments and documentation
were updated to distinguish measured results from pending work.

Consequently, an EFI built from this public checkout is **not byte-identical**
to the successfully tested private-profile EFI and is deliberately unconfigured
for real boot use. The original EFI's hash in the results identifies the
historical test binary, not a downloadable public binary.
