"""Windows MCP agent exposing only explicitly approved laptop controls."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import socket
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import cv2
from fastmcp import FastMCP
from fastmcp.utilities.types import Image

ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "apps.json"
CAPTURES_DIR = ROOT / "captures"
LOGS_DIR = ROOT / "logs"
AUDIT_LOG = LOGS_DIR / "actions.jsonl"

mcp = FastMCP(
    "Windows Laptop Controls",
    instructions=(
        "Controls a user-approved Windows laptop. Only use pc_open_app with a name "
        "returned by pc_list_allowed_apps. Camera access is single-photo only. "
        "WeGame, VALORANT and League of Legends are owner-approved fixed launch "
        "tasks: call their pc_open_* tool when requested, and never claim that "
        "administrator permission is required."
    ),
)

ELEVATED_GAME_TASKS = {
    "WeGame": "Xiaozhi Game - WeGame",
    "无畏契约": "Xiaozhi Game - VALORANT",
    "英雄联盟": "Xiaozhi Game - League of Legends",
}


def load_config() -> dict[str, Any]:
    if not CONFIG_PATH.exists():
        raise RuntimeError("apps.json is missing.")
    try:
        config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"apps.json is not valid JSON: {exc.msg}") from exc
    if not isinstance(config.get("allowed_apps"), dict):
        raise RuntimeError("apps.json must contain an allowed_apps object")
    return config


def audit(event: str, **details: Any) -> None:
    LOGS_DIR.mkdir(exist_ok=True)
    record = {"time": datetime.now(timezone.utc).isoformat(), "event": event, **details}
    with AUDIT_LOG.open("a", encoding="utf-8") as file:
        file.write(json.dumps(record, ensure_ascii=False) + "\n")


def get_app(app_name: str) -> tuple[str, dict[str, Any]]:
    apps: dict[str, Any] = load_config()["allowed_apps"]
    normalized = app_name.strip().casefold()
    for name, definition in apps.items():
        if name.casefold() == normalized and isinstance(definition, dict):
            return name, definition
    raise ValueError(f"'{app_name}' is not approved. Use pc_list_allowed_apps.")


def start_approved_app(app_name: str) -> dict[str, str]:
    name, definition = get_app(app_name)
    if name in ELEVATED_GAME_TASKS:
        task_name = ELEVATED_GAME_TASKS[name]
        result = subprocess.run(
            ["schtasks.exe", "/Run", "/TN", task_name],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if result.returncode != 0:
            audit("start_game_failed", app=name, task=task_name, exit_code=result.returncode)
            raise RuntimeError(
                f"The fixed launch task for '{name}' did not start: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        audit("start_game_task", app=name, task=task_name)
        return {"status": "started", "app": name, "launch_mode": "fixed_windows_task"}

    path = definition.get("path")
    args = definition.get("args", [])
    working_directory = definition.get("working_directory")
    if not isinstance(path, str) or not path.strip():
        raise RuntimeError(f"Approved app '{name}' has no executable path")
    if not isinstance(args, list) or not all(isinstance(item, str) for item in args):
        raise RuntimeError(f"Approved app '{name}' has invalid fixed arguments")
    executable = Path(os.path.expandvars(path)).expanduser()
    if not executable.is_file():
        raise RuntimeError(f"Approved executable for '{name}' was not found on this laptop")
    cwd = None
    if working_directory:
        cwd_path = Path(os.path.expandvars(working_directory)).expanduser()
        if not cwd_path.is_dir():
            raise RuntimeError(f"Working directory for '{name}' was not found")
        cwd = str(cwd_path)
    subprocess.Popen(
        [str(executable), *args], cwd=cwd, shell=False, close_fds=True,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS,
    )
    audit("open_app", app=name)
    return {"status": "started", "app": name}


@mcp.tool()
def pc_get_status() -> dict[str, Any]:
    """Return safe laptop status and the number of approved applications."""
    config = load_config()
    return {
        "hostname": socket.gethostname(),
        "time": datetime.now().astimezone().isoformat(timespec="seconds"),
        "approved_app_count": len(config["allowed_apps"]),
        "photo_capture": "single photo only",
        "live_preview": "not enabled",
    }


@mcp.tool()
def pc_list_allowed_apps() -> list[str]:
    """List application names the laptop owner explicitly approved."""
    return sorted(load_config()["allowed_apps"].keys())


@mcp.tool()
def pc_open_app(app_name: str) -> dict[str, str]:
    """Open one explicitly approved application by its exact approved name."""
    return start_approved_app(app_name)


APP_SHORTCUTS = {
    "pc_open_qq": "QQ", "pc_open_wechat": "微信", "pc_open_browser": "浏览器",
    "pc_open_wegame": "WeGame", "pc_open_valorant": "无畏契约",
    "pc_open_steam": "Steam", "pc_open_league_of_legends": "英雄联盟",
    "pc_open_netease_music": "网易云音乐", "pc_open_wps": "WPS",
    "pc_open_clash_verge": "Clash Verge",
}


def register_app_shortcuts() -> None:
    for tool_name, app_name in APP_SHORTCUTS.items():
        def open_shortcut(name: str = app_name) -> dict[str, str]:
            return start_approved_app(name)
        open_shortcut.__name__ = tool_name
        if app_name in ELEVATED_GAME_TASKS:
            open_shortcut.__doc__ = (
                f"Directly launch the user-approved game {app_name} through its fixed "
                "Windows launch task. This is already owner-authorized; do not ask for "
                "administrator permission or claim that it is unavailable."
            )
        else:
            open_shortcut.__doc__ = f"Open the user-approved application {app_name}."
        mcp.tool(name=tool_name)(open_shortcut)


register_app_shortcuts()


def confirm_then_start(app_name: str) -> dict[str, str]:
    approved_name, _ = get_app(app_name)
    answer = ctypes.windll.user32.MessageBoxW(
        0, f"小智请求启动“{approved_name}”。\n\n仅当您点击“是”后才会启动。",
        "小智电脑确认", 0x00000004 | 0x00000020,
    )
    if answer != 6:
        audit("start_cancelled", app=approved_name)
        return {"status": "cancelled", "app": approved_name}
    result = start_approved_app(approved_name)
    audit("start_confirmed", app=approved_name)
    return result


@mcp.tool()
def pc_confirm_start_wegame() -> dict[str, str]:
    """Ask for local confirmation before starting WeGame; never elevates Windows."""
    return confirm_then_start("WeGame")


@mcp.tool()
def pc_confirm_start_valorant() -> dict[str, str]:
    """Ask for local confirmation before starting VALORANT; never elevates Windows."""
    return confirm_then_start("无畏契约")


@mcp.tool()
def pc_confirm_start_league_of_legends() -> dict[str, str]:
    """Ask for local confirmation before starting League of Legends."""
    return confirm_then_start("英雄联盟")


def capture_current_view() -> Image:
    config = load_config()
    camera_index = config.get("camera_index", 0)
    max_width = config.get("photo_max_width", 1280)
    if not isinstance(camera_index, int) or not isinstance(max_width, int) or max_width < 320:
        raise RuntimeError("camera_index or photo_max_width is invalid")
    camera = cv2.VideoCapture(camera_index, cv2.CAP_DSHOW)
    if not camera.isOpened():
        raise RuntimeError("Camera could not be opened. Check Windows permissions.")
    try:
        for _ in range(3):
            camera.read()
        ok, frame = camera.read()
    finally:
        camera.release()
    if not ok or frame is None:
        raise RuntimeError("Camera did not return a frame")
    height, width = frame.shape[:2]
    if width > max_width:
        frame = cv2.resize(frame, (max_width, round(height * max_width / width)), interpolation=cv2.INTER_AREA)
    CAPTURES_DIR.mkdir(exist_ok=True)
    filename = datetime.now().strftime("photo-%Y%m%d-%H%M%S.jpg")
    photo_path = CAPTURES_DIR / filename
    if not cv2.imwrite(str(photo_path), frame, [cv2.IMWRITE_JPEG_QUALITY, 85]):
        raise RuntimeError("Photo could not be written")
    audit("take_photo", file=filename)
    return Image(path=photo_path, format="jpeg")


@mcp.tool()
def pc_see_camera() -> Image:
    """Capture the current camera view and return it to XiaoZhi."""
    return capture_current_view()


@mcp.tool()
def pc_take_photo() -> Image:
    """Take one current camera photo, save it locally, and return it."""
    return capture_current_view()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default=8765, type=int)
    args = parser.parse_args()
    if args.host in {"0.0.0.0", "127.0.0.1", "localhost"}:
        raise SystemExit("Refusing non-Tailscale bind address")
    load_config()
    mcp.run(transport="streamable-http", host=args.host, port=args.port, path="/mcp", stateless_http=True, show_banner=False)


if __name__ == "__main__":
    main()
