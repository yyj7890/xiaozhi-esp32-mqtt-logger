ARG MCP_PIPE_BASE_IMAGE=python:3.12-slim-bookworm
FROM ${MCP_PIPE_BASE_IMAGE}

ARG MCP_PIPE_REPOSITORY=https://github.com/78/mcp-calculator.git
ARG MCP_PIPE_REF=c537f71d61fd73b47d6c8955b5df6d3721acf4e4
ARG MCP_PIPE_BOOTSTRAP=1

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN if [ "${MCP_PIPE_BOOTSTRAP}" = "1" ]; then \
        apt-get update \
        && apt-get install --yes --no-install-recommends ca-certificates git \
        && git clone "${MCP_PIPE_REPOSITORY}" /opt/mcp-pipe \
        && cd /opt/mcp-pipe \
        && git checkout --detach "${MCP_PIPE_REF}" \
        && pip install --no-cache-dir -r requirements.txt \
        && pip install --no-cache-dir --no-deps --force-reinstall "websockets==14.2" \
        && apt-get purge --yes --auto-remove git \
        && rm -rf /var/lib/apt/lists/* /opt/mcp-pipe/.git; \
    fi

COPY bridge_entrypoint.py /app/bridge_entrypoint.py
COPY audited_mcp_proxy.py /app/audited_mcp_proxy.py
COPY iot_reminder_mcp.py /app/iot_reminder_mcp.py

WORKDIR /opt/mcp-pipe
ENTRYPOINT ["python", "/app/bridge_entrypoint.py"]
