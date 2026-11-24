#!/usr/bin/env python3
"""Build the CFD-DSA AArch64 guest benchmarks into one generated directory."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[3]
SOURCE_ROOT = REPO / "projects/cfd_dsa/benchmarks"
OUTPUT_ROOT = REPO / "build/cfd_dsa/aarch64"

BENCHMARKS = {
    "decode-exclusive": ("common/test_cfd_dsa_decode_exclusive.c",
                         "test_cfd_dsa_decode_exclusive_arm"),
    "patha": ("patha/test_cfd_patha_extreme_perf.c",
              "test_cfd_patha_extreme_arm"),
    "patha-legacy": ("patha/test_cfd_extreme_perf.c",
                     "test_cfd_extreme_arm"),
    "pathb": ("pathb/test_cfd_pathb_za_pipe_perf.c",
              "test_cfd_pathb_za_pipe_arm"),
    "pathc": ("pathc/test_cfd_pathc_za_outer_perf.c",
              "test_cfd_pathc_za_outer_arm"),
    "sme-squash": ("pathc/test_cfd_sme_za_squash.c",
                   "test_cfd_sme_za_squash_arm"),
    "trsv5": ("trsv5/test_cfd_trsv5.c", "test_cfd_trsv5_arm"),
    "lusgs-step1": ("lusgs/test_cfd_lusgs_patha_step1.c",
                    "test_cfd_lusgs_patha_step1_arm"),
    "lusgs-step2": ("lusgs/test_cfd_lusgs_patha_step2.c",
                    "test_cfd_lusgs_patha_step2_arm"),
    "lusgs-step3": ("lusgs/test_cfd_lusgs_patha_step3.c",
                    "test_cfd_lusgs_patha_step3_arm"),
    "lusgs-step4": ("lusgs/test_cfd_lusgs_patha_step4.c",
                    "test_cfd_lusgs_patha_step4_arm"),
    "cpu-study": ("lusgs/test_cfd_cpu_study.c", "test_cfd_cpu_study_arm"),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("targets", nargs="*", metavar="TARGET")
    parser.add_argument("--cc", default=os.environ.get(
        "AARCH64_CC", "aarch64-linux-gnu-gcc"))
    parser.add_argument("--opt", default="-O3")
    args = parser.parse_args()

    # Opt-in assembly references/baselines needing an SVE/SME-capable assembler.
    asm_targets = {
        "sme-line-isa": ("lusgs/sme_rank1.S", "-DCFD_ARCH_SME_REFERENCE",
                         "test_cfd_lusgs_sme_isa_arm"),
        "sve-line-opt": ("lusgs/sve_opt_kernels.S", "-DCFD_OPT_SVE_KERNEL",
                         "test_cfd_lusgs_sve_opt_arm"),
        "sme-line-opt": ("lusgs/sme_opt_kernels.S", "-DCFD_OPT_SME_KERNEL",
                         "test_cfd_lusgs_sme_opt_arm"),
    }
    unknown = sorted(set(args.targets) - set(BENCHMARKS) - set(asm_targets))
    if unknown:
        parser.error("unknown target(s): " + ", ".join(unknown))
    targets = args.targets or list(BENCHMARKS)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    common_flags = [args.opt, "-static", "-Wall", "-Wextra",
                    "-march=armv8-a+sve"]

    for target in targets:
        if target in asm_targets:
            asm_src, define_flag, out_name = asm_targets[target]
            assembler = (os.environ.get("LLVM_MC") or shutil.which("llvm-mc")
                         or shutil.which("llvm-mc-15"))
            if not assembler:
                parser.error(f"{target} needs an SME-capable llvm-mc (or LLVM_MC)")
            with tempfile.TemporaryDirectory() as tmp:
                obj = Path(tmp) / "kernel.o"
                asm = [assembler, "-triple=aarch64", "-mattr=+sve,+sme,+sme-f64",
                       "-filetype=obj", "-o", str(obj), str(SOURCE_ROOT / asm_src)]
                result = subprocess.run(asm, cwd=REPO)
                if result.returncode:
                    return result.returncode
                command = [args.cc, *common_flags, "-march=armv8-a", "-ffp-contract=off",
                           "-fno-tree-vectorize", define_flag,
                           str(SOURCE_ROOT / "lusgs/test_cfd_cpu_study.c"), str(obj), "-lm",
                           "-o", str(OUTPUT_ROOT / out_name)]
                print("+", " ".join(command))
                result = subprocess.run(command, cwd=REPO)
                if result.returncode:
                    return result.returncode
            continue
        source_name, output_name = BENCHMARKS[target]
        source = SOURCE_ROOT / source_name
        output = OUTPUT_ROOT / output_name
        if not source.is_file():
            print(f"missing source: {source}", file=sys.stderr)
            return 2
        # Keep floating-point association explicit for the CPU comparison.
        # Report the validated O3 NEON baseline. The SVE compiled LU path
        # failed independent-oracle checks in this toolchain/simulator.
        target_flags = ["-ffp-contract=off", "-march=armv8-a"] if target == "cpu-study" else []
        command = [args.cc, *common_flags, *target_flags, str(source), "-o", str(output),
                   "-lm"]
        print("+", " ".join(command))
        completed = subprocess.run(command, cwd=REPO, check=False)
        if completed.returncode:
            return completed.returncode

    print(f"Built {len(targets)} benchmark(s) in {OUTPUT_ROOT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
