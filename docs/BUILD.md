# Building the research archive

All commands below are local build or host-simulation steps. They do not
install an EFI application, modify boot selection, flash firmware, or require
root. Dependencies must already be installed. Do not run an upstream automatic
installer as part of preparing these inputs.

## Host tests without firmware

On x86-64 Linux, install GCC, binutils and GNU-EFI development headers/libraries
through your distribution's normal package process. The original system used
GNU-EFI 3.0.15. Paths in these scripts follow Debian/Ubuntu conventions.

```sh
bash tools/test_preflight.sh
```

This runs simulated startup, ESP lookup, logging and capture tests in plain
and AddressSanitizer/UndefinedBehaviorSanitizer builds. It never accesses a GPU.
There are 43 cases per build. With the original private ROM fixture available,
`TEST_PRIVATE_ROM=1 bash tools/test_preflight.sh` adds two fixture CRC checks;
those extra cases fail if the requested file is absent or mismatched.

## EFI v4 and firmware-dependent host tests

Required private/external inputs are pinned in
[results/input-manifest.json](../results/input-manifest.json):

- A manually obtained checkout of `xrip/cmp50hx-unlock` at the exact recorded
  commit, under `third_party/cmp50hx-unlock`. Only its input blob files are used
  by this build; no upstream installer is executed.
- The exact native FWSEC extracted from the test card's own ROM at
  `artifacts/2080ti-fwsec-from-v4/fwsec_50hx_prod.bin`.
- The audited Dell A14 root-bridge PE image at
  `artifacts/dell-a14-audit/PciRootBridge.pe`.

These firmware files are not bundled. Possessing a differently named file or
an apparently similar GPU is not enough; all hashes and device-state guards
must match. The build script refuses missing/mismatched inputs. Do not disable
its checks. Any independent adaptation requires its own platform/firmware audit.

```sh
bash tools/build_experiment_v4.sh
bash tools/test_experiment_v4.sh
```

The second command tests the core and simulated direct-DMA/platform paths in
plain and ASan/UBSan builds. It uses the objects produced by the first command,
but does not execute an EFI application or access physical GPU/high RAM.

The public `esp_guid` is deliberately all zero; the GRUB example likewise has
an unconfigured filesystem UUID. An output EFI is not ready to boot. The ESP
partition number/start/size, GPU/bridge topology and firmware guards are still
specific to the original machine. Configuring an ESP is not sufficient to make
the program safe on another machine. No installation command is provided.

Optional historical ROM-profile tests also require the original private files
`artifacts/2080ti-original-prom.bin` and
`artifacts/preflight-v4-20261021-103713-00-bulk.bin`:

```sh
python3 -m unittest discover -s tools -p test_tu102_port_profile.py
```

## CUDA benchmarks

The original measurements used CUDA 13 and cuBLAS 13. The build scripts require
`CUDA_ROOT` and accept `CUDA_LIB_DIR` if the libraries are not in `lib64` or
`lib` under that root:

```sh
CUDA_ROOT=/path/to/cuda-13 bash tools/build_baseline.sh
CUDA_ROOT=/path/to/cuda-13 bash tools/build_tensor_validate.sh
```

Do not run the resulting programs while mining or other GPU jobs are active.
They intentionally target PCI BDF `02:00.0` and refuse a device that is not
SM75 with 68 SMs. They are compute workloads, not unlock programs. Both produce
JSONL on stdout. Their source is unchanged from the historical measurements;
only the build scripts' machine-local toolkit path was generalized.

## Publication boundary

`artifacts/`, `build/` and `third_party/` are ignored. Do not force-add firmware,
EFI binaries, raw inventory, miner launch scripts, wallets or credentials.
Published measurement JSONL contains benchmark results, not mining account
configuration. Refer to NOTICE.md for the original binary hash and public
profile differences before attempting to reproduce any result.
