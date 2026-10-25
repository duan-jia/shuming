# Repository Guidelines

## Project Structure & Module Organization

This repository contains a local LLM + RAG question-answering platform. The C++17 backend lives in `engine/`: public headers are under `engine/include/`, implementations under `engine/src/`, and `engine/src/main.cpp` wires the HTTP routes. Major backend areas are `server/`, `persistence/`, `auth/`, `security/`, `inference/`, and `rag/`. SQL setup and migrations are in `sql/`. Python evaluation utilities and query fixtures are in `tests/`. The Next.js frontend is isolated in `web/`; read `web/AGENTS.md` before changing frontend code. Runtime artifacts such as `build/`, `build_nocuda/`, `data/`, `models/*.gguf`, and local configs are not source.

## Build, Test, and Development Commands

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF`: configure a CPU-only backend build.
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86`: configure a CUDA build; adjust the architecture for the target GPU.
- `cmake --build build --target fast-llm-reasoner -j$(nproc)`: build the server.
- `cmake --build build --target fast-llm-cli -j$(nproc)`: build the CLI test tool used by RAG evaluation.
- `./build/bin/fast-llm-reasoner --config config.json`: run the backend locally.
- `cd web && npm install && npm run dev`: run the frontend on `localhost:3000`.
- `cd web && npm run lint`: run frontend ESLint.

## Coding Style & Naming Conventions

Follow the existing C++ style: 4-space indentation, `snake_case` file names, and namespaces/directories organized by subsystem. Keep headers in `engine/include/<area>/` paired with matching `.cpp` files in `engine/src/<area>/`. Prefer standard library facilities and existing wrappers before adding new dependencies. Frontend code uses TypeScript, React, and Next.js conventions in `web/src/app/`.

## Testing Guidelines

Focused C++ tests are enabled by `FASTLLM_BUILD_TESTS` (see `engine/CMakeLists.txt`). Validate backend changes by building relevant targets and exercising endpoints or the CLI. Evaluation has two independent tracks: `python3 tests/agent_eval.py --predictions <normalized-traces.jsonl>` checks tool arguments, dependencies and evidence contracts; `python3 tests/rag_eval.py --base-url <backend> --document-map <import-map.json>` retrieves the current 100-query paper dataset. RAG uses `EMOS_EVAL_TOKEN` from the environment and supports offline `--predictions`. Incomplete relevance judgments produce precision bounds and known-positive recall, never fabricated exact P/R. The authoritative data is `tests/datasets/recent-papers-v1/`. See `tests/README.md` for datasets, limitations, and commands. Run metric regression with `python3 -m unittest discover -s tests -p 'test_evaluation.py'`.

## Commit & Pull Request Guidelines

Git history currently uses short imperative or scoped messages, for example `docs: change ...` and `Initial commit: ...`. Prefer concise, descriptive subjects with an optional scope such as `engine:`, `web:`, `sql:`, or `docs:`. Pull requests should describe the change, list build/test commands run, note configuration or migration impacts, and include screenshots for visible frontend changes.

## Security & Configuration Tips

Never commit `config.json`, `.env*`, GGUF model files, generated RAG indexes, or secrets. Start from `config.example.json`, use local credentials, and rotate default admin passwords before production use.

