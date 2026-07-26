# Windows Laptop MCP Agent

This agent is deliberately restrictive. It exposes only:

- laptop status;
- launching entries explicitly listed in `apps.json`;
- one current camera image per tool call, returned directly to XiaoZhi.

It never accepts arbitrary PowerShell, shell commands, URLs, command-line arguments, or file paths from XiaoZhi.

## Setup

1. Copy `apps.example.json` to `apps.json`.
2. Add only approved executable paths to `apps.json`.
3. Start the agent with `start-agent.ps1`. It binds only to the laptop's Tailscale IP and writes its private MCP URL to `agent-status.txt`.
4. Set `PC_MCP_URL` in the NAS bridge environment to that URL, then recreate the NAS bridge container.

## Camera privacy

`pc_see_camera` lets XiaoZhi inspect the current camera image directly; `pc_take_photo` also saves a timestamped JPEG under `captures/`. Both tools open the camera only for one capture and then release it. This is an on-demand image, not continuous recording or an always-on public video stream.
