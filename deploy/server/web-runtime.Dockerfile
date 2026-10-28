ARG NODE_IMAGE=node:24-bookworm-slim
FROM ${NODE_IMAGE}
ARG DEBIAN_MIRROR=deb.debian.org
ARG PIP_INDEX_URL=https://pypi.org/simple
RUN sed -i "s|deb.debian.org|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get install --yes --no-install-recommends python3 python3-venv poppler-utils \
    && python3 -m venv /opt/pdf-python \
    && /opt/pdf-python/bin/pip install --no-cache-dir pypdf==6.19.0 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app/web
COPY --chown=node:node .artifacts/web/ ./
COPY scripts/pdf_to_markdown.py scripts/pdf_text.py scripts/paper_card.py /app/scripts/
ENV NODE_ENV=production NEXT_TELEMETRY_DISABLED=1 HOSTNAME=0.0.0.0 PORT=3000 \
    PROJECT_ROOT=/app PDF_PROCESSOR_PYTHON=/opt/pdf-python/bin/python3 \
    PDF_SOURCE_DIR=/data/pdfs PDF_PROCESSED_DIR=/data/processed \
    BACKEND_API_BASE=http://backend:9006
USER node
EXPOSE 3000
CMD ["node", "server.js"]
