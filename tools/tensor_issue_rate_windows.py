# SPDX-License-Identifier: copyleft-next-0.3.1
# Adapted from haohervchb/Turing-unlock, tools/tensor_baseline.cu, at
# a188b24a392414f29f931ea886329227cfbc2f98. This entire derived file is licensed
# under copyleft-next 0.3.1. The license is available at LICENSE_URL below.
"""Bounded SM75 tensor issue-rate test, called explicitly by a safety harness.

Importing this module does not initialize CUDA or execute a benchmark. The
caller must provide a check_safety() callback that raises on deadline, thermal,
or other stop conditions. No clock, power, firmware, or GPU register changes
are made. The test uses PyTorch's current CUDA context and current stream.

The CUDA arithmetic/measurement design is adapted from the pinned source below:
four independent MMA accumulator groups, cancelling nonzero +/-1 operands,
1024 threads per CTA, one CTA per SM, SM-cycle timing and exact-result checks.
The adaptation uses NVRTC plus the CUDA driver API and PyTorch-owned buffers.
"""

from __future__ import annotations

import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import statistics
from typing import Callable


UPSTREAM_COMMIT = "a188b24a392414f29f931ea886329227cfbc2f98"
UPSTREAM_URL = (
    "https://github.com/haohervchb/Turing-unlock/blob/"
    + UPSTREAM_COMMIT + "/tools/tensor_baseline.cu"
)
UPSTREAM_SHA256 = "2b93e0e7e5ea74a52b3aaa5c3666b3a5ac031eb002330abbdb8c8648f4c611ba"
LICENSE_URL = (
    "https://github.com/haohervchb/Turing-unlock/blob/"
    + UPSTREAM_COMMIT + "/LICENSE"
)
ROUNDS = 2048
THREADS = 1024
WARMUPS = 2
SAMPLES = 7


CUDA_SOURCE = r'''// SPDX-License-Identifier: copyleft-next-0.3.1
// Derived from haohervchb/Turing-unlock tools/tensor_baseline.cu
// commit a188b24a392414f29f931ea886329227cfbc2f98.
// License: https://github.com/haohervchb/Turing-unlock/blob/
// a188b24a392414f29f931ea886329227cfbc2f98/LICENSE
// Adaptation: header-free NVRTC kernels and packed-half exact-result checks.

__device__ __forceinline__ void mma32(float *c, unsigned a, unsigned b) {
    asm volatile("mma.sync.aligned.m16n8k8.row.col.f32.f16.f16.f32 "
        "{%0,%1,%2,%3}, {%4,%4}, {%5}, {%0,%1,%2,%3};"
        : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3])
        : "r"(a), "r"(b));
}

__device__ __forceinline__ void mma16(unsigned *c, unsigned a, unsigned b) {
    asm volatile("mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 "
        "{%0,%1}, {%2,%2}, {%3}, {%0,%1};"
        : "+r"(c[0]), "+r"(c[1]) : "r"(a), "r"(b));
}

template<bool FP32> __device__ __forceinline__
void issue_body(float *errors, unsigned long long *ticks, unsigned *smids,
                int rounds, unsigned one, unsigned negone) {
    float cf[4][4] = {};
    unsigned ch[4][2] = {};
    __syncthreads();
    unsigned long long start = clock64();
    for (int r = 0; r < rounds; r++) {
        #pragma unroll
        for (int u = 0; u < 8; u++) {
            #pragma unroll
            for (int j = 0; j < 4; j++) {
                if constexpr (FP32) mma32(cf[j], one, one);
                else mma16(ch[j], one, one);
            }
            #pragma unroll
            for (int j = 0; j < 4; j++) {
                if constexpr (FP32) mma32(cf[j], one, negone);
                else mma16(ch[j], one, negone);
            }
        }
    }
    // Every accumulator must end at exactly 8 after cancelling +/-8 pairs.
    #pragma unroll
    for (int j = 0; j < 4; j++) {
        if constexpr (FP32) mma32(cf[j], one, one);
        else mma16(ch[j], one, one);
    }
    __syncthreads();
    unsigned long long end = clock64();
    if (threadIdx.x == 0) {
        ticks[blockIdx.x] = end - start;
        unsigned smid;
        asm("mov.u32 %0, %%smid;" : "=r"(smid));
        smids[blockIdx.x] = smid;
    }
    unsigned bad = 0;
    #pragma unroll
    for (int j = 0; j < 4; j++) {
        if constexpr (FP32) {
            #pragma unroll
            for (int k = 0; k < 4; k++) bad |= (cf[j][k] != 8.0f);
        } else {
            // IEEE binary16 8.0 is 0x4800; check both halves of each register.
            #pragma unroll
            for (int k = 0; k < 2; k++) bad |= (ch[j][k] != 0x48004800u);
        }
    }
    errors[blockIdx.x * blockDim.x + threadIdx.x] = (float)bad;
}

extern "C" __global__ __launch_bounds__(1024, 1)
void issue_fp32(float *errors, unsigned long long *ticks, unsigned *smids,
                int rounds, unsigned one, unsigned negone) {
    issue_body<true>(errors, ticks, smids, rounds, one, negone);
}

extern "C" __global__ __launch_bounds__(1024, 1)
void issue_fp16(float *errors, unsigned long long *ticks, unsigned *smids,
                int rounds, unsigned one, unsigned negone) {
    issue_body<false>(errors, ticks, smids, rounds, one, negone);
}
'''


