"""Unit tests for verified climate target selection and safety checks."""

from __future__ import annotations

import os
import unittest
from unittest.mock import patch

import ha_climate_mcp as climate


class ClimateTargetTests(unittest.TestCase):
    def test_default_entity_wins_over_supplied_value(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_CLIMATE_ENTITY_ID": "climate.bedroom"}, clear=False):
            self.assertEqual(climate.resolve_entity_id("climate.old"), "climate.bedroom")

    def test_single_climate_entity_is_discovered(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_CLIMATE_ENTITY_ID": ""}, clear=False):
            with patch.object(climate, "request", return_value=[{"entity_id": "climate.midea_ac"}]):
                self.assertEqual(climate.resolve_entity_id(), "climate.midea_ac")

    def test_multiple_entities_need_private_default(self) -> None:
        with patch.dict(os.environ, {"DEFAULT_CLIMATE_ENTITY_ID": ""}, clear=False):
            with patch.object(climate, "request", return_value=[
                {"entity_id": "climate.one"}, {"entity_id": "climate.two"},
            ]):
                with self.assertRaisesRegex(ValueError, "DEFAULT_CLIMATE_ENTITY_ID"):
                    climate.resolve_entity_id()

    def test_rejects_unsupported_fan_mode(self) -> None:
        sample = {"attributes": {"fan_modes": ["auto", "high"]}}
        with self.assertRaisesRegex(ValueError, "Unsupported fan_modes"):
            climate.require_supported_value(sample, "fan_modes", "turbo")


if __name__ == "__main__":
    unittest.main()
