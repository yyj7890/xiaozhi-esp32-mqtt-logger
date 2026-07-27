import os
import unittest

from bridge_entrypoint import tool_environment


class ToolEnvironmentTest(unittest.TestCase):
    def test_default_device_is_forwarded_to_child_tools(self):
        previous = os.environ.get("DEFAULT_DEVICE_CODE")
        try:
            os.environ["DEFAULT_DEVICE_CODE"] = "TEST-DEVICE"
            self.assertEqual(
                {"IOT_API_URL": "http://127.0.0.1:18080", "DEFAULT_DEVICE_CODE": "TEST-DEVICE"},
                tool_environment("http://127.0.0.1:18080"),
            )
        finally:
            if previous is None:
                os.environ.pop("DEFAULT_DEVICE_CODE", None)
            else:
                os.environ["DEFAULT_DEVICE_CODE"] = previous


if __name__ == "__main__":
    unittest.main()
