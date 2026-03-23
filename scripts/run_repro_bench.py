#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def read_text(path: Path):
    try:
        return path.read_text()
    except Exception:
        return None


def capture_proc_files(pid: int, out_dir: Path):
    for name in ["smaps_rollup", "numa_maps", "status"]:
        src = Path(f"/proc/{pid}/{name}")
        text = read_text(src)
        if text is not None:
            (out_dir / f"proc_{name}.txt").write_text(text)


def run_wrapped(cmd, out_dir: Path, tool: str):
    env = os.environ.copy()
    output_json = out_dir / "benchmark.json"
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    time.sleep(0.5)
    capture_proc_files(proc.pid, out_dir)
    stdout, stderr = proc.communicate()
    (out_dir / "benchmark.stdout.txt").write_text(stdout)
    (out_dir / "benchmark.stderr.txt").write_text(stderr)
    if proc.returncode != 0:
        raise SystemExit(f"benchmark failed with code {proc.returncode}")
    output_json.write_text(stdout)

    if tool in {"perf", "both"} and shutil.which("perf"):
      perf_cmd = [
          "perf", "stat", "-x,",
          "-e", "cycles,instructions,cache-misses,cache-references,dTLB-load-misses,dTLB-loads,page-faults",
          "--",
      ] + cmd
      perf_run = subprocess.run(perf_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
      (out_dir / "perf.stdout.txt").write_text(perf_run.stdout)
      (out_dir / "perf.stderr.txt").write_text(perf_run.stderr)
    if tool in {"vtune", "both"} and shutil.which("vtune"):
      vtune_dir = out_dir / "vtune-result"
      vtune_cmd = ["vtune", "-collect", "memory-access", "-result-dir", str(vtune_dir), "--"] + cmd
      vtune_run = subprocess.run(vtune_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
      (out_dir / "vtune.stdout.txt").write_text(vtune_run.stdout)
      (out_dir / "vtune.stderr.txt").write_text(vtune_run.stderr)
    if shutil.which("numastat"):
      numastat_run = subprocess.run(["numastat", "-p", str(proc.pid)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
      (out_dir / "numastat.txt").write_text(numastat_run.stdout + "\n" + numastat_run.stderr)


def main():
    parser = argparse.ArgumentParser(description="Run reproducible TurboMem experiments with optional perf/VTune collection.")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--allocator", default="turbomem", choices=["turbomem", "malloc"])
    parser.add_argument("--scenario", default="packet-burst")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--operations", type=int, default=200000)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--pool-capacity", type=int, default=1 << 15)
    parser.add_argument("--local-cache", type=int, default=64)
    parser.add_argument("--bulk-size", type=int, default=32)
    parser.add_argument("--object-size", type=int, default=256)
    parser.add_argument("--hold-ms", type=int, default=1500)
    parser.add_argument("--tool", default="none", choices=["none", "perf", "vtune", "both"])
    parser.add_argument("--out-dir", default="artifacts/latest-run")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    binary = Path(args.build_dir) / "bench_turbomem"
    if not binary.exists():
        raise SystemExit(f"missing benchmark binary: {binary}")

    cmd = [
        str(binary),
        "--json",
        "--allocator", args.allocator,
        "--scenario", args.scenario,
        "--threads", str(args.threads),
        "--operations", str(args.operations),
        "--batch-size", str(args.batch_size),
        "--pool-capacity", str(args.pool_capacity),
        "--local-cache", str(args.local_cache),
        "--bulk-size", str(args.bulk_size),
        "--object-size", str(args.object_size),
        "--hold-ms", str(args.hold_ms),
    ]
    run_wrapped(cmd, out_dir, args.tool)


if __name__ == "__main__":
    main()
