"""MCP tools that adapt XiaoZhi calls to the local AIoT reminder API."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

from fastmcp import FastMCP


mcp = FastMCP("AIoT Reminders")


def resolve_device_code(provided_device_code: str = "") -> str:
    """Resolve the reminder target without exposing it in logs or errors.

    An explicitly configured bridge default always wins over a value supplied
    by the upstream XiaoZhi MCP call. This prevents stale device metadata in
    the official endpoint from delivering reminders to an old device.
    """
    configured_default = os.environ.get("DEFAULT_DEVICE_CODE", "").strip()
    if configured_default:
        return configured_default

    supplied = provided_device_code.strip()
    if supplied:
        return supplied

    raise ValueError(
        "No reminder target is configured. Set DEFAULT_DEVICE_CODE in the private "
        "bridge.env, or provide device_code explicitly."
    )


def api_url(path: str) -> str:
    base = os.environ.get("IOT_API_URL", "").strip().rstrip("/")
    if not base:
        raise RuntimeError("IOT_API_URL is not configured")
    return f"{base}{path}"


def request(method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    req = Request(api_url(path), data=body, method=method, headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=10) as response:
            result = json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        raise RuntimeError(f"IoT reminder API rejected the request ({exc.code})") from exc
    except URLError as exc:
        raise RuntimeError("IoT reminder API is unavailable") from exc
    if result.get("code") != 200:
        raise RuntimeError(f"IoT reminder API failed: {result.get('errorCode', 'UNKNOWN')}")
    return result.get("data")


def report_execution(tool_name: str, target_summary: str, status: str, result_summary: str) -> None:
    """Best-effort audit; it must never change a tool's primary result."""
    payload = json.dumps({"toolName": tool_name, "targetSummary": target_summary[:300],
                          "status": status, "resultSummary": result_summary[:500]}, ensure_ascii=False).encode("utf-8")
    try:
        req = Request(api_url("/api/mcp-tool-executions"), data=payload, method="POST",
                      headers={"Content-Type": "application/json"})
        with urlopen(req, timeout=5):
            pass
    except (HTTPError, URLError, RuntimeError):
        pass


def normalized_time(remind_at: str) -> str:
    try:
        value = datetime.fromisoformat(remind_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError("remind_at must be an ISO-8601 local time, for example 2026-07-27T20:00:00") from exc
    return value.isoformat(timespec="seconds")


def request_id(device_code: str, message: str, remind_at: str) -> str:
    material = "\n".join((device_code.strip(), message.strip(), remind_at)).encode("utf-8")
    return "xiaozhi-mcp-" + hashlib.sha256(material).hexdigest()[:48]


def target_summary(device_code: str, detail: str = "") -> str:
    """Return an audit-safe summary derived from the resolved device code.

    The raw device code must never be written to the bridge or IoT audit logs.
    A short one-way fingerprint still lets operators correlate records for the
    same final target without revealing its identifier.
    """
    fingerprint = hashlib.sha256(device_code.encode("utf-8")).hexdigest()[:12]
    return f"reminder-target:{fingerprint}{detail}"


@mcp.tool()
def aiot_create_reminder(message: str, remind_at: str, device_code: str = "") -> dict[str, Any]:
    """Create a one-time reminder for a XiaoZhi device.

    Use ISO-8601 local time such as 2026-07-27T20:00:00. The bridge prevents
    duplicate creation when the same device, message and time are retried. A
    private DEFAULT_DEVICE_CODE, when configured, always takes precedence over
    device_code supplied by the caller.
    """
    due = normalized_time(remind_at)
    resolved_device_code = resolve_device_code(device_code)
    target = target_summary(resolved_device_code, f" @ {due}")
    try:
        data = request("POST", "/api/reminders", {"requestId": request_id(resolved_device_code, message, due),
                "deviceCode": resolved_device_code, "message": message.strip(), "remindAt": due})
    except RuntimeError as exc:
        report_execution("aiot_create_reminder", target, "FAILED", "IoT reminder API failed")
        raise exc
    report_execution("aiot_create_reminder", target, "SUCCEEDED", "Reminder created")
    return {"success": True, "reminder": data}


@mcp.tool()
def aiot_list_reminders(device_code: str = "") -> dict[str, Any]:
    """List reminders for the configured default or an explicitly supplied device."""
    resolved_device_code = resolve_device_code(device_code)
    target = target_summary(resolved_device_code)
    suffix = "?deviceCode=" + quote(resolved_device_code, safe="")
    try:
        reminders = request("GET", "/api/reminders" + suffix)
    except RuntimeError as exc:
        report_execution("aiot_list_reminders", target, "FAILED", "IoT reminder API failed")
        raise exc
    report_execution("aiot_list_reminders", target, "SUCCEEDED", "Reminders listed")
    return {"success": True, "reminders": reminders}


@mcp.tool()
def aiot_cancel_reminder(reminder_id: int) -> dict[str, Any]:
    """Cancel a scheduled reminder by its numeric reminder_id."""
    try:
        request("DELETE", f"/api/reminders/{reminder_id}")
    except RuntimeError as exc:
        report_execution("aiot_cancel_reminder", str(reminder_id), "FAILED", "IoT reminder API failed")
        raise exc
    report_execution("aiot_cancel_reminder", str(reminder_id), "SUCCEEDED", "Reminder canceled")
    return {"success": True, "reminderId": reminder_id, "status": "CANCELED"}


if __name__ == "__main__":
    mcp.run(transport="stdio")
