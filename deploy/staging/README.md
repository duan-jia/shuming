# Staging deployment

This directory deploys the smallest real MCP acceptance environment on the
Tencent Cloud self-hosted runner:

- MySQL 8.4
- CPU-only C++ RAG backend
- `bge-small-zh-v1.5-q4_k_m.gguf` embedding model
- TypeScript MCP gateway

The 3B chat model is intentionally omitted because the staging VM has about
3.6 GiB RAM and the MCP acceptance path only requires embeddings.

## Security model

- MySQL is reachable only on the Compose network.
- Backend and MCP ports bind to host loopback only.
- First deployment generates credentials in
  `/opt/fast-llm-reasoner/staging/.env` with mode `0600`.
- Runtime configuration and model/data files stay outside the repository.
- The workflow accepts only same-repository pull requests and targets the
  dedicated `staging` runner label.

## Automatic verification

`.github/workflows/staging-e2e.yml` downloads and verifies the pinned embedding
model, builds the images on the staging runner, starts the stack, creates a
dedicated service account and deterministic knowledge document, then checks:

1. MCP authentication.
2. Service-account login to the C++ backend.
3. Tenant-bound `search_knowledge` retrieval.
4. Rejection of caller-supplied `tenant_id`.

The persistent Compose project is named `fast-llm-reasoner-staging`.
