# Bounded Windows tensor validation

`tools/tensor_validate_windows.py` adapts the native SM75 issue-rate test to
Windows without a separate `nvcc` build. It uses the installed CUDA-enabled
PyTorch distribution's NVRTC and cuBLAS libraries, the NVIDIA CUDA driver,
NumPy, and `nvidia-smi`. It does not change GPU clocks, power limits, firmware,
registers or boot configuration, and it does not apply an unlock.

The current scope remains a single CUDA-visible RTX 2080 Ti with compute
capability 7.5 and 68 SMs. The caller must supply the expected PCI address;
both CUDA and telemetry must identify that same GPU. Other devices refuse
before the benchmark kernels run. A second visible CUDA device also refuses.

## Running

Use 64-bit Windows and Python 3.10 or newer, NumPy, an NVIDIA driver, and a
CUDA-enabled PyTorch wheel containing one ordinary `nvrtc64_*.dll` and one
`cublas64_*.dll` in its `lib` directory. The underlying workload was exercised
with Python 3.10, PyTorch 2.9.1+cu130, CUDA/NVRTC 13.0, and cuBLAS 13.0.0.
Other dependency versions are unvalidated. Dependencies are not installed by
the script. A CPU-only PyTorch wheel is insufficient.

Stop other GPU workloads first. Determine the intended address with:

```powershell
nvidia-smi --query-gpu=name,pci.bus_id --format=csv
```

Replace `dddd:bb:dd.f` below with that address, and select a new output folder.
Both four- and eight-digit PCI domain notation are accepted.

```powershell
python tools/tensor_validate_windows.py --pci-bus-id dddd:bb:dd.f --out results/windows-run --run
```

`--run` is required. `--help` and the host tests do not import PyTorch or start
CUDA. The default wall-time budget is 120 seconds and the temperature stop
threshold is 80 C; `--max-seconds 30..120` and `--max-temp 50..80` can lower
those limits. An existing output directory refuses rather than overwriting
evidence. Exit status is zero only when the complete suite and cleanup pass.

The monitor samples `nvidia-smi` roughly every 0.75 seconds. Invalid, missing,
nonfinite or mismatched telemetry stops subsequent batches. Limits are checked
between kernel launches and timed batches; they cannot interrupt an in-flight
GPU kernel. A monitor-thread watchdog exits the process if a CUDA call exceeds
the deadline plus five seconds, with additional delay possible during the
five-second telemetry query timeout. This is a bounded diagnostic workload,
not a hardware-level protection mechanism or endurance test.

## Measurements and interpretation

The instruction test retains the CUDA arithmetic from
[`tensor_baseline.cu` at a188b24a](https://github.com/haohervchb/Turing-unlock/blob/a188b24a392414f29f931ea886329227cfbc2f98/tools/tensor_baseline.cu):

- Explicit SM75 `mma.sync.aligned.m16n8k8` variants for FP16 inputs with
  FP32 or FP16 accumulation, four independent accumulator groups, and
  cancelling nonzero positive/negative operands.
- One 1024-thread block per SM; the driver occupancy query must permit
  exactly one active block per SM. All 68 distinct SM identifiers must appear.
- Two warmup launches and seven measured launches per mode, alternating order.
  Per-SM `clock64` intervals provide FLOP/SM/clock, alongside CUDA-event timings.
- Every thread checks that every accumulator ends at exactly 8. The packed
  half-precision check is bit-exact and does not need CUDA toolkit headers.

The cuBLAS section uses explicit `cublasGemmEx` compute types and FP32 output
for the FP32-accumulation target, with reduced-precision reductions disabled.
It runs 4096 and 8192 square products using seeded nonzero inputs, warmup,
preallocated buffers and seven interleaved timing windows of at least 250 ms.
FP16 accumulation and ordinary FP32 are controls. A complete 256-square CPU
FP64 reference and 32 independent dot products per larger case check numerical
error. The target tolerance is relative L2 error <= 5e-5; the lower-precision
FP16 control uses a separate 1% limit.

Near-equal FP32/FP16 instruction rates around 1024 FLOP/SM/clock support
full-rate behavior in the current session. This does not establish a speedup
over the same card's stock state, all application performance, integer tensor
throughput, selector register values after driver initialization, persistent
unlocking, or long-term stability. The FP16 control's different output dtype
also changes memory traffic, so its GEMM ratio is not an isolated accumulation
speed ratio.

## Evidence and privacy

The output includes sample/summary JSON, telemetry with relative elapsed time,
kernel source, compiled PTX, compiler diagnostics and source/provenance hashes.
The result schema deliberately omits GPU UUIDs, serial numbers, hostnames,
PCI addresses, wall-clock timestamps and absolute installation paths. It
records only the NVRTC DLL basename. No data is uploaded by these tools.
Review any locally generated compiler diagnostics before sharing them.

Failure results retain the exception type rather than raw library exception
text or a Python traceback, which could include private installation paths.
Already completed samples remain available. Process-watchdog termination may
leave the main `results.json` incomplete; `watchdog-stop.txt` identifies that
case. A result marked `running`, incomplete artifacts, or only successful
instruction samples is not a complete suite pass.

## Validation boundary and licensing

An earlier machine-pinned version of this same CUDA math and cuBLAS workload
ran successfully once on an MSI 22 GiB RTX 2080 Ti after a separate guarded
preboot transaction: approximately 1021.36 and 1020.99 FLOP/SM/clock for FP32
and FP16 accumulation (ratio 1.00036), 78.04/79.97 TFLOPS for target GEMMs,
and a peak sampled temperature of 60 C. Exact and numerical checks passed.
There was no matched stock baseline. The observed workload lasted about
14 seconds; that result is not an endurance claim.

The publication version changes CLI selection, telemetry identity comparison,
DLL discovery and privacy handling. Those changes have host-only tests and
syntax checks; this exact revised script has **not** been run on a GPU.

Run the offline checks with:

```powershell
python -m py_compile tools/tensor_issue_rate_windows.py tools/tensor_validate_windows.py
python tools/test_tensor_windows_host.py
```

The adaptation and harness are distributed under copyleft-next 0.3.1, matching
this repository's [LICENSE](../LICENSE). The module and generated CUDA source
retain their source attribution and pin. NVIDIA libraries and PyTorch remain
external dependencies with their own licenses; no binary library, firmware,
ROM image, or generated PTX is committed with this contribution.