def _bind(dll, name, args, result=ct.c_int):
    fn = getattr(dll, name)
    fn.argtypes = args
    fn.restype = result
    return fn


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json(path: Path, data) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


class _Nvrtc:
    def __init__(self, dll_path: Path):
        self.dll = ct.CDLL(str(dll_path))
        pvoid = ct.POINTER(ct.c_void_p)
        pchar = ct.POINTER(ct.c_char_p)
        self.create = _bind(self.dll, "nvrtcCreateProgram", [pvoid, ct.c_char_p, ct.c_char_p, ct.c_int, pchar, pchar])
        self.compile = _bind(self.dll, "nvrtcCompileProgram", [ct.c_void_p, ct.c_int, pchar])
        self.log_size = _bind(self.dll, "nvrtcGetProgramLogSize", [ct.c_void_p, ct.POINTER(ct.c_size_t)])
        self.log = _bind(self.dll, "nvrtcGetProgramLog", [ct.c_void_p, ct.c_void_p])
        self.ptx_size = _bind(self.dll, "nvrtcGetPTXSize", [ct.c_void_p, ct.POINTER(ct.c_size_t)])
        self.ptx = _bind(self.dll, "nvrtcGetPTX", [ct.c_void_p, ct.c_void_p])
        self.destroy = _bind(self.dll, "nvrtcDestroyProgram", [pvoid])
        self.err = _bind(self.dll, "nvrtcGetErrorString", [ct.c_int], ct.c_char_p)
        self.version = _bind(self.dll, "nvrtcVersion", [ct.POINTER(ct.c_int), ct.POINTER(ct.c_int)])

    def check(self, status: int, operation: str) -> None:
        if status:
            detail = self.err(status)
            raise RuntimeError(f"{operation}: NVRTC {status}: {detail!r}")

    def build(self, outdir: Path) -> tuple[bytes, dict]:
        program = ct.c_void_p()
        source = CUDA_SOURCE.encode("utf-8")
        (outdir / "tensor_issue_rate.cu").write_bytes(source)
        major, minor = ct.c_int(), ct.c_int()
        self.check(self.version(ct.byref(major), ct.byref(minor)), "nvrtcVersion")
        options = [b"--std=c++17", b"--gpu-architecture=compute_75"]
        opts = (ct.c_char_p * len(options))(*options)
        self.check(self.create(ct.byref(program), source, b"tensor_issue_rate.cu", 0, None, None), "nvrtcCreateProgram")
        try:
            status = self.compile(program, len(options), opts)
            size = ct.c_size_t()
            self.check(self.log_size(program, ct.byref(size)), "nvrtcGetProgramLogSize")
            log = ct.create_string_buffer(max(size.value, 1))
            self.check(self.log(program, log), "nvrtcGetProgramLog")
            (outdir / "nvrtc.log").write_bytes(log.value)
            self.check(status, "nvrtcCompileProgram; see nvrtc.log")
            self.check(self.ptx_size(program, ct.byref(size)), "nvrtcGetPTXSize")
            ptx = ct.create_string_buffer(size.value)
            self.check(self.ptx(program, ptx), "nvrtcGetPTX")
            result = ptx.raw
            (outdir / "tensor_issue_rate.ptx").write_bytes(result.rstrip(b"\0"))
            return result, {
                "nvrtc_version": [major.value, minor.value],
                "compile_options": [x.decode() for x in options],
                "kernel_source_sha256": _sha(source),
                "ptx_sha256": _sha(result.rstrip(b"\0")),
            }
        finally:
            if program.value:
                self.check(self.destroy(ct.byref(program)), "nvrtcDestroyProgram")


