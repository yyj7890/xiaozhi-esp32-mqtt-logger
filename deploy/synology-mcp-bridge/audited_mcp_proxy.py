"""Transparent stdio wrapper around mcp-proxy with safe tool-call auditing."""
from __future__ import annotations
import json, os, subprocess, sys, threading
from urllib.error import URLError
from urllib.request import Request, urlopen

pending: dict[str, tuple[str, str]] = {}

def audit(tool: str, target: str, status: str, result: str) -> None:
    base = os.environ.get("IOT_API_URL", "").rstrip("/")
    if not base: return
    body = json.dumps({"toolName": tool[:100], "targetSummary": target[:300], "status": status,
                       "resultSummary": result[:500]}, ensure_ascii=False).encode()
    try:
        with urlopen(Request(base + "/api/mcp-tool-executions", data=body, method="POST",
                             headers={"Content-Type": "application/json"}), timeout=5): pass
    except (URLError, OSError): pass

def target_of(arguments: object) -> str:
    if not isinstance(arguments, dict): return ""
    for key in ("entity_id", "device_id", "deviceCode", "name"):
        value = arguments.get(key)
        if isinstance(value, str): return value
    return ""

def stdin_loop(process: subprocess.Popen[str]) -> None:
    for line in sys.stdin:
        try:
            msg = json.loads(line)
            if msg.get("method") == "tools/call":
                params = msg.get("params") or {}
                pending[str(msg.get("id"))] = (str(params.get("name", "unknown")), target_of(params.get("arguments")))
        except (ValueError, AttributeError): pass
        process.stdin.write(line); process.stdin.flush()
    process.stdin.close()

def stdout_loop(process: subprocess.Popen[str]) -> None:
    for line in process.stdout:
        try:
            msg = json.loads(line); call = pending.pop(str(msg.get("id")), None)
            if call: audit(call[0], call[1], "FAILED" if "error" in msg else "SUCCEEDED", "Tool call completed" if "error" not in msg else "Tool call failed")
        except (ValueError, AttributeError): pass
        sys.stdout.write(line); sys.stdout.flush()

if __name__ == "__main__":
    if len(sys.argv) != 2: raise SystemExit("usage: audited_mcp_proxy.py <mcp-url>")
    p = subprocess.Popen([sys.executable, "-m", "mcp_proxy", "--transport", "streamablehttp", sys.argv[1]],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    a = threading.Thread(target=stdin_loop, args=(p,), daemon=True); a.start()
    stdout_loop(p); p.wait()
