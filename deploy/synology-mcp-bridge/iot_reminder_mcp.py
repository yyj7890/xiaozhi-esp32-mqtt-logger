"""MCP tools that adapt XiaoZhi calls to the local AIoT reminder API."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
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


LOCAL_TIMEZONE = ZoneInfo("Asia/Shanghai")
CHINESE_DIGITS = {"零": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5,
                  "六": 6, "七": 7, "八": 8, "九": 9}


def chinese_number(value: str) -> int:
    """Parse the small Chinese numbers normally used in reminder phrases."""
    if value.isdigit():
        return int(value)
    if value == "十":
        return 10
    if "十" in value:
        left, _, right = value.partition("十")
        tens = CHINESE_DIGITS.get(left, 1) if left else 1
        ones = CHINESE_DIGITS.get(right, 0) if right else 0
        return tens * 10 + ones
    if value in CHINESE_DIGITS:
        return CHINESE_DIGITS[value]
    raise ValueError("unsupported number")


def local_now() -> datetime:
    return datetime.now(LOCAL_TIMEZONE).replace(tzinfo=None, microsecond=0)


def normalized_time(time_expression: str, now: datetime | None = None) -> str:
    """Resolve a safe subset of Chinese natural-language reminder times.

    Supported examples include: ``两分钟后``, ``1小时后``, ``明天早上八点`` and
    ``今晚 20:30``.  ISO-8601 remains accepted so the official AI may pass an
    already resolved time.  Ambiguous wording is rejected instead of creating
    a reminder for an unintended time.
    """
    value = time_expression.strip()
    if not value:
        raise ValueError("time_expression is required")
    reference = (now or local_now()).replace(microsecond=0)

    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is not None:
            parsed = parsed.astimezone(LOCAL_TIMEZONE).replace(tzinfo=None)
        return parsed.isoformat(timespec="seconds")
    except ValueError:
        pass

    import re

    relative = re.fullmatch(
        r"(?:再|过)?([0-9]+|[零一二两三四五六七八九十]+)\s*(分钟|分|小时|钟头|天|日)(?:后|以后|之后)",
        value,
    )
    if relative:
        amount = chinese_number(relative.group(1))
        if amount <= 0:
            raise ValueError("delay must be greater than zero")
        unit = relative.group(2)
        delta = timedelta(minutes=amount) if unit in {"分钟", "分"} else (
            timedelta(hours=amount) if unit in {"小时", "钟头"} else timedelta(days=amount)
        )
        return (reference + delta).isoformat(timespec="seconds")

    clock = re.fullmatch(
        r"(?:(今天|今日|今晚|明天|明日|明晚|后天)(?:的)?(?:早上|上午|中午|下午|晚上)?)?"
        r"(?:(早上|上午|中午|下午|晚上))?\s*([0-9]+|[零一二两三四五六七八九十]+)"
        r"(?:点|时)(?:(半)|([0-9]{1,2})分?)?",
        value,
    )
    if clock:
        day_word, period, hour_text, half, minute_text = clock.groups()
        hour = chinese_number(hour_text)
        minute = 30 if half else int(minute_text or 0)
        if hour > 23 or minute > 59:
            raise ValueError("clock time is outside the valid range")
        if period in {"下午", "晚上"} or day_word in {"今晚", "明晚"}:
            if 1 <= hour <= 11:
                hour += 12
        elif period == "中午" and 1 <= hour <= 10:
            hour += 12
        offset = {"明天": 1, "明日": 1, "明晚": 1, "后天": 2}.get(day_word, 0)
        due = reference.replace(hour=hour, minute=minute, second=0) + timedelta(days=offset)
        if day_word is None and due <= reference:
            due += timedelta(days=1)
        return due.isoformat(timespec="seconds")

    raise ValueError(
        "I cannot safely resolve that time. Try expressions such as '两分钟后', "
        "'明天早上八点', '今晚八点半', or an ISO-8601 time."
    )


def selected_device_code(device_code: str) -> str:
    selected = device_code.strip() or os.environ.get("DEFAULT_DEVICE_CODE", "").strip()
    if not selected:
        raise ValueError("No target device is configured. Set DEFAULT_DEVICE_CODE in bridge.env.")
    return selected


def request_id(device_code: str, message: str, remind_at: str) -> str:
    material = "\n".join((device_code.strip(), message.strip(), remind_at)).encode("utf-8")
    return "xiaozhi-mcp-" + hashlib.sha256(material).hexdigest()[:48]


@mcp.tool()
def aiot_create_reminder(message: str, time_expression: str, device_code: str = "") -> dict[str, Any]:
    """Create a one-time reminder for a XiaoZhi device.

    Create reminders from ordinary time expressions, for example "两分钟后",
    "明天早上八点" or "今晚八点半". An ISO-8601 local time remains valid.
    When device_code is omitted, the bridge uses its private DEFAULT_DEVICE_CODE.
    Retried calls with the same device, text and resolved time reuse a
    deterministic idempotency key.
    """
    selected_device = selected_device_code(device_code)
    due = normalized_time(time_expression)
    target = f"{selected_device} @ {due}"
    try:
        data = request(
            "POST",
            "/api/reminders",
            {
                "requestId": request_id(selected_device, message, due),
                "deviceCode": selected_device,
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
