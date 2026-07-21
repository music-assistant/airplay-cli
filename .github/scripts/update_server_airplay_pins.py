#!/usr/bin/env python3
"""Atomically update the airplay-cli release pins in the server Dockerfile."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


VERSION_PREFIX = "ARG CLIAIRPLAY_VERSION="
CHECKSUM_PREFIX = "ARG CLIAIRPLAY_CHECKSUMS_SHA256="
VERSION_DECLARATION = re.compile(
    r"^\s*(?i:ARG)\s+CLIAIRPLAY_VERSION(?=$|[\s=])"
)
CHECKSUM_DECLARATION = re.compile(
    r"^\s*(?i:ARG)\s+CLIAIRPLAY_CHECKSUMS_SHA256(?=$|[\s=])"
)
VERSION_LINE = re.compile(rf"{VERSION_PREFIX}(?P<value>\S+)")
CHECKSUM_LINE = re.compile(rf"{CHECKSUM_PREFIX}(?P<value>[0-9a-f]{{64}})")
RELEASE_TAG = re.compile(r"v[0-9A-Za-z][0-9A-Za-z._+-]*")
SHA256 = re.compile(r"[0-9a-f]{64}")


def _find_unique_line(
    lines: list[str],
    declaration: re.Pattern[str],
    exact: re.Pattern[str],
    label: str,
    path: Path,
) -> tuple[int, str]:
    declarations = [
        (index, line.rstrip("\r\n"))
        for index, line in enumerate(lines)
        if declaration.match(line.rstrip("\r\n"))
    ]
    if len(declarations) != 1:
        raise ValueError(
            f"{path}: expected exactly one {label} declaration, "
            f"found {len(declarations)}"
        )
    index, content = declarations[0]
    if not (match := exact.fullmatch(content)):
        raise ValueError(f"{path}: {label} declaration has an unexpected format")
    return index, match.group("value")


def _replace_value(line: str, prefix: str, value: str) -> str:
    content = line.rstrip("\r\n")
    return f"{prefix}{value}{line[len(content):]}"


def _atomic_write(path: Path, content: str) -> None:
    mode = stat.S_IMODE(path.stat().st_mode)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}."
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as temporary:
            os.fchmod(temporary.fileno(), mode)
            temporary.write(content)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def update_pins(
    dockerfile: Path, version: str, checksums_sha256: str
) -> tuple[bool, str, str]:
    """Validate and update both pins, returning changed and previous values."""
    if not RELEASE_TAG.fullmatch(version):
        raise ValueError(f"invalid release tag: {version!r}")
    if not SHA256.fullmatch(checksums_sha256):
        raise ValueError("SHA256SUMS digest must be 64 lowercase hexadecimal characters")

    with dockerfile.open("r", encoding="utf-8", newline="") as source:
        original = source.read()
    lines = original.splitlines(keepends=True)
    version_index, old_version = _find_unique_line(
        lines,
        VERSION_DECLARATION,
        VERSION_LINE,
        "CLIAIRPLAY_VERSION",
        dockerfile,
    )
    checksum_index, old_checksum = _find_unique_line(
        lines,
        CHECKSUM_DECLARATION,
        CHECKSUM_LINE,
        "CLIAIRPLAY_CHECKSUMS_SHA256",
        dockerfile,
    )
    if not RELEASE_TAG.fullmatch(old_version):
        raise ValueError(f"{dockerfile}: invalid existing release tag: {old_version!r}")

    updated_lines = lines.copy()
    updated_lines[version_index] = _replace_value(
        lines[version_index], VERSION_PREFIX, version
    )
    updated_lines[checksum_index] = _replace_value(
        lines[checksum_index], CHECKSUM_PREFIX, checksums_sha256
    )
    updated = "".join(updated_lines)
    if updated == original:
        return False, old_version, old_checksum

    _atomic_write(dockerfile, updated)
    return True, old_version, old_checksum


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dockerfile", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--checksums-sha256", required=True)
    args = parser.parse_args(argv)

    try:
        changed, old_version, _ = update_pins(
            args.dockerfile, args.version, args.checksums_sha256
        )
    except (OSError, ValueError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 1

    if changed:
        print(f"Updated {args.dockerfile} release pins for {args.version}")
    else:
        print(f"{args.dockerfile} already pins {args.version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