class _Driver:
    def __init__(self):
        dll_path = Path(os.environ["WINDIR"]) / "System32" / "nvcuda.dll"
        self.dll = ct.WinDLL(str(dll_path))
        pvoid = ct.POINTER(ct.c_void_p)
        pint = ct.POINTER(ct.c_int)
        self.error_name = _bind(self.dll, "cuGetErrorName", [ct.c_int, ct.POINTER(ct.c_char_p)])
        self.error_string = _bind(self.dll, "cuGetErrorString", [ct.c_int, ct.POINTER(ct.c_char_p)])
        self.ctx_current = _bind(self.dll, "cuCtxGetCurrent", [pvoid])
        self.ctx_device = _bind(self.dll, "cuCtxGetDevice", [pint])
        self.bus_id = _bind(self.dll, "cuDeviceGetPCIBusId", [ct.c_void_p, ct.c_int, ct.c_int])
        self.load = _bind(self.dll, "cuModuleLoadData", [pvoid, ct.c_void_p])
        self.function = _bind(self.dll, "cuModuleGetFunction", [pvoid, ct.c_void_p, ct.c_char_p])
        self.unload = _bind(self.dll, "cuModuleUnload", [ct.c_void_p])
        self.occupancy = _bind(self.dll, "cuOccupancyMaxActiveBlocksPerMultiprocessor", [pint, ct.c_void_p, ct.c_int, ct.c_size_t])
        self.launch = _bind(self.dll, "cuLaunchKernel", [
            ct.c_void_p, ct.c_uint, ct.c_uint, ct.c_uint,
            ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint,
            ct.c_void_p, pvoid, pvoid,
        ])

    def check(self, status: int, operation: str) -> None:
        if status:
            name, detail = ct.c_char_p(), ct.c_char_p()
            self.error_name(status, ct.byref(name))
            self.error_string(status, ct.byref(detail))
            raise RuntimeError(f"{operation}: CUDA {status}: {name.value!r}: {detail.value!r}")


