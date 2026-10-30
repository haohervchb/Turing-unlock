# Turing-unlock

Board-specific research into a **volatile, pre-OS tensor-compute unlock on an
ASUS RTX 2080 Ti (TU102, SM75)**. One physical v4 run succeeded on 2026-10-22.
This repository publishes the loader source, host-side tests, benchmark code
and measured results. It is **not a general-purpose installer or VBIOS mod**.

## What was measured

| Workload | Locked | Unlocked | Change |
| --- | ---: | ---: | ---: |
| FP16-input / FP32-accumulation MMA, FLOP/SM/clock | 511.6652 | 1022.4380 | 1.9983× |
| Same MMA, TFLOPS | 46.9361 | 93.6649 | 1.9956× |
| cuBLAS 8192 GEMM, TFLOPS | 48.1488 | 85.1705 | 1.7689× |
| FP16-input / FP16-accumulation MMA, FLOP/SM/clock | 1022.1337 | 1022.1171 | Unchanged |

The NVIDIA RM GET controls changed FMLA32 and IMLA4 from half to full rate.
**IMLA4 has not received a dedicated integer-throughput A/B test.** No extra
SMs, CUDA cores, Tensor Cores or VRAM were enabled. Do not interpret these
results as a 2× improvement in all compute, games, AI workloads or mining.

An additional nonzero cuBLAS validation checked **295,035 outputs**, all exactly
matching an independent CPU reference. The 8192 case checked 128 sampled
outputs, not the entire matrix. These are bounded tests, not an endurance or
exhaustive numerical-stability certification.

Pearl mining with the existing PeakMiner 2.19.1 showed **no meaningful gain**:
85.1 TH/s after warm-up at a 190 W limit versus the user's approximate 86 TH/s
historical reference. See [validation notes](docs/VALIDATION.md), the
[comparison JSON](results/2080ti-v4-comparison.json), and the raw benchmark
JSONL files under [results](results/).

## Scope and current status

- Tested unlock hardware: ASUS RTX 2080 Ti Rev. A, `10de:1e07`, subsystem
  `1043:866a`, VBIOS `90.02.17.00.47`, 11 GiB, 68 SMs.
- Tested motherboard: Alienware Area-51 R2, BIOS A14, Xeon E5-1660 v3.
- Tested Linux driver: unmodified NVIDIA open driver `580.178.04`.
- The successful run used Above 4G Decoding **off**, CSM on, and a tightly
  guarded direct-physical high-RAM DMA path. This is not evidence that the
  same path is safe on other firmware or IOMMU configurations.
- **No GPU VBIOS or motherboard BIOS was flashed by this implementation.**
- The unlock is volatile. A subsequent normal restart restored half-rate
  FMLA32/IMLA4. Automatic reapplication is **not implemented or enabled**.
- An RTX 5060 Ti was reinstalled and both cards resumed normal mining.
  **The unlock has not yet been validated with both cards installed.**
  This TU102 firmware/payload must not be applied to the SM120 card.

## Important boot limitations

The source is deliberately tied to the observed PCI topology and firmware
state. It checks the device at `0000:02:00.0`, the exact PCI/subsystem IDs,
BAR0, PCI command, parent bridge, firmware descriptors/bytes, engine state,
and a finite set of observed selector tuples before executing a payload.
It does not blindly choose NVIDIA GPU index 0.

Unknown pre-write states refuse the attempt. A failed, possibly partial GPU
mutation holds the machine in UEFI with DMA backing retained: it must **not**
continue into the OS; a full power cycle is required. These safeguards reduce
specific risks but cannot guarantee safety or recovery on untested hardware.

The public source has an **unconfigured ESP GUID**, and the GRUB example has
a placeholder filesystem UUID. No prebuilt EFI or automatic installation
script is published. Do not remove identity/DMA guards just to make a build
run. Do not run a GPU reset/unbind against the active Linux display card.

The existing one-shot GRUB entry is **not suitable as a permanent default**:
it returns through `grub.cfg`, which can select it again. See
[persistence and dual-GPU work](docs/BOOT-STATUS.md) for the outstanding design
and checks. Nothing in this repository installs a boot hook automatically.

## Repository layout

- `tools/tu102_experiment.c`, `tu102_experiment_core.h`: v4 EFI wrapper and
  injected-I/O loader core.
- `tools/tu102_{preflight,prom_capture,dma_services,high_dma,direct_dma}.*`:
  logging, firmware identity and board-specific DMA guards.
- `tools/test_*.c`: simulated protocol/register failure-path tests; no real
  GPU access when running the host tests.
- `tools/tensor_baseline.cu`, `tensor_validate.cu`: SM75 throughput and
  independent CPU-reference numerical checks.
- `tools/tensor_validate_windows.py`, `tensor_issue_rate_windows.py`: opt-in
  Windows/PyTorch NVRTC adaptation; see [Windows benchmark notes](docs/WINDOWS-BENCHMARK.md)
  for dependencies, identity checks and the hardware-validation boundary.
- `tools/tu102_port_profile.py`, `analyze_prom_capture.py`: offline ROM/profile
  analysis. These do not flash a card or execute firmware.
- `docs/BUILD.md`: dependencies, private inputs, and build/test boundaries.
- `results/`: measured outputs and explicit limitations.

## Build and test

Read [BUILD.md](docs/BUILD.md) first. The EFI/core tests require exact external
firmware inputs that are intentionally **not redistributed**. Firmware-free
startup/logging simulations can be run on x86-64 Linux with GCC and GNU-EFI:

```sh
bash tools/test_preflight.sh
```

CUDA 13 benchmark builds use an explicit toolkit directory:

```sh
CUDA_ROOT=/path/to/cuda-13 bash tools/build_baseline.sh
CUDA_ROOT=/path/to/cuda-13 bash tools/build_tensor_validate.sh
```

Building is not running. Do not run these GPU benchmarks alongside mining or
other GPU jobs. They intentionally target BDF `02:00.0` and require SM75/68 SMs.

## Credits and license

The work builds on the CMP unlock research, particularly
[xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock),
[PZH1gdmu/CMP40HX-Unlock](https://github.com/PZH1gdmu/CMP40HX-Unlock), Jon Pry's
V67 canary research, and NVIDIA's open driver interfaces. See
[NOTICE.md](NOTICE.md) for exact provenance, license notices and changes made
for publication. This adaptation is distributed under
[copyleft-next 0.3.1](LICENSE), with upstream MIT notices retained.

No credentials, mining wallet, raw machine inventory, proprietary firmware,
or host-specific boot installer is included.
