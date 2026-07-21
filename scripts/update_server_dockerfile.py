#!/usr/bin/env python3
"""Atomically update the airplay-cli pins in the server Dockerfile."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

VERSION_PIN = "CLIAIRPLAY_VERSION"
CHECKSUM_PIN = "CLIAIRPLAY_CHECKSUMS_SHA256"
PIN_NAMES = (VERSION_PIN, CHECKSUM_PIN)

_SEMVER_NUMBER = r"(?:0|[1-9][0-9]*)"
_SEMVER_IDENTIFIER = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
TAG_PATTERN = re.compile(
    rf"^v{_SEMVER_NUMBER}\.{_SEMVER_NUMBER}\.{_SEMVER_NUMBER}"
    rf"(?:-{_SEMVER_IDENTIFIER}(?:\.{_SEMVER_IDENTIFIER})*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)
DIGEST_PATTERN = re.compile(r"^[0-9a-f]{64}$")
TARGET_LINE_PATTERN = re.compile(
    rf"^\s*ARG\s+(?P<name>{VERSION_PIN}|{CHECKSUM_PIN})\b.*$"
)
PIN_LINE_PATTERNS = {
    VERSION_PIN: re.compile(rf"^ARG {VERSION_PIN}=(?P<value>\S+)$"),
    CHECKSUM_PIN: re.compile(rf"^ARG {CHECKSUM_PIN}=(?P<value>\S+)$"),
}


class PinUpdateError(ValueError):
    """Raised when release inputs or Dockerfile pins violate the contract."""


def _validate_release_inputs(tag: str, checksums_sha256: str) -> None:
    if TAG_PATTERN.fullmatch(tag) is None:
        raise PinUpdateError(f"invalid release tag: {tag!r}")
    if DIGEST_PATTERN.fullmatch(checksums_sha256) is None:
        raise PinUpdateError(
            "invalid SHA256SUMS digest: expected 64 lowercase hexadecimal characters"
        )


def _updated_contents(
    original: bytes, tag: str, checksums_sha256: str
) -> bytes:
    try:
        text = original.decode("utf-8")
    except UnicodeDecodeError as err:
        raise PinUpdateError("Dockerfile must be valid UTF-8") from err

    lines = text.splitlines(keepends=True)
    occurrences: dict[str, list[tuple[int, str, str]]] = {
        name: [] for name in PIN_NAMES
    }

    for index, line in enumerate(lines):
        content = line.rstrip("\r\n")
        line_ending = line[len(content) :]
        match = TARGET_LINE_PATTERN.fullmatch(content)
        if match is not None:
            occurrences[match.group("name")].append(
                (index, content, line_ending)
            )

    for name in PIN_NAMES:
        count = len(occurrences[name])
        if count == 0:
            raise PinUpdateError(f"missing Dockerfile pin: ARG {name}=...")
        if count != 1:
            raise PinUpdateError(
                f"duplicate Dockerfile pin: ARG {name}=... appears {count} times"
            )

    replacements = {
        VERSION_PIN: tag,
        CHECKSUM_PIN: checksums_sha256,
    }
    validators = {
        VERSION_PIN: TAG_PATTERN,
        CHECKSUM_PIN: DIGEST_PATTERN,
    }

    for name in PIN_NAMES:
        index, content, line_ending = occurrences[name][0]
        match = PIN_LINE_PATTERNS[name].fullmatch(content)
        if match is None or validators[name].fullmatch(match.group("value")) is None:
            raise PinUpdateError(
                f"malformed Dockerfile pin: expected exactly ARG {name}=<valid-value>"
            )
        lines[index] = f"ARG {name}={replacements[name]}{line_ending}"

    return "".join(lines).encode("utf-8")


def update_dockerfile(
    dockerfile: Path, tag: str, checksums_sha256: str
) -> bool:
    """Update both pins in one atomic replacement, returning whether it changed."""

    _validate_release_inputs(tag, checksums_sha256)

    source_stat = dockerfile.lstat()
    if stat.S_ISLNK(source_stat.st_mode) or not stat.S_ISREG(source_stat.st_mode):
        raise PinUpdateError(f"Dockerfile path is not a regular file: {dockerfile}")

    original = dockerfile.read_bytes()
    updated = _updated_contents(original, tag, checksums_sha256)
    if updated == original:
        return False

    fd, temporary_name = tempfile.mkstemp(
        dir=dockerfile.parent,
        prefix=f".{dockerfile.name}.",
        suffix=".tmp",
    )
    replaced = False
    try:
        with os.fdopen(fd, "wb") as temporary:
            temporary.write(updated)
            os.fchmod(temporary.fileno(), stat.S_IMODE(source_stat.st_mode))
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, dockerfile)
        replaced = True
        # Persist the renamed directory entry as well as the file contents.
        directory_fd = os.open(dockerfile.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if not replaced:
            Path(temporary_name).unlink(missing_ok=True)

    return True


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Update airplay-cli release pins in a server Dockerfile."
    )
    parser.add_argument("dockerfile", type=Path)
    parser.add_argument("tag")
    parser.add_argument("checksums_sha256")
    args = parser.parse_args(argv)

    try:
        changed = update_dockerfile(
            args.dockerfile, args.tag, args.checksums_sha256
        )
    except (OSError, PinUpdateError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 1

    status = "updated" if changed else "already up to date"
    print(f"{args.dockerfile}: {status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
