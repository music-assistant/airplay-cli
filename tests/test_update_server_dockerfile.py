from __future__ import annotations

import os
from pathlib import Path
import stat
import tempfile
import unittest

from scripts.update_server_dockerfile import PinUpdateError, update_dockerfile

OLD_TAG = "v0.1.0"
NEW_TAG = "v1.2.3"
OLD_DIGEST = "1" * 64
NEW_DIGEST = "2" * 64
DOCKERFILE = (
    "# syntax=docker/dockerfile:1\n"
    "\n"
    "FROM scratch\n"
    f"ARG CLIAIRPLAY_VERSION={OLD_TAG}\n"
    f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_DIGEST}\n"
    "RUN echo ready\n"
)


class UpdateServerDockerfileTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.dockerfile = Path(self.temporary_directory.name) / "Dockerfile"
        self.dockerfile.write_text(DOCKERFILE, encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_updates_both_pins(self) -> None:
        self.assertTrue(
            update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)
        )

        updated = self.dockerfile.read_text(encoding="utf-8")
        self.assertIn(f"ARG CLIAIRPLAY_VERSION={NEW_TAG}\n", updated)
        self.assertIn(
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={NEW_DIGEST}\n", updated
        )
        self.assertNotIn(OLD_TAG, updated)
        self.assertNotIn(OLD_DIGEST, updated)

    def test_idempotent_update_does_not_rewrite_file(self) -> None:
        self.assertTrue(
            update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)
        )
        before = self.dockerfile.stat()

        self.assertFalse(
            update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)
        )

        after = self.dockerfile.stat()
        self.assertEqual(before.st_ino, after.st_ino)
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)

    def test_rejects_malformed_pins_without_partial_update(self) -> None:
        malformed_lines = (
            (
                f"ARG CLIAIRPLAY_VERSION={OLD_TAG}",
                f"ARG CLIAIRPLAY_VERSION = {OLD_TAG}",
            ),
            (
                f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_DIGEST}",
                "ARG CLIAIRPLAY_CHECKSUMS_SHA256=not-a-digest",
            ),
        )
        for valid_line, malformed_line in malformed_lines:
            with self.subTest(pin=valid_line):
                contents = DOCKERFILE.replace(valid_line, malformed_line)
                self.dockerfile.write_text(contents, encoding="utf-8")
                before = self.dockerfile.read_bytes()

                with self.assertRaisesRegex(PinUpdateError, "malformed"):
                    update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)

                self.assertEqual(before, self.dockerfile.read_bytes())

    def test_rejects_duplicate_pins(self) -> None:
        for line in (
            f"ARG CLIAIRPLAY_VERSION={OLD_TAG}\n",
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_DIGEST}\n",
        ):
            with self.subTest(pin=line):
                contents = DOCKERFILE.replace(line, line + line)
                self.dockerfile.write_text(contents, encoding="utf-8")
                before = self.dockerfile.read_bytes()

                with self.assertRaisesRegex(PinUpdateError, "duplicate"):
                    update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)

                self.assertEqual(before, self.dockerfile.read_bytes())

    def test_rejects_missing_pins(self) -> None:
        for line in (
            f"ARG CLIAIRPLAY_VERSION={OLD_TAG}\n",
            f"ARG CLIAIRPLAY_CHECKSUMS_SHA256={OLD_DIGEST}\n",
        ):
            with self.subTest(pin=line):
                contents = DOCKERFILE.replace(line, "")
                self.dockerfile.write_text(contents, encoding="utf-8")
                before = self.dockerfile.read_bytes()

                with self.assertRaisesRegex(PinUpdateError, "missing"):
                    update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)

                self.assertEqual(before, self.dockerfile.read_bytes())

    def test_rejects_invalid_release_tags(self) -> None:
        for tag in ("1.2.3", "v1.2", "v01.2.3", "v1.2.3/other"):
            with self.subTest(tag=tag):
                before = self.dockerfile.read_bytes()

                with self.assertRaisesRegex(
                    PinUpdateError, "invalid release tag"
                ):
                    update_dockerfile(self.dockerfile, tag, NEW_DIGEST)

                self.assertEqual(before, self.dockerfile.read_bytes())

    def test_rejects_invalid_digests(self) -> None:
        for digest in ("2" * 63, "g" * 64, "A" * 64):
            with self.subTest(digest=digest):
                before = self.dockerfile.read_bytes()

                with self.assertRaisesRegex(
                    PinUpdateError, "invalid SHA256SUMS digest"
                ):
                    update_dockerfile(self.dockerfile, NEW_TAG, digest)

                self.assertEqual(before, self.dockerfile.read_bytes())

    def test_preserves_file_mode(self) -> None:
        os.chmod(self.dockerfile, 0o640)

        update_dockerfile(self.dockerfile, NEW_TAG, NEW_DIGEST)

        mode = stat.S_IMODE(self.dockerfile.stat().st_mode)
        self.assertEqual(0o640, mode)


if __name__ == "__main__":
    unittest.main()
