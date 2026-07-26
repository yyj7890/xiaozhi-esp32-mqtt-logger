"""Prepare a secure Home Assistant target and start Xiaozhi's official MCP pipe."""

from __future__ import annotations

import json
import os
import socket
import sys
import time
from pathlib import Path
from urllib.parse import urlparse


MCP_PIPE = Path("/opt/mcp-pipe/mcp_pipe.py")
GENERATED_CONFIG = Path("/tmp/mcp_config.json")


def required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def validate_url(name: str, value: str, schemes: set[str]) -> str:
    parsed = urlparse(value)
    if parsed.scheme not in schemes or not parsed.hostname:
        allowed = ", ".join(sorted(schemes))
        raise RuntimeError(f"{name} must use one of these URL schemes: {allowed}")
    return value


def wait_for_home_assistant(url: str) -> None:
    parsed = urlparse(url)
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    while True:
        try:
            with socket.create_connection((host, port), timeout=5):
                print(f"Home Assistant is reachable at {host}:{port}", flush=True)
                return
        except OSError:
            print(f"Waiting for Home Assistant at {host}:{port}...", flush=True)
            time.sleep(5)


def main() -> None:
    endpoint = validate_url(
        "MCP_ENDPOINT", required_env("MCP_ENDPOINT"), {"ws", "wss"}
    )
    ha_url = validate_url(
        "HA_MCP_URL",
        os.environ.get("HA_MCP_URL", "http://127.0.0.1:8123/api/mcp").strip(),
        {"http", "https"},
    )
    ha_token = required_env("HA_TOKEN")

    servers = {"home-assistant": {"type": "http", "url": ha_url}}
    pc_url = os.environ.get("PC_MCP_URL", "").strip()
    if pc_url:
        servers["windows-laptop"] = {
            "type": "http",
            "url": validate_url("PC_MCP_URL", pc_url, {"http", "https"}),
        }

    config = {"mcpServers": servers}
    GENERATED_CONFIG.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    os.chmod(GENERATED_CONFIG, 0o600)

    os.environ["MCP_ENDPOINT"] = endpoint
    os.environ["MCP_CONFIG"] = str(GENERATED_CONFIG)
    # mcp-proxy reads this variable and builds the Authorization header.
    os.environ["API_ACCESS_TOKEN"] = ha_token

    wait_for_home_assistant(ha_url)
    print(f"Starting Xiaozhi MCP bridge for: {', '.join(servers)}", flush=True)
    os.execv(sys.executable, [sys.executable, str(MCP_PIPE)])


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Configuration error: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(2) from exc
