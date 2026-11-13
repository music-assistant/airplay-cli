"""Decode the native client's actual session SETUP bodies with plistlib."""

import os
import plistlib
import subprocess
import sys
import unittest


CLIENT = sys.argv.pop(1)
RELAY_KEYS = ("senderSupportsRelay", "isMultiSelectAirPlay")


class SessionSetupTest(unittest.TestCase):
    def test_control_relay_fields(self):
        cases = (
            ("PTP", "AudioAccessory5,1", {}, True),
            ("NTP", "AudioAccessory5,1", {}, False),
            ("PTP", "AudioAccessory5,1", {"CLIAIRPLAY_CONTROL_RELAY": "0"}, False),
            ("PTP", "AudioAccessory5,1", {"CLIAIRPLAY_MRP": "0"}, False),
            ("PTP", "AppleTV11,1", {}, False),
        )
        for protocol, model, overrides, enabled in cases:
            with self.subTest(protocol=protocol, model=model, overrides=overrides):
                env = os.environ.copy()
                env.pop("CLIAIRPLAY_CONTROL_RELAY", None)
                env.pop("CLIAIRPLAY_MRP", None)
                env.update(overrides)
                body = subprocess.check_output(
                    [CLIENT, "--session-setup", protocol, model], env=env
                )
                setup = plistlib.loads(body)
                self.assertEqual(setup["timingProtocol"], protocol)
                self.assertEqual(setup["sessionUUID"], "test-session")
                for key in RELAY_KEYS:
                    if enabled:
                        self.assertIs(setup[key], True)
                    else:
                        self.assertNotIn(key, setup)


if __name__ == "__main__":
    unittest.main()