def run_issue_tests(outdir, check_safety: Callable[[], None]) -> dict:
    """Execute 2 warmup + 7 measured launches per mode, alternating modes.

    check_safety is called before every kernel launch and immediately after
    its event completes. The caller owns thermal/deadline policy. Exceptions
    stop the suite; already completed samples remain in issue-samples.jsonl.
    """
    if os.name != "nt":
        raise RuntimeError("This harness currently supports Windows only")
    if not callable(check_safety):
        raise TypeError("A check_safety callback is required")
    check_safety()
    import torch

    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    if any((outdir / p).exists() for p in ("issue-result.json", "issue-samples.jsonl")):
        raise FileExistsError("Use a fresh issue-test output directory")
    torch.cuda.init()
    device = torch.cuda.current_device()
    props = torch.cuda.get_device_properties(device)
    if (props.major, props.minor, props.multi_processor_count) != (7, 5, 68):
        raise RuntimeError("Expected the current 68-SM SM75 device; refusing")
    if "2080 Ti" not in props.name:
        raise RuntimeError(f"Unexpected GPU name: {props.name}")
    libdir = Path(torch.__file__).resolve().parent / "lib"
    nvrtc_dlls = sorted(p for p in libdir.glob("nvrtc64_*.dll") if ".alt." not in p.name)
    if len(nvrtc_dlls) != 1:
        raise RuntimeError(f"Expected one PyTorch NVRTC DLL, found {len(nvrtc_dlls)}")
    provenance = {
        "upstream_url": UPSTREAM_URL, "upstream_commit": UPSTREAM_COMMIT,
        "upstream_source_sha256": UPSTREAM_SHA256,
        "license": "copyleft-next-0.3.1", "license_url": LICENSE_URL,
        "adaptation": "Header-free NVRTC kernels, PyTorch current context/stream, CUDA driver launches, packed-half exact checks, interleaved modes.",
        "nvrtc_dll": nvrtc_dlls[0].name, "torch": torch.__version__,
        "torch_cuda": torch.version.cuda,
        "python_source_sha256": _sha(Path(__file__).read_bytes()),
    }
    _json(outdir / "provenance.json", provenance)
    (outdir / "tensor_issue_rate_windows.py").write_bytes(Path(__file__).read_bytes())
    dll_directory = os.add_dll_directory(str(libdir))
    module = ct.c_void_p()
    driver = None
    stream = None
    try:
        nvrtc = _Nvrtc(nvrtc_dlls[0])
        ptx, compile_metadata = nvrtc.build(outdir)
        provenance.update(compile_metadata)
        _json(outdir / "provenance.json", provenance)
        check_safety()
        # PyTorch initializes/owns the primary context and all device memory.
        errors = torch.empty((68 * THREADS,), dtype=torch.float32, device=device)
        ticks = torch.empty((68,), dtype=torch.int64, device=device)
        smids = torch.empty((68,), dtype=torch.int32, device=device)
        stream = torch.cuda.current_stream(device)
        driver = _Driver()
        ctx, ctx_device = ct.c_void_p(), ct.c_int()
        driver.check(driver.ctx_current(ct.byref(ctx)), "cuCtxGetCurrent")
        driver.check(driver.ctx_device(ct.byref(ctx_device)), "cuCtxGetDevice")
        if not ctx.value or ctx_device.value != device:
            raise RuntimeError("PyTorch current CUDA context/device mismatch")
        bdf_buf = ct.create_string_buffer(32)
        driver.check(driver.bus_id(bdf_buf, len(bdf_buf), ctx_device), "cuDeviceGetPCIBusId")
        ptx_buffer = ct.create_string_buffer(ptx)
        driver.check(driver.load(ct.byref(module), ptx_buffer), "cuModuleLoadData")
        functions, occupancies = {}, {}
        for mode in ("fp32", "fp16"):
            fn, active = ct.c_void_p(), ct.c_int()
            driver.check(driver.function(ct.byref(fn), module, ("issue_" + mode).encode()), "cuModuleGetFunction")
            driver.check(driver.occupancy(ct.byref(active), fn, THREADS, 0), "cuOccupancyMaxActiveBlocksPerMultiprocessor")
            if active.value != 1:
                raise RuntimeError(f"{mode}: expected one active CTA per SM, got {active.value}")
            functions[mode], occupancies[mode] = fn, active.value
        args = [
            ct.c_void_p(errors.data_ptr()), ct.c_void_p(ticks.data_ptr()),
            ct.c_void_p(smids.data_ptr()), ct.c_int(ROUNDS),
            ct.c_uint(0x3C003C00), ct.c_uint(0xBC00BC00),
        ]
        kernel_args = (ct.c_void_p * len(args))(*[ct.cast(ct.byref(a), ct.c_void_p) for a in args])
        # 32 warps/CTA; every m16n8k8 MMA performs 2*16*8*8 FLOPs/warp.
        flops_per_sm = (ROUNDS * 64 + 4) * (THREADS // 32) * 2048
        records = []
        with (outdir / "issue-samples.jsonl").open("x", encoding="utf-8") as log:
            for sample in range(-WARMUPS, SAMPLES):
                # Alternate which mode goes first to reduce ordering bias.
                order = ("fp32", "fp16") if sample % 2 == 0 else ("fp16", "fp32")
                for mode in order:
                    check_safety()
                    errors.fill_(float("nan"))
                    ticks.zero_()
                    smids.fill_(-1)
                    begin = torch.cuda.Event(enable_timing=True)
                    end = torch.cuda.Event(enable_timing=True)
                    begin.record(stream)
                    driver.check(driver.launch(
                        functions[mode], 68, 1, 1, THREADS, 1, 1, 0,
                        ct.c_void_p(stream.cuda_stream), kernel_args, None,
                    ), f"cuLaunchKernel({mode})")
                    end.record(stream)
                    end.synchronize()
                    check_safety()
                    ms = float(begin.elapsed_time(end))
                    host_errors = errors.cpu()
                    cycles = [int(v) for v in ticks.cpu().tolist()]
                    ids = [int(v) for v in smids.cpu().tolist()]
                    exact = bool(torch.isfinite(host_errors).all().item()) and bool((host_errors == 0).all().item())
                    unique_sms = len(set(ids))
                    if not exact or unique_sms != 68 or any(v < 0 for v in ids):
                        raise RuntimeError(f"{mode}: exact_result={exact}, unique_sms={unique_sms}, smids={ids}")
                    if ms <= 0 or any(v <= 0 for v in cycles):
                        raise RuntimeError(f"{mode}: invalid event/cycle timing")
                    median_cycles = statistics.median(cycles)
                    record = {
                        "test": "mma_f16_acc_" + mode, "sample": sample,
                        "warmup": sample < 0, "milliseconds": ms,
                        "tflops": flops_per_sm * 68 / (ms * 1e9),
                        "median_sm_cycles": median_cycles,
                        "flops_per_sm_cycle": flops_per_sm / median_cycles,
                        "cycles_by_block": cycles, "smids_by_block": ids,
                        "unique_sms": unique_sms, "all_accumulators_exact_8": exact,
                    }
                    log.write(json.dumps(record) + "\n")
                    log.flush()
                    records.append(record)
        summary = {}
        for mode in ("fp32", "fp16"):
            rows = [r for r in records if r["test"] == "mma_f16_acc_" + mode and not r["warmup"]]
            summary[mode] = {
                "samples": len(rows),
                "median_flops_per_sm_cycle": statistics.median(r["flops_per_sm_cycle"] for r in rows),
                "median_tflops": statistics.median(r["tflops"] for r in rows),
                "median_milliseconds": statistics.median(r["milliseconds"] for r in rows),
            }
        result = {
            "gpu": props.name, "device": device,
            "compute_capability": [7, 5], "sm_count": 68,
            "threads_per_block": THREADS, "blocks": 68, "rounds": ROUNDS,
            "flops_per_sm": flops_per_sm, "occupancy_blocks_per_sm": occupancies,
            "warmups_per_mode": WARMUPS, "samples_per_mode": SAMPLES,
            "summary": summary,
            "fp32_to_fp16_issue_rate_ratio": summary["fp32"]["median_flops_per_sm_cycle"] / summary["fp16"]["median_flops_per_sm_cycle"],
            "records": records, "provenance": provenance,
            "scope": "Native instruction issue rate and bounded exact-result checks; not application speedup or endurance certification.",
        }
        _json(outdir / "issue-result.json", result)
        return result
    finally:
        # Unload only after queued work is complete. No device reset is used.
        if module.value and driver is not None:
            if stream is not None:
                stream.synchronize()
            driver.check(driver.unload(module), "cuModuleUnload")
        dll_directory.close()
