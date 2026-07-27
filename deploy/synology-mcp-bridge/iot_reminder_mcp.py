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


def api_url(path: str) -> str:
    base = os.environ.get("IOT_API_URL", "").strip().rstrip("/")
    if not base:
        raise RuntimeError("IOT_API_URL is not configured")
    return f"{base}{path}"


def request(method: str, path: str, payload: dict[str, Any] | None = None) -> Any:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload else None
    response_request = Request(
        api_url(path), data=body, method=method, headers={"Content-Type": "application/json"}
    )
    try:
        with urlopen(response_request, timeout=10) as response:
            result = json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        raise RuntimeError(f"IoT reminder API rejected the request ({exc.code})") from exc
    except URLError as exc:
        raise RuntimeError("IoT reminder API is unavailable") from exc
    if result.get("code") != 200:
        raise RuntimeError(f"IoT reminder API failed: {result.get('errorCode', 'UNKNOWN')}")
    return result.get("data")


def report_execution(tool_name: str, target_summary: str, status: str, result_summary: str) -> None:
    """Write a best-effort audit event without changing a tool's result."""
    payload = {
        "toolName": tool_name,
        "targetSummary": target_summary[:300],
        "status": status,
        "resultSummary": result_summary[:500],
    }
    try:
        request("POST", "/api/mcp-tool-executions", payload)
    except RuntimeError:
        pass


def normalized_time(remind_at: str) -> str:
    try:
        value = datetime.fromisoformat(remind_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError(
            "remind_at must be ISO-8601, for example 2026-07-27T20:00:00"
        ) from exc
    return value.isoformat(timespec="seconds")


def request_id(device_code: str, message: str, remind_at: str) -> str:
    material = "\n".join((device_code.strip(), message.strip(), remind_at)).encode("utf-8")
    return "xiaozhi-mcp-" + hashlib.sha256(material).hexdigest()[:48]


@mcp.tool()
def aiot_create_reminder(device_code: str, message: str, remind_at: str) -> dict[str, Any]:
    """Create a one-time reminder for a XiaoZhi device.

    Use ISO-8601 local time such as 2026-07-27T20:00:00. Retried calls with
    the same device, text and time reuse a deterministic idempotency key.
    """
    due = normalized_time(remind_at)
    target = f"{device_code.strip()} @ {due}"
    try:
        data = request(
            "POST",
            "/api/reminders",
            {
                "requestId": request_id(device_code, message, due),
                "deviceCode": device_code.strip(),
                "message": message.strip(),
                "remindAt": due,
            },
        )
    except RuntimeError:
        report_execution("aiot_create_reminder", target, "FAILED", "IoT reminder API failed")
        raise
    report_execution("aiot_create_reminder", target, "SUCCEEDED", "Reminder created")
    return {"success": True, "reminder": data}


@mcp.tool()
def aiot_list_reminders(device_code: str = "") -> dict[str, Any]:
    """List reminders, optionally limited to one device code."""
    target = device_code.strip() or "all devices"
    suffix = "" if not device_code.strip() else "?deviceCode=" + quote(device_code.strip(), safe="")
    try:
        reminders = request("GET", "/api/reminders" + suffix)
    except RuntimeError:
        report_execution("aiot_list_reminders", target, "FAILED", "IoT reminder API failed")
        raise
    report_execution("aiot_list_reminders", target, "SUCCEEDED", "Reminders listed")
    return {"success": True, "reminders": reminders}


@mcp.tool()
def aiot_cancel_reminder(reminder_id: int) -> dict[str, Any]:
    """Cancel a scheduled reminder by its numeric reminder_id."""
    target = str(reminder_id)
    try:
        request("DELETE", f"/api/reminders/{reminder_id}")
    except RuntimeError:
        report_execution("aiot_cancel_reminder", target, "FAILED", "IoT reminder API failed")
        raise
    report_execution("aiot_cancel_reminder", target, "SUCCEEDED", "Reminder canceled")
    return {"success": True, "reminderId": reminder_id, "status": "CANCELED"}


if __name__ == "__main__":
    mcp.run(transport="stdio")
