# SPDX-License-Identifier: copyleft-next-0.3.1
"""Bounded TU102 compute validation; no register/clock/power/firmware writes.

Uses the already installed PyTorch CUDA libraries. The separate instruction
test checks SM75 issue rate; GEMM measures application-library throughput.
Neither invents a percentage gain without a matched stock baseline.
"""
import argparse
import ctypes as ct
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

# Imported only after command-line validation; --help and host tests use no CUDA.
np = None
torch = None


def normalize_bdf(value):
    """Normalize CUDA/NVML's four- or eight-digit PCI domain notation."""
    match = re.fullmatch(r"([0-9a-fA-F]{4}|[0-9a-fA-F]{8}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])", value)
    if not match or int(match[3], 16) > 31:
        raise ValueError("Expected PCI address dddd:bb:dd.f or dddddddd:bb:dd.f")
    domain, bus, device, function = (int(part, 16) for part in match.groups())
    return f"{domain:08x}:{bus:02x}:{device:02x}.{function:x}"


def parse_telemetry(output, expected_bdf):
    fields = [v.strip() for v in output.strip().split(",")]
    if len(fields) != 8 or normalize_bdf(fields[0]) != expected_bdf:
        raise RuntimeError("Unexpected GPU telemetry identity/format")
    row = dict(zip(
        ["temperature_c", "gpu_percent", "power_w", "power_limit_w", "sm_clock_mhz", "memory_clock_mhz", "memory_used_mib"],
        map(float, fields[1:]),
    ))
    if not all(math.isfinite(value) and value >= 0 for value in row.values()):
        raise RuntimeError("Invalid GPU telemetry value")
    return row


def find_nvidia_smi():
    located = shutil.which("nvidia-smi")
    if located:
        return located
    fallback = Path(os.environ.get("WINDIR", "")) / "System32" / "nvidia-smi.exe"
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("nvidia-smi is required for thermal monitoring")


