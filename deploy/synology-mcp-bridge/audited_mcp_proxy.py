"""Transparent stdio wrapper around mcp-proxy with safe tool-call auditing."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
from urllib.error import URLError
from urllib.request import Request, urlopen


pending: dict[str, tuple[str, str]] = {}


def audit(tool: str, target: str, status: str, result: str) -> None:
    base = os.environ.get("IOT_API_URL", "").strip().rstrip("/")
    if not base:
        return
    body = json.dumps(
        {"toolName": tool[:100], "targetSummary": target[:300], "status": status,
         "resultSummary": result[:500]}, ensure_ascii=False
    ).encode("utf-8")
    try:
        request = Request(base + "/api/mcp-tool-executions", data=body, method="POST",
                          headers={"Content-Type": "application/json"})
        with urlopen(request, timeout=5):
            pass
    except (URLError, OSError):
        pass


def target_of(arguments: object) -> str:
    if not isinstance(arguments, dict):
        return ""
    for key in ("entity_id", "device_id", "deviceCode", "name"):
        value = arguments.get(key)
        if isinstance(value, str):
            return value
    return ""


def stdin_loop(process: subprocess.Popen[str]) -> None:
    for line in sys.stdin:
        try:
            message = json.loads(line)
            if message.get("method") == "tools/call":
                params = message.get("params") or {}
                pending[str(message.get("id"))] = (
                    str(params.get("name", "unknown")), target_of(params.get("arguments"))
                )
        except (ValueError, AttributeError):
            pass
        process.stdin.write(line)
        process.stdin.flush()
    process.stdin.close()


def stdout_loop(process: subprocess.Popen[str]) -> None:
    for line in process.stdout:
        try:
            message = json.loads(line)
            call = pending.pop(str(message.get("id")), None)
            if call:
                failed = "error" in message
                audit(call[0], call[1], "FAILED" if failed else "SUCCEEDED",
                      "Tool call failed" if failed else "Tool call completed")
        except (ValueError, AttributeError):
            pass
        sys.stdout.write(line)
        sys.stdout.flush()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: audited_mcp_proxy.py <mcp-url>")
    process = subprocess.Popen(
        [sys.executable, "-m", "mcp_proxy", "--transport", "streamablehttp", sys.argv[1]],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
    )
    threading.Thread(target=stdin_loop, args=(process,), daemon=True).start()
    stdout_loop(process)
    process.wait()
