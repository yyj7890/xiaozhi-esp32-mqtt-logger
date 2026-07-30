"""Verified, least-privilege Home Assistant climate controls for XiaoZhi."""

from __future__ import annotations

import hashlib
import json
import os
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

from fastmcp import FastMCP


mcp = FastMCP("Verified Bedroom Climate")


def required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(f"{name} is not configured")
    return value


def api_url(path: str) -> str:
    return required_env("HA_API_URL").rstrip("/") + path


def request(method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    request_headers = {
        "Authorization": f"Bearer {required_env('HA_TOKEN')}",
        "Content-Type": "application/json",
    }
    req = Request(api_url(path), data=body, method=method, headers=request_headers)
    try:
        with urlopen(req, timeout=10) as response:
            raw = response.read().decode("utf-8")
    except HTTPError as exc:
        raise RuntimeError(f"Home Assistant rejected the climate command ({exc.code})") from exc
    except URLError as exc:
        raise RuntimeError("Home Assistant is unavailable") from exc
    return json.loads(raw) if raw else None


def state(entity_id: str) -> dict[str, Any]:
    result = request("GET", "/states/" + quote(entity_id, safe="._"))
    if not isinstance(result, dict):
        raise RuntimeError("Home Assistant returned an invalid climate state")
    return result


def resolve_entity_id(provided_entity_id: str = "") -> str:
    """Use the private default, explicit caller value, or one discovered climate."""
    configured_default = os.environ.get("DEFAULT_CLIMATE_ENTITY_ID", "").strip()
    candidate = configured_default or provided_entity_id.strip()
    if candidate:
        if not candidate.startswith("climate."):
            raise ValueError("The configured climate target is invalid")
        return candidate

    all_states = request("GET", "/states")
    climate_entities = [
        item.get("entity_id", "") for item in all_states if isinstance(item, dict)
        and isinstance(item.get("entity_id"), str) and item["entity_id"].startswith("climate.")
    ] if isinstance(all_states, list) else []
    if len(climate_entities) == 1:
        return climate_entities[0]
    raise ValueError(
        "No single climate target can be selected. Configure DEFAULT_CLIMATE_ENTITY_ID "
        "in the private bridge.env."
    )


def target_summary(entity_id: str) -> str:
    """Use a one-way fingerprint so audit logs never reveal entity identifiers."""
    return "climate:" + hashlib.sha256(entity_id.encode("utf-8")).hexdigest()[:12]


def audit(tool_name: str, entity_id: str, status: str, result_summary: str) -> None:
    base = os.environ.get("IOT_API_URL", "").strip().rstrip("/")
    if not base:
        return
    payload = json.dumps({
        "toolName": tool_name,
        "targetSummary": target_summary(entity_id),
        "status": status,
        "resultSummary": result_summary,
    }, ensure_ascii=False).encode("utf-8")
    try:
        req = Request(base + "/api/mcp-tool-executions", data=payload, method="POST",
                      headers={"Content-Type": "application/json"})
        with urlopen(req, timeout=5):
            pass
    except (HTTPError, URLError, OSError):
        pass


def attribute_values(climate_state: dict[str, Any], name: str) -> list[str]:
    value = climate_state.get("attributes", {}).get(name, [])
    return [item for item in value if isinstance(item, str)] if isinstance(value, list) else []


def require_supported_value(climate_state: dict[str, Any], attribute: str, value: str) -> None:
    allowed = attribute_values(climate_state, attribute)
    if not allowed:
        raise ValueError(f"This air conditioner does not expose {attribute} control to Home Assistant")
    if value not in allowed:
        raise ValueError(f"Unsupported {attribute} value. Use BedroomClimateGetStatus to see available values")


def action_and_verify(entity_id: str, service: str, data: dict[str, Any], attribute: str, expected: Any) -> dict[str, Any]:
    try:
        request("POST", "/services/climate/" + service, {"entity_id": entity_id, **data})
        for _ in range(6):
            refreshed = state(entity_id)
            if refreshed.get("attributes", {}).get(attribute) == expected:
                audit(service, entity_id, "SUCCEEDED", "Climate state verified")
                return {"success": True, "verified": True}
            time.sleep(2)
    except (RuntimeError, ValueError):
        audit(service, entity_id, "FAILED", "Climate command failed")
        raise
    audit(service, entity_id, "FAILED", "Climate state did not confirm requested value")
    raise RuntimeError("The air conditioner did not confirm the requested setting")


@mcp.tool()
def BedroomClimateGetStatus(entity_id: str = "") -> dict[str, Any]:
    """Read the bedroom air conditioner's target temperature and supported controls."""
    resolved = resolve_entity_id(entity_id)
    try:
        current = state(resolved)
        attributes = current.get("attributes", {})
        result = {
            "success": True,
            "hvac_mode": current.get("state"),
            "current_temperature": attributes.get("current_temperature"),
            "target_temperature": attributes.get("temperature"),
            "fan_mode": attributes.get("fan_mode"),
            "swing_mode": attributes.get("swing_mode"),
            "swing_horizontal_mode": attributes.get("swing_horizontal_mode"),
            "fan_modes": attribute_values(current, "fan_modes"),
            "swing_modes": attribute_values(current, "swing_modes"),
            "swing_horizontal_modes": attribute_values(current, "swing_horizontal_modes"),
        }
    except (RuntimeError, ValueError):
        audit("BedroomClimateGetStatus", resolved, "FAILED", "Climate status read failed")
        raise
    audit("BedroomClimateGetStatus", resolved, "SUCCEEDED", "Climate status read")
    return result


@mcp.tool()
def BedroomClimateSetTemperature(temperature: float, entity_id: str = "") -> dict[str, Any]:
    """Set bedroom AC target temperature and return success only after HA confirms it."""
    resolved = resolve_entity_id(entity_id)
    return action_and_verify(resolved, "set_temperature", {"temperature": temperature}, "temperature", temperature)


@mcp.tool()
def BedroomClimateSetFanMode(fan_mode: str, entity_id: str = "") -> dict[str, Any]:
    """Set bedroom AC fan speed. Query BedroomClimateGetStatus for valid fan_modes."""
    resolved = resolve_entity_id(entity_id)
    current = state(resolved)
    require_supported_value(current, "fan_modes", fan_mode)
    return action_and_verify(resolved, "set_fan_mode", {"fan_mode": fan_mode}, "fan_mode", fan_mode)


@mcp.tool()
def BedroomClimateSetSwingMode(swing_mode: str, entity_id: str = "") -> dict[str, Any]:
    """Set bedroom AC vertical swing. Query status first for valid swing_modes."""
    resolved = resolve_entity_id(entity_id)
    current = state(resolved)
    require_supported_value(current, "swing_modes", swing_mode)
    return action_and_verify(resolved, "set_swing_mode", {"swing_mode": swing_mode}, "swing_mode", swing_mode)


@mcp.tool()
def BedroomClimateSetHorizontalSwingMode(swing_horizontal_mode: str, entity_id: str = "") -> dict[str, Any]:
    """Set bedroom AC horizontal swing when Home Assistant exposes it."""
    resolved = resolve_entity_id(entity_id)
    current = state(resolved)
    require_supported_value(current, "swing_horizontal_modes", swing_horizontal_mode)
    return action_and_verify(resolved, "set_swing_horizontal_mode",
                             {"swing_horizontal_mode": swing_horizontal_mode},
                             "swing_horizontal_mode", swing_horizontal_mode)


if __name__ == "__main__":
    mcp.run(transport="stdio")
