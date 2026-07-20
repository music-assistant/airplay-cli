#!/usr/bin/env python3
"""Generate and send controlled Apple TV MediaRemote artwork probes."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import stat
import sys
from typing import Any


SIZE_TARGETS = (44032, 61440, 65535, 65536, 66560, 102400, 153600)
PROFILE_TARGET = 65536
SOF_MARKERS = {
    0xC0: "baseline",
    0xC1: "extended-sequential",
    0xC2: "progressive",
}


def inspect_jpeg(data: bytes) -> dict[str, Any]:
    if len(data) < 6 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("not a complete JPEG")

    result: dict[str, Any] = {
        "bytes": len(data),
        "comment_segment_bytes": 0,
    }
    pos = 2
    while pos + 1 < len(data):
        if data[pos] != 0xFF:
            raise ValueError(f"expected marker at offset {pos}")
        pos += 1
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            break
        marker = data[pos]
        pos += 1
        if marker == 0xD9:
            break
        if marker == 0x01 or 0xD0 <= marker <= 0xD7:
            continue
        if marker in (0x00, 0xD8) or pos + 2 > len(data):
            raise ValueError(f"invalid marker 0x{marker:02x}")
        segment_len = int.from_bytes(data[pos : pos + 2], "big")
        if segment_len < 2 or pos + segment_len > len(data):
            raise ValueError(f"invalid segment length at offset {pos}")

        if marker in SOF_MARKERS:
            if segment_len < 8:
                raise ValueError("short SOF segment")
            sof = data[pos + 2 : pos + segment_len]
            components = sof[5]
            if segment_len != 8 + 3 * components:
                raise ValueError("invalid SOF component table")
            result.update(
                {
                    "precision": sof[0],
                    "height": int.from_bytes(sof[1:3], "big"),
                    "width": int.from_bytes(sof[3:5], "big"),
                    "components": components,
                    "sof_marker": f"0x{marker:02x}",
                    "profile": SOF_MARKERS[marker],
                    "progressive": marker == 0xC2,
                }
            )
        elif marker == 0xFE:
            result["comment_segment_bytes"] += segment_len + 2
        elif marker == 0xDA:
            break
        pos += segment_len

    if "sof_marker" not in result:
        raise ValueError("JPEG has no supported SOF marker")
    return result


def pad_jpeg(data: bytes, target_size: int) -> bytes:
    if target_size == len(data):
        return data
    remaining = target_size - len(data)
    if remaining < 4:
        raise ValueError(
            f"target {target_size} must exceed base JPEG by at least 4 bytes"
        )

    segments = bytearray()
    while remaining:
        if remaining < 4:
            raise ValueError("cannot represent trailing JPEG padding under 4 bytes")
        total = min(remaining, 65537)
        tail = remaining - total
        if 0 < tail < 4:
            total -= 4 - tail
        segment_len = total - 2
        segments.extend(b"\xff\xfe")
        segments.extend(segment_len.to_bytes(2, "big"))
        segments.extend(b"M" * (total - 4))
        remaining -= total

    padded = data[:2] + bytes(segments) + data[2:]
    if len(padded) != target_size:
        raise AssertionError("JPEG padding produced the wrong size")
    inspect_jpeg(padded)
    return padded


def source_image(image_module: Any) -> Any:
    image = image_module.new("RGB", (600, 600))
    pixels = image.load()
    for y in range(600):
        for x in range(600):
            pixels[x, y] = (
                x * 255 // 599,
                y * 255 // 599,
                ((x // 12) * 17 + (y // 12) * 29) % 256,
            )
    return image


def encode_jpeg(image: Any, quality: int, progressive: bool) -> bytes:
    output = io.BytesIO()
    options = {
        "quality": quality,
        "optimize": False,
        "progressive": progressive,
    }
    if image.mode == "RGB":
        options["subsampling"] = 2
    image.save(output, format="JPEG", **options)
    return output.getvalue()


def build_bases() -> tuple[dict[str, bytes], int]:
    try:
        from PIL import Image
    except ImportError as err:
        raise SystemExit(
            "Pillow is required only to generate the matrix: "
            "python3 -m pip install Pillow"
        ) from err

    rgb = source_image(Image)
    grayscale = rgb.convert("L")
    for quality in range(90, 34, -5):
        bases = {
            "rgb-baseline": encode_jpeg(rgb, quality, False),
            "rgb-progressive": encode_jpeg(rgb, quality, True),
            "grayscale-baseline": encode_jpeg(grayscale, quality, False),
        }
        if max(map(len, bases.values())) <= min(SIZE_TARGETS) - 4:
            return bases, quality
    raise SystemExit("could not encode base JPEGs below the smallest matrix target")


def write_case(
    output_dir: Path,
    name: str,
    base: bytes,
    target_size: int,
    quality: int,
) -> dict[str, Any]:
    data = pad_jpeg(base, target_size)
    from PIL import Image

    with Image.open(io.BytesIO(data)) as decoded:
        decoded.verify()
    path = output_dir / f"{name}-{target_size}.jpg"
    path.write_bytes(data)
    details = inspect_jpeg(data)
    details.update(
        {
            "name": path.stem,
            "path": str(path.resolve()),
            "sha256": hashlib.sha256(data).hexdigest(),
            "quality": quality,
            "pillow_verified": True,
        }
    )
    return details


def print_cases(cases: list[dict[str, Any]]) -> None:
    print(
        f"{'name':34} {'bytes':>8} {'size':>9} {'SOF':>5} "
        f"{'comp':>4} {'progressive':>11}"
    )
    for case in cases:
        dimensions = f"{case['width']}x{case['height']}"
        print(
            f"{case['name']:34} {case['bytes']:8} {dimensions:>9} "
            f"{case['sof_marker']:>5} {case['components']:4} "
            f"{str(case['progressive']):>11}"
        )


def generate(args: argparse.Namespace) -> int:
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    bases, quality = build_bases()

    cases = [
        write_case(
            output_dir,
            "rgb-baseline",
            bases["rgb-baseline"],
            target,
            quality,
        )
        for target in SIZE_TARGETS
    ]
    cases.extend(
        [
            write_case(
                output_dir,
                "rgb-progressive",
                bases["rgb-progressive"],
                PROFILE_TARGET,
                quality,
            ),
            write_case(
                output_dir,
                "grayscale-baseline",
                bases["grayscale-baseline"],
                PROFILE_TARGET,
                quality,
            ),
        ]
    )
    manifest = output_dir / "manifest.json"
    manifest.write_text(json.dumps(cases, indent=2) + "\n", encoding="utf-8")

    print_cases(cases)
    print(f"\nManifest: {manifest}")
    print("\nSend one case to a running cliairplay command pipe:")
    script = shlex.quote(str(Path(__file__).resolve()))
    python = shlex.quote(sys.executable)
    sample = shlex.quote(cases[0]["path"])
    print(
        f"{python} {script} send --cmdpipe /path/to/cliairplay.fifo "
        f"--artwork {sample}"
    )
    return 0


def inspect_files(args: argparse.Namespace) -> int:
    cases = []
    for artwork in args.artwork:
        path = artwork.resolve()
        details = inspect_jpeg(path.read_bytes())
        details.update({"name": path.stem, "path": str(path)})
        cases.append(details)
    print_cases(cases)
    return 0


def send(args: argparse.Namespace) -> int:
    artwork = args.artwork.resolve()
    details = inspect_jpeg(artwork.read_bytes())
    title = args.title or f"MRP matrix {artwork.stem}"
    commands = (
        f"TITLE={title}\n"
        "ARTIST=Music Assistant\n"
        "ALBUM=Apple TV artwork validation\n"
        f"ARTWORK={artwork}\n"
        "ACTION=SENDMETA\n"
    )

    print(json.dumps({"artwork": str(artwork), **details}, indent=2))
    print("\nCommands:")
    print(commands, end="")
    if args.dry_run:
        return 0

    cmdpipe = args.cmdpipe.resolve()
    mode = os.stat(cmdpipe).st_mode
    if not stat.S_ISFIFO(mode):
        raise SystemExit(f"command pipe is not a FIFO: {cmdpipe}")
    with cmdpipe.open("w", encoding="utf-8") as pipe:
        pipe.write(commands)
        pipe.flush()
    print(
        "\nObserve `[STATUS] mrp artwork=posted status=... bytes=...` "
        "and record whether Apple TV renders the cover."
    )
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    subparsers = root.add_subparsers(dest="command", required=True)

    generate_parser = subparsers.add_parser(
        "generate", help="generate the exact-size JPEG matrix"
    )
    generate_parser.add_argument(
        "--output",
        type=Path,
        default=Path("/tmp/cliairplay-mrp-matrix"),
    )
    generate_parser.set_defaults(func=generate)

    inspect_parser = subparsers.add_parser(
        "inspect", help="print JPEG dimensions and profile markers"
    )
    inspect_parser.add_argument("artwork", type=Path, nargs="+")
    inspect_parser.set_defaults(func=inspect_files)

    send_parser = subparsers.add_parser(
        "send", help="send one generated case to a cliairplay command FIFO"
    )
    send_parser.add_argument("--cmdpipe", type=Path, required=True)
    send_parser.add_argument("--artwork", type=Path, required=True)
    send_parser.add_argument("--title")
    send_parser.add_argument("--dry-run", action="store_true")
    send_parser.set_defaults(func=send)
    return root


def main() -> int:
    args = parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
