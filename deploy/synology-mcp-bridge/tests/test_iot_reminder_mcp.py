"""Unit tests for private reminder target selection."""

from __future__ import annotations

import os
import unittest
from unittest.mock import patch

import iot_reminder_mcp as reminders


class ResolveDeviceCodeTests(unittest.TestCase):
    def test_configured_default_wins_over_supplied_value(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_DEVICE_CODE": "new-device"}, clear=False):
            self.assertEqual(reminders.resolve_device_code("old-device"), "new-device")

    def test_supplied_value_is_used_without_configured_default(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_DEVICE_CODE": ""}, clear=False):
            self.assertEqual(reminders.resolve_device_code("caller-device"), "caller-device")

    def test_missing_default_and_supplied_value_is_safe_error(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_DEVICE_CODE": ""}, clear=False):
            with self.assertRaisesRegex(ValueError, "No reminder target is configured"):
                reminders.resolve_device_code("")

    def test_target_summary_does_not_expose_device_code(self) -> None:
        raw_device_code = "private-device-code"
        summary = reminders.target_summary(raw_device_code)
        self.assertNotIn(raw_device_code, summary)
        self.assertTrue(summary.startswith("reminder-target:"))


if __name__ == "__main__":
    unittest.main()
