# Windows Laptop MCP Agent

This agent is deliberately restrictive. It exposes only:

- laptop status;
- launching entries explicitly listed in `apps.json`;
- direct voice-friendly tools for selected approved applications, including WeGame and VALORANT;
- one current camera image per tool call, returned directly to XiaoZhi.

It never accepts arbitrary PowerShell, shell commands, URLs, command-line arguments, or file paths from XiaoZhi.

For applications that voice models commonly misclassify as privileged (such as game launchers), the agent also offers an explicit tool such as `pc_open_wegame` or `pc_open_valorant`. These tools still run only the fixed, owner-approved executable and never elevate Windows privileges.

For games, prefer `pc_confirm_start_wegame`, `pc_confirm_start_valorant`, or `pc_confirm_start_league_of_legends`. The tool first shows a confirmation dialog on the laptop; it launches nothing until the person at the laptop clicks **Yes**. This is designed for an interactive signed-in Windows session, not a background Scheduled Task session.

## Setup

1. Copy `apps.example.json` to `apps.json`.
2. Add only approved executable paths to `apps.json`.
3. Start the agent with `start-agent.ps1`. It binds only to the laptop's Tailscale IP and writes its private MCP URL to `agent-status.txt`.
4. Set `PC_MCP_URL` in the NAS bridge environment to that URL, then recreate the NAS bridge container.

## Camera privacy

`pc_see_camera` lets XiaoZhi inspect the current camera image directly; `pc_take_photo` also saves a timestamped JPEG under `captures/`. Both tools open the camera only for one capture and then release it. This is an on-demand image, not continuous recording or an always-on public video stream.
