from __future__ import annotations

import os
from pathlib import Path
import stat
import tempfile
import unittest

from update_server_airplay_pins import update_pins


OLD_CHECKSUM = "1" * 64
NEW_CHECKSUM = "a" * 64


class UpdateServerAirplayPinsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.dockerfile = Path(self.temporary_directory.name) / "Dockerfile"

    def write_dockerfile(self, content: str) -> None:
        self.dockerfile.write_text(content, encoding="utf-8")

    def test_updates_both_pins_and_preserves_mode(self) -> None:
        self.write_dockerfile(
            "FROM alpine\n"
            "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
            "RUN echo done\n"
        )
        os.chmod(self.dockerfile, 0o640)

        changed, old_version, old_checksum = update_pins(
            self.dockerfile, "v0.2.0", NEW_CHECKSUM
        )

        self.assertTrue(changed)
        self.assertEqual(old_version, "v0.1.0")
        self.assertEqual(old_checksum, OLD_CHECKSUM)
        self.assertEqual(
            self.dockerfile.read_text(encoding="utf-8"),
            "FROM alpine\n"
            "ARG CLIAIRPLAY_VERSION=v0.2.0\n"
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={NEW_CHECKSUM}\n"
            "RUN echo done\n",
        )
        self.assertEqual(stat.S_IMODE(self.dockerfile.stat().st_mode), 0o640)

    def test_is_idempotent(self) -> None:
        content = (
            "ARG CLIAIRPLAY_VERSION=v0.2.0\n"
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={NEW_CHECKSUM}\n"
        )
        self.write_dockerfile(content)

        changed, _, _ = update_pins(self.dockerfile, "v0.2.0", NEW_CHECKSUM)

        self.assertFalse(changed)
        self.assertEqual(self.dockerfile.read_text(encoding="utf-8"), content)

    def test_unexpected_shapes_do_not_partially_update(self) -> None:
        fixtures = {
            "missing checksum": "ARG CLIAIRPLAY_VERSION=v0.1.0\n",
            "duplicate version": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                "ARG CLIAIRPLAY_VERSION=v0.1.1\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
            ),
            "mixed valid and malformed checksum declarations": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
                "ARG CLIAIRPLAY_CHECKSUMS_SHA256=not-a-digest\n"
            ),
            "indented duplicate version declaration": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                "  ARG CLIAIRPLAY_VERSION=v0.1.1\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
            ),
            "lowercase duplicate version instruction": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                "arg CLIAIRPLAY_VERSION=v0.1.1\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
            ),
            "lowercase duplicate checksum instruction": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
                f"arg CLIAIRPLAY_CHECKSUMS_SHA256={NEW_CHECKSUM}\n"
            ),
            "malformed checksum": (
                "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
                "ARG CLIAIRPLAY_CHECKSUMS_SHA256=not-a-digest\n"
            ),
            "malformed version": (
                "ARG CLIAIRPLAY_VERSION=latest\n"
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
            ),
        }
        for name, content in fixtures.items():
            with self.subTest(name=name):
                self.write_dockerfile(content)
                with self.assertRaises(ValueError):
                    update_pins(self.dockerfile, "v0.2.0", NEW_CHECKSUM)
                self.assertEqual(
                    self.dockerfile.read_text(encoding="utf-8"), content
                )

    def test_invalid_inputs_do_not_modify_file(self) -> None:
        content = (
            "ARG CLIAIRPLAY_VERSION=v0.1.0\n"
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_CHECKSUM}\n"
        )
        self.write_dockerfile(content)

        for version, checksum in (
            ("0.2.0", NEW_CHECKSUM),
            ("v0.2.0", "A" * 64),
            ("v0.2.0", "a" * 63),
        ):
            with self.subTest(version=version, checksum=checksum):
                with self.assertRaises(ValueError):
                    update_pins(self.dockerfile, version, checksum)
                self.assertEqual(
                    self.dockerfile.read_text(encoding="utf-8"), content
                )


if __name__ == "__main__":
    unittest.main()
