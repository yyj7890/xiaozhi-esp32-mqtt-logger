import unittest
from datetime import datetime

from iot_reminder_mcp import normalized_time


class ReminderTimeExpressionTest(unittest.TestCase):
    NOW = datetime(2026, 7, 28, 19, 0, 0)

    def test_relative_minutes(self):
        self.assertEqual("2026-07-28T19:02:00", normalized_time("两分钟后", self.NOW))

    def test_relative_hours_and_days(self):
        self.assertEqual("2026-07-28T20:00:00", normalized_time("1小时后", self.NOW))
        self.assertEqual("2026-07-30T19:00:00", normalized_time("两天后", self.NOW))

    def test_named_clock_time(self):
        self.assertEqual("2026-07-29T08:00:00", normalized_time("明天早上八点", self.NOW))
        self.assertEqual("2026-07-28T20:30:00", normalized_time("今晚八点半", self.NOW))

    def test_iso_time_remains_supported(self):
        self.assertEqual("2026-07-28T21:30:00", normalized_time("2026-07-28T21:30:00", self.NOW))

    def test_ambiguous_time_is_rejected(self):
        with self.assertRaises(ValueError):
            normalized_time("过一会儿", self.NOW)


if __name__ == "__main__":
    unittest.main()