class Monitor:
    def __init__(self, outdir, smi, bdf, max_seconds=120, max_temp=80):
        self.outdir = outdir
        self.smi = smi
        self.bdf = bdf
        self.max_seconds = max_seconds
        self.started = time.monotonic()
        self.deadline = self.started + max_seconds
        self.max_temp = max_temp
        self.samples = []
        self.error = None
        self.stop = threading.Event()
        self.thread = None

    def sample(self):
        query = "pci.bus_id,temperature.gpu,utilization.gpu,power.draw,power.limit,clocks.current.sm,clocks.current.memory,memory.used"
        result = subprocess.run(
            [self.smi, "-i", self.bdf, "--query-gpu=" + query, "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5, check=True,
        )
        row = parse_telemetry(result.stdout, self.bdf)
        row.update(elapsed_s=time.monotonic() - self.started)
        self.samples.append(row)
        with (self.outdir / "telemetry.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(row) + "\n")
        if row["temperature_c"] >= self.max_temp:
            self.error = f"Temperature reached {row['temperature_c']} C; limit {self.max_temp} C"
        return row

    def background(self):
        while not self.stop.wait(0.75):
            try:
                self.sample()
            except Exception as e:
                self.error = "Telemetry failed: " + type(e).__name__
            if time.monotonic() > self.deadline + 5:
                # A stalled CUDA call must not make the bounded test indefinite.
                (self.outdir / "watchdog-stop.txt").write_text("Process deadline exceeded\n")
                os._exit(124)

    def start(self):
        self.sample()
        self.check()
        self.thread = threading.Thread(target=self.background, daemon=True)
        self.thread.start()

    def check(self):
        if self.error:
            raise RuntimeError(self.error)
        if time.monotonic() > self.deadline:
            raise RuntimeError("Benchmark exceeded its wall-time budget")

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=6)


class Blas:
    """Explicit cuBLAS compute types and current-stream binding."""
    def __init__(self):
        self.libdir = Path(torch.__file__).parent / "lib"
        self.dll_dir = os.add_dll_directory(str(self.libdir))
        dlls = sorted(p for p in self.libdir.glob("cublas64_*.dll") if re.fullmatch(r"cublas64_\d+\.dll", p.name))
        if len(dlls) != 1:
            self.dll_dir.close()
            raise RuntimeError("Expected exactly one cuBLAS DLL in PyTorch's library directory")
        self.dll = ct.WinDLL(str(dlls[0]))
        self.create = self.bind("cublasCreate_v2", [ct.POINTER(ct.c_void_p)])
        self.destroy = self.bind("cublasDestroy_v2", [ct.c_void_p])
        self.set_stream = self.bind("cublasSetStream_v2", [ct.c_void_p, ct.c_void_p])
        self.set_math = self.bind("cublasSetMathMode", [ct.c_void_p, ct.c_int])
        self.get_version = self.bind("cublasGetVersion_v2", [ct.c_void_p, ct.POINTER(ct.c_int)])
        p, i = ct.c_void_p, ct.c_int
        self.gemm = self.bind("cublasGemmEx", [p, i, i, i, i, i, p, p, i, i, p, i, i, p, p, i, i, i, i])
        self.handle = ct.c_void_p()
        self.ok(self.create(ct.byref(self.handle)), "create")
        self.ok(self.set_stream(self.handle, ct.c_void_p(torch.cuda.current_stream().cuda_stream)), "stream")
        version = ct.c_int()
        self.ok(self.get_version(self.handle, ct.byref(version)), "version")
        self.version = version.value

    def bind(self, name, args):
        f = getattr(self.dll, name)
        f.argtypes, f.restype = args, ct.c_int
        return f

    @staticmethod
    def ok(status, label):
        if status != 0:
            raise RuntimeError(f"cuBLAS {label} failed: {status}")

    def operation(self, a, b, c, mode):
        n = a.shape[0]
        is_half_acc = mode == "fp16_acc_fp16"
        is_fp32_input = mode == "fp32_control"
        alpha = ct.c_uint16(0x3c00) if is_half_acc else ct.c_float(1.0)
        beta = ct.c_uint16(0) if is_half_acc else ct.c_float(0.0)
        input_type = 0 if is_fp32_input else 2  # CUDA_R_32F / CUDA_R_16F
        output_type = 2 if is_half_acc else 0
        compute = 64 if is_half_acc else 68  # CUBLAS_COMPUTE_16F / _32F
        algorithm = -1 if is_fp32_input else 99  # default / default tensor op

        def select():
            # Flag value 16 disallows reduced-precision reductions for FP32 accumulation.
            self.ok(self.set_math(self.handle, 0 if is_half_acc else 16), "math mode")

        def run():
            # Row-major C=A@B is column-major C^T=B^T@A^T.
            self.ok(self.gemm(
                self.handle, 0, 0, n, n, n, ct.byref(alpha),
                ct.c_void_p(b.data_ptr()), input_type, n,
                ct.c_void_p(a.data_ptr()), input_type, n,
                ct.byref(beta), ct.c_void_p(c.data_ptr()), output_type, n,
                compute, algorithm,
            ), mode)
        return select, run

    def close(self):
        self.ok(self.destroy(self.handle), "destroy")
        self.dll_dir.close()


def assess(got, reference, mode):
    if not np.isfinite(got).all() or not np.isfinite(reference).all():
        raise RuntimeError("Nonfinite matrix output")
    error = got.astype(np.float64) - reference
    max_abs = float(np.abs(error).max())
    relative_norm = float(np.linalg.norm(error.ravel()) / max(np.linalg.norm(reference.ravel()), 1e-30))
    limit = 0.01 if mode == "fp16_acc_fp16" else 5e-5
    if relative_norm > limit:
        raise RuntimeError(f"{mode} numerical error {relative_norm} exceeds {limit}")
    return dict(max_abs_error=max_abs, relative_l2_error=relative_norm, relative_l2_limit=limit, passed=True)


def full_correctness(blas, monitor):
    rows = []
    generator = torch.Generator(device="cuda").manual_seed(20261029)
    a16 = torch.randn((256, 256), dtype=torch.float16, device="cuda", generator=generator) / 4
    b16 = torch.randn((256, 256), dtype=torch.float16, device="cuda", generator=generator) / 4
    reference = a16.cpu().numpy().astype(np.float64) @ b16.cpu().numpy().astype(np.float64)
    for mode in ("fp16_acc_fp32", "fp16_acc_fp16", "fp32_control"):
        monitor.check()
        a, b = (a16.float(), b16.float()) if mode == "fp32_control" else (a16, b16)
        c = torch.empty((256, 256), dtype=torch.float16 if mode == "fp16_acc_fp16" else torch.float32, device="cuda")
        select, run = blas.operation(a, b, c, mode)
        select()
        run()
        torch.cuda.synchronize()
        row = dict(mode=mode, n=256, reference="complete CPU FP64 product")
        row.update(assess(c.cpu().numpy(), reference, mode))
        rows.append(row)
    return rows


def timed_batch(op, count, monitor):
    monitor.check()
    begin = torch.cuda.Event(enable_timing=True)
    end = torch.cuda.Event(enable_timing=True)
    begin.record()
    for _ in range(count):
        op()
    end.record()
    end.synchronize()
    monitor.check()
    elapsed = begin.elapsed_time(end)
    if elapsed <= 0:
        raise RuntimeError("Invalid CUDA event interval")
    return elapsed


def sampled_correctness(a, b, c, mode):
    n = a.shape[0]
    rng = np.random.default_rng(49004 + n)
    row_ids, col_ids = rng.integers(0, n, size=(2, 32))
    ri = torch.tensor(row_ids, device="cuda", dtype=torch.int64)
    ci = torch.tensor(col_ids, device="cuda", dtype=torch.int64)
    lhs = a[ri, :].cpu().numpy().astype(np.float64)
    rhs = b[:, ci].t().contiguous().cpu().numpy().astype(np.float64)
    reference = np.sum(lhs * rhs, axis=1)
    got = c[ri, ci].cpu().numpy()
    check = assess(got, reference, mode)
    check.update(samples=32, row_indices=row_ids.tolist(), column_indices=col_ids.tolist())
    return check


def benchmark_shape(blas, n, monitor):
    generator = torch.Generator(device="cuda").manual_seed(12345 + n)
    a16 = torch.randn((n, n), device="cuda", dtype=torch.float16, generator=generator) / 4
    b16 = torch.randn((n, n), device="cuda", dtype=torch.float16, generator=generator) / 4
    modes = ["fp16_acc_fp32", "fp16_acc_fp16"]
    if n == 4096:
        modes.append("fp32_control")
    cases = {}
    for mode in modes:
        a, b = (a16.float(), b16.float()) if mode == "fp32_control" else (a16, b16)
        c = torch.empty((n, n), device="cuda", dtype=torch.float16 if mode == "fp16_acc_fp16" else torch.float32)
        select, op = blas.operation(a, b, c, mode)
        select()
        for _ in range(5):
            op()
        torch.cuda.synchronize()
        single = timed_batch(op, 1, monitor)
        batch = max(1, min(64, int(100 / single)))
        until = time.monotonic() + 0.5
        while time.monotonic() < until:
            timed_batch(op, batch, monitor)
        cases[mode] = dict(a=a, b=b, c=c, select=select, op=op, batch=batch, windows=[])

    # Alternate ordering to limit temperature/order bias.
    for sample in range(7):
        for mode in modes if sample % 2 == 0 else reversed(modes):
            case = cases[mode]
            case["select"]()
            started = time.monotonic() - monitor.started
            elapsed_ms, iterations = 0.0, 0
            while elapsed_ms < 250:
                elapsed_ms += timed_batch(case["op"], case["batch"], monitor)
                iterations += case["batch"]
            ended = time.monotonic() - monitor.started
            tf = 2.0 * n ** 3 * iterations / (elapsed_ms * 1e9)
            case["windows"].append(dict(sample=sample, iterations=iterations, elapsed_ms=elapsed_ms,
                                        tflops=tf, started_s=started, ended_s=ended))
            print(json.dumps(dict(stage="gemm", mode=mode, n=n, sample=sample, tflops=round(tf, 3))), flush=True)
    rows = []
    for mode, case in cases.items():
        monitor.check()
        case["select"]()
        case["op"]()
        torch.cuda.synchronize()
        correctness = sampled_correctness(case["a"], case["b"], case["c"], mode)
        rates = [w["tflops"] for w in case["windows"]]
        rows.append(dict(mode=mode, n=n, input_dtype=str(case["a"].dtype), output_dtype=str(case["c"].dtype),
                         compute_type="CUBLAS_COMPUTE_16F" if mode == "fp16_acc_fp16" else "CUBLAS_COMPUTE_32F",
                         median_tflops=statistics.median(rates), min_tflops=min(rates), max_tflops=max(rates),
                         batch=case["batch"], windows=case["windows"], correctness=correctness))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--pci-bus-id", required=True, type=normalize_bdf,
                        help="Expected GPU PCI address, obtained separately with nvidia-smi")
    parser.add_argument("--run", action="store_true", help="Explicitly authorize this GPU workload")
    parser.add_argument("--max-seconds", type=int, default=120, choices=range(30, 121), metavar="30..120")
    parser.add_argument("--max-temp", type=int, default=80, choices=range(50, 81), metavar="50..80")
    args = parser.parse_args()
    if not args.run:
        parser.error("--run is required to execute GPU workloads")
    if os.name != "nt":
        parser.error("This harness currently supports Windows only")
    global np, torch
    import numpy as np
    import torch
    args.out.mkdir(parents=True, exist_ok=False)
    result = dict(status="running", matched_stock_baseline=False)
    monitor, blas = Monitor(args.out, find_nvidia_smi(), args.pci_bus_id, args.max_seconds, args.max_temp), None
    try:
        if not torch.cuda.is_available() or torch.cuda.device_count() != 1:
            raise RuntimeError("Expected exactly one CUDA GPU")
        torch.cuda.set_device(0)
        p = torch.cuda.get_device_properties(0)
        if (p.major, p.minor, p.multi_processor_count) != (7, 5, 68) or "2080 Ti" not in p.name:
            raise RuntimeError("CUDA GPU identity mismatch")
        from tensor_issue_rate_windows import _Driver, run_issue_tests
        driver = _Driver()
        ctx_device = ct.c_int()
        driver.check(driver.ctx_device(ct.byref(ctx_device)), "cuCtxGetDevice")
        bdf = ct.create_string_buffer(32)
        driver.check(driver.bus_id(bdf, len(bdf), ctx_device), "cuDeviceGetPCIBusId")
        if normalize_bdf(bdf.value.decode()) != args.pci_bus_id:
            raise RuntimeError("CUDA GPU does not match the requested PCI address")
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cuda.matmul.allow_fp16_accumulation = False
        torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
        result["device"] = dict(name=p.name, sms=p.multi_processor_count,
                                capability=[p.major, p.minor],
                                total_memory_bytes=p.total_memory, torch=torch.__version__, cuda=torch.version.cuda)
        monitor.start()
        print("Running SM75 instruction-rate checks", flush=True)
        result["tensor_issue"] = run_issue_tests(args.out, check_safety=monitor.check)
        (args.out / "instruction-results.json").write_text(json.dumps(result["tensor_issue"], indent=2))
        blas = Blas()
        result["cublas_version"] = blas.version
        result["full_correctness"] = full_correctness(blas, monitor)
        result["gemm"] = []
        for n in (4096, 8192):
            result["gemm"].extend(benchmark_shape(blas, n, monitor))
            (args.out / "partial-results.json").write_text(json.dumps(result, indent=2))
        monitor.check()
        result["status"] = "passed"
    except Exception as e:
        result["status"] = "failed"
        # Library exceptions/tracebacks can contain private installation paths.
        result["error_type"] = type(e).__name__
        print("Benchmark failed (" + type(e).__name__ + "); retained completed results", file=sys.stderr)
    finally:
        if blas:
            try:
                blas.close()
            except Exception as cleanup_error:
                result["status"] = "failed"
                result["cleanup_error_type"] = type(cleanup_error).__name__
        monitor.close()
        result["elapsed_s"] = time.monotonic() - monitor.started
        result["telemetry"] = monitor.samples
        if monitor.samples:
            result["max_temperature_c"] = max(s["temperature_c"] for s in monitor.samples)
            result["max_power_w"] = max(s["power_w"] for s in monitor.samples)
        result["peak_torch_allocated_bytes"] = torch.cuda.max_memory_allocated() if torch.cuda.is_initialized() else None
        (args.out / "results.json").write_text(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
