#!/usr/bin/env python3
"""Scan a local sample folder and upload a Move MCP sample index.

Example:
  python3 scan_samples.py ~/Samples \
    --move-root /data/UserData/UserLibrary/Samples \
    --upload http://move.local:7700 \
    --token "$MOVE_MCP_TOKEN"
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import wave
from pathlib import Path
from typing import Any

try:
    import aifc
except ModuleNotFoundError:  # Python 3.13 removed aifc; ffprobe can cover AIFF.
    aifc = None


AUDIO_EXTS = {
    ".wav",
    ".wave",
    ".aif",
    ".aiff",
    ".aifc",
    ".flac",
    ".mp3",
    ".m4a",
    ".ogg",
}

TAG_KEYWORDS = {
    "kick",
    "bd",
    "snare",
    "sd",
    "clap",
    "rim",
    "hat",
    "hihat",
    "oh",
    "ch",
    "ride",
    "crash",
    "perc",
    "tom",
    "bass",
    "sub",
    "chord",
    "stab",
    "pad",
    "lead",
    "arp",
    "pluck",
    "vocal",
    "vox",
    "voice",
    "loop",
    "break",
    "drum",
    "fx",
    "noise",
    "texture",
    "field",
}

MOOD_KEYWORDS = {
    "dub",
    "deep",
    "dark",
    "warm",
    "dusty",
    "lofi",
    "raw",
    "ambient",
    "space",
    "minor",
    "major",
    "soul",
    "jazz",
    "techno",
    "house",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", help="Local folder to scan on the Mac")
    parser.add_argument("--move-root", default="/data/UserData/UserLibrary/Samples",
                        help="Path prefix stored in the Move sample index")
    parser.add_argument("--output", default="sample-index.json",
                        help="Output JSON path, or '-' for stdout")
    parser.add_argument("--upload", help="Schwung Manager base URL, e.g. http://move.local:7700")
    parser.add_argument("--token", default=os.environ.get("MOVE_MCP_TOKEN", ""),
                        help="Move MCP token, or MOVE_MCP_TOKEN env var")
    parser.add_argument("--no-ffprobe", action="store_true", help="Disable optional ffprobe metadata")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N files, for quick tests")
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        parser.error(f"root is not a directory: {root}")

    index = build_index(root, args.move_root, use_ffprobe=not args.no_ffprobe, limit=args.limit)
    payload = json.dumps(index, indent=2, ensure_ascii=False).encode("utf-8") + b"\n"

    if args.output == "-":
        sys.stdout.buffer.write(payload)
    else:
        out = Path(args.output).expanduser()
        out.write_bytes(payload)
        print(f"Wrote {out} ({index['count']} samples)")

    if args.upload:
        upload_index(args.upload, payload, args.token)
        print(f"Uploaded {index['count']} samples to {args.upload.rstrip('/')}/api/mcp/samples/index")

    return 0


def build_index(root: Path, move_root: str, use_ffprobe: bool, limit: int) -> dict[str, Any]:
    ffprobe = shutil.which("ffprobe") if use_ffprobe else None
    samples: list[dict[str, Any]] = []
    warnings: list[str] = []

    for path in iter_audio_files(root):
        if limit and len(samples) >= limit:
            break
        try:
            samples.append(scan_file(path, root, move_root, ffprobe))
        except Exception as exc:  # keep a bad file from killing the whole scan
            warnings.append(f"{path}: {exc}")

    summary = summarize(samples)
    return {
        "schema": "move-mcp-sample-index-v1",
        "generated_at": utc_now(),
        "source_root": str(root),
        "move_root": move_root.rstrip("/"),
        "count": len(samples),
        "samples": samples,
        "summary": summary,
        "analyzer": {
            "name": "move-mcp scan_samples.py",
            "version": "1",
            "ffprobe": "yes" if ffprobe else "no",
        },
        "warnings": warnings[:200],
    }


def iter_audio_files(root: Path):
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if path.name.startswith("."):
            continue
        if path.suffix.lower() in AUDIO_EXTS:
            yield path


def scan_file(path: Path, root: Path, move_root: str, ffprobe: str | None) -> dict[str, Any]:
    rel = path.relative_to(root)
    rel_posix = rel.as_posix()
    move_path = posixpath.join(move_root.rstrip("/"), rel_posix)
    stat = path.stat()
    name = path.stem
    ext = path.suffix.lower()

    metadata: dict[str, Any] = {}
    source = "filename"
    if ext in {".wav", ".wave"}:
        metadata.update(read_wave_header(path))
        source = "filename+wav_header"
    elif ext in {".aif", ".aiff", ".aifc"} and aifc is not None:
        metadata.update(read_aiff_header(path))
        source = "filename+aiff_header"

    if ffprobe and missing_audio_metadata(metadata):
        probed = read_ffprobe(path, ffprobe)
        if probed:
            metadata.update({k: v for k, v in probed.items() if v is not None})
            source = source + "+ffprobe" if source != "filename" else "filename+ffprobe"

    inferred = infer_from_name(name, rel_posix, metadata.get("duration_sec"))
    metadata.update({k: v for k, v in inferred.items() if v is not None and v != []})

    sample = {
        "id": sample_id(move_path),
        "path": move_path,
        "relative_path": rel_posix,
        "name": name,
        "ext": ext,
        "size": stat.st_size,
        "mtime": utc_from_timestamp(stat.st_mtime),
        "metadata_source": source,
    }
    sample.update(metadata)
    return sample


def read_wave_header(path: Path) -> dict[str, Any]:
    with wave.open(str(path), "rb") as wf:
        frames = wf.getnframes()
        rate = wf.getframerate()
        width = wf.getsampwidth()
        return {
            "duration_sec": round(frames / rate, 6) if rate else None,
            "sample_rate": rate,
            "channels": wf.getnchannels(),
            "bit_depth": width * 8,
        }


def read_aiff_header(path: Path) -> dict[str, Any]:
    if aifc is None:
        return {}
    with aifc.open(str(path), "rb") as af:
        frames = af.getnframes()
        rate = af.getframerate()
        width = af.getsampwidth()
        return {
            "duration_sec": round(frames / rate, 6) if rate else None,
            "sample_rate": rate,
            "channels": af.getnchannels(),
            "bit_depth": width * 8,
        }


def missing_audio_metadata(metadata: dict[str, Any]) -> bool:
    return any(metadata.get(k) is None for k in ("duration_sec", "sample_rate", "channels"))


def read_ffprobe(path: Path, ffprobe: str) -> dict[str, Any]:
    cmd = [
        ffprobe,
        "-v",
        "error",
        "-show_entries",
        "format=duration:stream=sample_rate,channels,bits_per_sample",
        "-of",
        "json",
        str(path),
    ]
    try:
        proc = subprocess.run(cmd, check=True, text=True, capture_output=True, timeout=10)
        data = json.loads(proc.stdout)
    except Exception:
        return {}

    stream = first_audio_stream(data.get("streams", []))
    fmt = data.get("format", {})
    out: dict[str, Any] = {}
    if fmt.get("duration"):
        out["duration_sec"] = round(float(fmt["duration"]), 6)
    if stream:
        if stream.get("sample_rate"):
            out["sample_rate"] = int(stream["sample_rate"])
        if stream.get("channels"):
            out["channels"] = int(stream["channels"])
        if stream.get("bits_per_sample"):
            bits = int(stream["bits_per_sample"])
            if bits > 0:
                out["bit_depth"] = bits
    return out


def first_audio_stream(streams: list[dict[str, Any]]) -> dict[str, Any] | None:
    for stream in streams:
        if stream.get("sample_rate") or stream.get("channels"):
            return stream
    return streams[0] if streams else None


def infer_from_name(name: str, rel_posix: str, duration: float | None) -> dict[str, Any]:
    haystack = f"{rel_posix} {name}"
    tokens = tokenize(haystack)
    bpm = infer_bpm(haystack, tokens)
    key, mode = infer_key(haystack)
    tags = sorted({tok for tok in tokens if tok in TAG_KEYWORDS})
    mood = sorted({tok for tok in tokens if tok in MOOD_KEYWORDS})
    one_shot = infer_one_shot(tags, tokens, duration)
    loop_bars = infer_loop_bars(duration, bpm, one_shot)
    return {
        "bpm": bpm,
        "key": key,
        "mode": mode,
        "tags": tags,
        "mood": mood,
        "one_shot": one_shot,
        "loop_bars": loop_bars,
    }


def tokenize(text: str) -> list[str]:
    return [t for t in re.split(r"[^A-Za-z0-9#]+", text.lower()) if t]


def infer_bpm(text: str, tokens: list[str]) -> float | None:
    bpm_match = re.search(r"(?i)(?<!\d)([5-9]\d|1\d\d|2\d\d|3\d\d)\s*bpm(?!\d)", text)
    if bpm_match:
        return float(bpm_match.group(1))
    for tok in tokens:
        if tok.isdigit():
            value = int(tok)
            if 60 <= value <= 220:
                return float(value)
    return None


def infer_key(text: str) -> tuple[str, str] | tuple[None, None]:
    match = re.search(
        r"(?i)(^|[^A-Za-z0-9])([A-G](?:#|b)?)(?:[\s_-]?(maj|major|min|minor|m))([^A-Za-z0-9]|$)",
        text,
    )
    if not match:
        return None, None
    root = match.group(2).replace("b", "b").replace("#", "#")
    suffix = (match.group(3) or "").lower()
    if suffix in {"min", "minor", "m"}:
        return root[0].upper() + root[1:] + "m", "minor"
    if suffix in {"maj", "major"}:
        return root[0].upper() + root[1:], "major"
    return root[0].upper() + root[1:], ""


def infer_one_shot(tags: list[str], tokens: list[str], duration: float | None) -> bool | None:
    if "loop" in tags or "loop" in tokens or "break" in tags:
        return False
    percussive = {"kick", "bd", "snare", "sd", "clap", "rim", "hat", "hihat", "oh", "ch", "perc", "tom"}
    if any(tag in percussive for tag in tags):
        return True
    if duration is not None:
        if duration <= 1.5:
            return True
        if duration >= 2.5:
            return False
    return None


def infer_loop_bars(duration: float | None, bpm: float | None, one_shot: bool | None) -> float | None:
    if duration is None or bpm is None or one_shot is True:
        return None
    bars = duration * bpm / 240.0
    rounded = round(bars * 2) / 2
    if 0.5 <= rounded <= 64 and abs(bars - rounded) <= 0.08:
        return rounded
    return round(bars, 3)


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    tags: dict[str, int] = {}
    keys: dict[str, int] = {}
    with_bpm = 0
    with_duration = 0
    for sample in samples:
        if sample.get("bpm") is not None:
            with_bpm += 1
        if sample.get("duration_sec") is not None:
            with_duration += 1
        if sample.get("key"):
            keys[sample["key"]] = keys.get(sample["key"], 0) + 1
        for tag in sample.get("tags", []):
            tags[tag] = tags.get(tag, 0) + 1
    return {
        "total": len(samples),
        "with_bpm": with_bpm,
        "with_duration": with_duration,
        "tags": dict(sorted(tags.items(), key=lambda kv: (-kv[1], kv[0]))[:50]),
        "keys": dict(sorted(keys.items(), key=lambda kv: (-kv[1], kv[0]))[:50]),
    }


def sample_id(move_path: str) -> str:
    digest = hashlib.sha1(move_path.encode("utf-8")).hexdigest()
    return digest[:16]


def utc_now() -> str:
    return utc_from_timestamp(time.time())


def utc_from_timestamp(ts: float) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts))


def upload_index(base_url: str, payload: bytes, token: str) -> None:
    url = base_url.rstrip("/") + "/api/mcp/samples/index"
    req = urllib.request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=60) as resp:
        body = resp.read().decode("utf-8", errors="replace")
        if resp.status >= 300:
            raise RuntimeError(body)


if __name__ == "__main__":
    raise SystemExit(main())
