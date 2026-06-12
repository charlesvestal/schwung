#!/usr/bin/env python3
import argparse
import glob
import math
import os
import struct
import subprocess
import sys
from statistics import mean


def ffprobe_stream(path):
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "a:0",
        "-show_entries",
        "stream=sample_rate,sample_fmt",
        "-of",
        "default=noprint_wrappers=1",
        path,
    ]
    out = subprocess.check_output(cmd, text=True)
    info = {}
    for line in out.strip().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            info[key] = value
    return info


def decode_stereo_f32(path):
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-i",
        path,
        "-f",
        "f32le",
        "-acodec",
        "pcm_f32le",
        "-ac",
        "2",
        "-",
    ]
    raw = subprocess.check_output(cmd)
    sample_count = len(raw) // 4
    if sample_count == 0:
        return ()
    return struct.unpack("<%df" % sample_count, raw[: sample_count * 4])


def audio_metrics(path):
    stream = ffprobe_stream(path)
    sample_rate = float(stream.get("sample_rate", "44100"))
    sample_fmt = stream.get("sample_fmt", "?")
    interleaved = decode_stereo_f32(path)
    frame_count = len(interleaved) // 2
    if frame_count == 0:
        return {
            "path": path,
            "fmt": sample_fmt,
            "sr": sample_rate,
            "dur": 0.0,
            "rms": 0.0,
            "low": 0.0,
            "low_rms": 0.0,
            "peak": 0.0,
            "side": 0.0,
        }

    alpha = 1.0 - math.exp(-2.0 * math.pi * 200.0 / sample_rate)
    lp_l = 0.0
    lp_r = 0.0
    sum_l = 0.0
    sum_r = 0.0
    sum_low_l = 0.0
    sum_low_r = 0.0
    sum_mid = 0.0
    sum_side = 0.0
    peak = 0.0

    for i in range(0, len(interleaved), 2):
        left = float(interleaved[i])
        right = float(interleaved[i + 1])
        abs_peak = max(abs(left), abs(right))
        if abs_peak > peak:
            peak = abs_peak

        mid = 0.5 * (left + right)
        side = 0.5 * (left - right)
        sum_l += left * left
        sum_r += right * right
        sum_mid += mid * mid
        sum_side += side * side

        lp_l += alpha * (left - lp_l)
        lp_r += alpha * (right - lp_r)
        sum_low_l += lp_l * lp_l
        sum_low_r += lp_r * lp_r

    rms_l = math.sqrt(sum_l / frame_count)
    rms_r = math.sqrt(sum_r / frame_count)
    rms = 0.5 * (rms_l + rms_r)
    low = 0.5 * (
        math.sqrt(sum_low_l / frame_count) + math.sqrt(sum_low_r / frame_count)
    )
    mid_rms = math.sqrt(sum_mid / frame_count)
    side_rms = math.sqrt(sum_side / frame_count)

    return {
        "path": path,
        "fmt": sample_fmt,
        "sr": sample_rate,
        "dur": frame_count / sample_rate,
        "rms": rms,
        "low": low,
        "low_rms": low / (rms + 1e-12),
        "peak": peak,
        "side": side_rms / (mid_rms + 1e-12),
    }


def print_summary(name, rows):
    if not rows:
        return
    print(f"\n{name}: n={len(rows)}")
    print(f"  avg rms={mean(r['rms'] for r in rows):.4f}")
    print(f"  avg low={mean(r['low'] for r in rows):.4f}")
    print(f"  avg low/rms={mean(r['low_rms'] for r in rows):.3f}")
    print(f"  avg peak={mean(r['peak'] for r in rows):.4f}")
    print(f"  avg side={mean(r['side'] for r in rows):.3f}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Analyze WAV loudness/low-end/stereo metrics using ffmpeg+ffprobe."
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="WAV files, directories, or glob patterns.",
    )
    parser.add_argument(
        "--group-prefix",
        action="append",
        default=[],
        metavar="NAME:PREFIX",
        help='Group summary by file prefix, e.g. --group-prefix "samples:sample_"',
    )
    parser.add_argument(
        "--drop-silence",
        type=float,
        default=0.0,
        metavar="RMS",
        help="Exclude files with RMS below this threshold from group summaries.",
    )
    return parser.parse_args()


def resolve_inputs(items):
    paths = []
    for item in items:
        if any(ch in item for ch in "*?[]"):
            paths.extend(glob.glob(item))
            continue
        if os.path.isdir(item):
            paths.extend(glob.glob(os.path.join(item, "*.wav")))
            paths.extend(glob.glob(os.path.join(item, "*.WAV")))
            continue
        paths.append(item)
    unique = []
    seen = set()
    for p in paths:
        rp = os.path.abspath(p)
        if rp in seen:
            continue
        seen.add(rp)
        unique.append(rp)
    return [p for p in unique if os.path.isfile(p)]


def main():
    args = parse_args()
    files = resolve_inputs(args.inputs)
    if not files:
        print("No WAV files found for inputs.", file=sys.stderr)
        return 1

    rows = []
    for path in sorted(files):
        try:
            rows.append(audio_metrics(path))
        except Exception as exc:
            print(f"ERROR: {path}: {exc}", file=sys.stderr)

    if not rows:
        print("No files analyzed successfully.", file=sys.stderr)
        return 1

    for row in rows:
        base = os.path.basename(row["path"])
        print(
            f"{base:<28} fmt={row['fmt']:<6} dur={row['dur']:7.2f}s "
            f"rms={row['rms']:.4f} low={row['low']:.4f} "
            f"low/rms={row['low_rms']:.3f} peak={row['peak']:.4f} side={row['side']:.3f}"
        )

    if args.group_prefix:
        non_silent = [r for r in rows if r["rms"] >= args.drop_silence]
        for spec in args.group_prefix:
            if ":" not in spec:
                print(f"Skipping invalid --group-prefix value: {spec}", file=sys.stderr)
                continue
            name, prefix = spec.split(":", 1)
            grouped = [
                r for r in non_silent if os.path.basename(r["path"]).startswith(prefix)
            ]
            print_summary(name, grouped)

        valid_specs = [s for s in args.group_prefix if ":" in s]
        if len(valid_specs) == 2:
            n1, p1 = valid_specs[0].split(":", 1)
            n2, p2 = valid_specs[1].split(":", 1)
            g1 = [r for r in non_silent if os.path.basename(r["path"]).startswith(p1)]
            g2 = [r for r in non_silent if os.path.basename(r["path"]).startswith(p2)]
            if g1 and g2:
                g1_rms = mean(r["rms"] for r in g1)
                g2_rms = mean(r["rms"] for r in g2)
                g1_lr = mean(r["low_rms"] for r in g1)
                g2_lr = mean(r["low_rms"] for r in g2)
                delta_db = 20.0 * math.log10((g2_rms + 1e-12) / (g1_rms + 1e-12))
                print(
                    f"\nDelta {n2} vs {n1}: loudness={delta_db:+.2f} dB "
                    f"low/rms={g2_lr - g1_lr:+.3f}"
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
