"""Tests for safe filtering of the old non-verifying climate tool."""

from __future__ import annotations

import os
import unittest
from unittest.mock import patch

import audited_mcp_proxy as proxy


class ToolFilterTests(unittest.TestCase):
    def test_hides_only_generic_climate_temperature_tool(self) -> None:
        message: dict[str, object] = {"result": {"tools": [
            {"name": "HassClimateSetTemperature"},
            {"name": "HassTurnOn"},
        ]}}
        with patch.dict(os.environ, {"HIDE_GENERIC_CLIMATE_TOOLS": "1"}, clear=False):
            proxy.hide_generic_climate_tools(message)
        names = [tool["name"] for tool in message["result"]["tools"]]  # type: ignore[index]
        self.assertEqual(names, ["HassTurnOn"])


if __name__ == "__main__":
    unittest.main()
