# Staging deployment

This deploys MySQL 8.4 from Tencent's mirror plus private backend and MCP images
from GHCR. GitHub-hosted Actions compile the images and embed the pinned,
SHA-256-verified BGE model. The Tencent host therefore does not need reliable
access to GitHub, Docker Hub, npm, or Hugging Face during deployment.

The 3B chat model is intentionally omitted because the staging VM has about
3.6 GiB RAM and MCP knowledge-search acceptance only requires embeddings.

Credentials are generated once in `/opt/fast-llm-reasoner/staging/.env` with
mode `0600`. MySQL remains on the private Compose network; backend and MCP bind
to host loopback only. The workflow creates a dedicated service account and
deterministic document, verifies tenant-bound retrieval, and rejects a caller-
supplied `tenant_id`.
