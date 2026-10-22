# Validation record — 2026-10-22

## Physical v4 run

The private-profile EFI completed native FWSEC followed by the pinned
Booter/V67 sequence. Its log reported selectors `88888888/8`, stage 11,
`result=0`, `mutated=1`, `unlocked=1`, `quiescent=1`, successful FreePages,
and unchanged PCI/root attributes after cleanup. Linux then booted normally.

Historical test binary SHA-256:

`8e589bd59e199efdbbb1a1c15794b6391ab80864eb09f728a4471b5bc8e3f645`

Historical complete preboot log SHA-256:

`f992762f2aa4374dc20bfe6f9409842081f373a6ee03f187d6a23bca73d92984`

Those files are retained privately. The public checkout has a sanitized,
unconfigured ESP profile and will not reproduce that EFI hash. A Linux reboot
command preceded the successful run, but intervening physical power history
was not independently established. Do not infer reliable warm-reboot or
cold-boot repeatability from a single success.

The runtime power limit was 260 W for the compute measurements; the same
existing V/F/memory/core-cap settings were retained. Baseline and unlocked
environments were not perfectly identical: the second GPU/display topology
changed. Per-SM-cycle MMA is the strongest evidence for the approximately
2× issue-rate change, rather than the absolute TFLOPS alone.

## Throughput and correctness

The unchanged native benchmark uses SM75 `m16n8k8` inline PTX, 68 SMs,
one 1024-thread CTA per SM, two warm-ups and seven timing samples. It checks
the exact simple arithmetic result and observes all 68 unique SMs. FP16
accumulation is an unchanged-rate control. Raw measurements are in
`results/2080ti-{locked,unlocked}-tensor.jsonl`.

The original 8192 cuBLAS throughput case uses zero inputs; its separate 256
CPU-reference check passed. The additional validator uses nonzero exact-dyadic
inputs and independently computes CPU integer dot products. Ten cases cover
all transpose combinations, irregular dimensions, nontrivial alpha/beta, and
leading-dimension padding. It checks 295,035 outputs in total with zero error.
The 8192 case is sampled at 128 outputs, not fully checked. Its additional
85.3544 TFLOPS result has no corresponding locked run of that new test.

After testing, RM still reported full FMLA32/IMLA4. No matching new NVIDIA Xid,
fallen-off-bus, machine-check or hardware-error messages were observed. This
does not establish long-duration stability, exhaustive floating-point behavior,
or an integer instruction speedup.

## Pearl mining

The existing PeakMiner 2.19.1 binary ran unprivileged on the 2080 Ti only, using
the user's existing pool account, with its own CMP unlock disabled. The runtime
power limit was temporarily set to 190 W to match the saved baseline. Clock,
memory, V/F and fan settings were not changed by the trial.

The mean of 24 rolling API samples after the first 60 seconds was
85.1099 TH/s (range 84.1295–85.8698), at approximately 184.8 W reported draw
and 1304 MHz mean core clock. Three shares were accepted and none invalid.
The planned five-minute run stopped after about three minutes at its 75°C
cutoff. The miner stopped, 260 W was restored, and the unlock remained active.

The reference was a user-confirmed **approximate historical 86 TH/s**, not a
fresh interleaved A/B experiment. Rolling-rate samples are correlated, and
three shares cannot establish a precise long-term pool rate. The supported
conclusion is **no meaningful observed Pearl improvement**, not a precise
regression percentage or a proven explanation of PeakMiner's internal kernel.

## Subsequent two-card boot

After a later normal reboot with the 5060 Ti reinstalled, both cards were
recognized and mining. The 2080 Ti remained at `02:00.0` but became NVIDIA
index 1; the 5060 Ti was index 0 at `01:00.0`. Read-only RM GET showed FMLA32
and IMLA4 back at half rate on the 2080 Ti. This confirms that the previous
unlock did not persist across that restart. It is **not** a successful
two-card unlock test. No GPU reset, clock change, or mining interruption was
performed for this observation.
