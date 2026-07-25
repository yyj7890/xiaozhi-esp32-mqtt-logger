FROM python:3.12-slim

ARG MCP_PIPE_REPOSITORY=https://github.com/78/mcp-calculator.git
ARG MCP_PIPE_REF=c537f71d61fd73b47d6c8955b5df6d3721acf4e4

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates git \
    && git clone "\${MCP_PIPE_REPOSITORY}" /opt/mcp-pipe \
    && cd /opt/mcp-pipe \
    && git checkout --detach "\${MCP_PIPE_REF}" \
    && pip install --no-cache-dir -r requirements.txt \
    && apt-get purge --yes --auto-remove git \
    && rm -rf /var/lib/apt/lists/* /opt/mcp-pipe/.git

COPY bridge_entrypoint.py /app/bridge_entrypoint.py

WORKDIR /opt/mcp-pipe
ENTRYPOINT ["python", "/app/bridge_entrypoint.py"]
