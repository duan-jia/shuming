# Single-server deployment

The frontend extends the existing backend, MySQL and MCP deployment. Chat and
PDF processing use a remote model; embeddings and retrieval run locally. Only
Caddy exposes public ports 80 and 443. Frontend port 3000, backend port 9006 and
MCP port 3100 bind to loopback; MySQL publishes no host port.

## Frontend image

Build from the repository root:

```bash
docker build -f deploy/server/web.Dockerfile -t fast-llm-reasoner-web:local .
```

On a small VM, compile the frontend on another Linux x86-64 machine first:

```bash
cd web
npm ci
BACKEND_API_BASE=http://backend:9006 NEXT_TELEMETRY_DISABLED=1 npm run build
cd ..
mkdir -p .artifacts/web
cp -a web/.next/standalone/. .artifacts/web/
mkdir -p .artifacts/web/.next/static .artifacts/web/public
cp -a web/.next/static/. .artifacts/web/.next/static/
cp -a web/public/. .artifacts/web/public/
docker build -f deploy/server/web-runtime.Dockerfile -t fast-llm-reasoner-web:local .
```

Transfer the artifacts before the last command if building the runtime image on
the VM. Both Dockerfiles accept NODE_IMAGE, DEBIAN_MIRROR and PIP_INDEX_URL build
arguments. The API rewrite target is fixed during compilation, so changing it
requires rebuilding. Native PDF routes read BACKEND_API_BASE at runtime.

## Existing server

| Path | Purpose |
|---|---|
| /srv/lab-knowledge/src | Deployment files and frontend artifacts |
| /srv/lab-knowledge/pdfs | Source PDFs, owned by UID/GID 1000 |
| /srv/lab-knowledge/processed | Generated Markdown and JSON |
| /srv/lab-knowledge/web-model.env | Model configuration, mode 0600 |
| /srv/lab-knowledge/backups | Integration configuration backups |
| /opt/fast-llm-reasoner/staging | Existing backend configuration and RAG index |
| /srv/waku-simple/tenants/admin | Existing Waku data |

MySQL keeps its existing fast-llm-reasoner-staging_mysql-data Docker volume.
Never remove that volume or run Compose down with --volumes. Back up MySQL, the
RAG directory and both PDF directories together. Scheduled backups are not
configured by this deployment.

Configure the model on the server without placing its key in arguments:

```bash
sudo python3 /srv/lab-knowledge/src/deploy/server/configure-model.py \
  --config /opt/fast-llm-reasoner/staging/config.json \
  --web-env /srv/lab-knowledge/web-model.env
```

Recreate backend and web afterwards. Atomic file replacement means a plain
backend restart still reads its old bind-mounted inode.

Use the persistent integration overrides for all subsequent updates:

```bash
sudo REPO_ROOT=/srv/lab-knowledge/src docker compose \
  --env-file /opt/fast-llm-reasoner/staging/.env \
  -f /srv/lab-knowledge/backend.compose.yml \
  -f /srv/lab-knowledge/backend.integration.json up -d

sudo SERVER_ROOT=/srv/lab-knowledge docker compose \
  -f /srv/lab-knowledge/src/deploy/server/compose.json up -d

sudo docker compose -f /srv/waku-simple/compose.json \
  -f /srv/waku-simple/lab.compose.json up -d
```

The backend override pins the installed images. Choose new image versions
explicitly when upgrading. CI staging now uses a separate project, directory, network and database.
See [isolated staging](../staging/README.md) for the release workflow.

## Waku and HTTPS

Production shuming is https://emoslab.online; Waku is
https://waku.emoslab.online. Caddy manages certificates and automatic renewal
for both. The retired kb.emoslab.online route is removed; deleting its DNS
record also removes the ability to redirect visitors from that hostname.
The independent blog.emoslab.online route must be preserved.

Caddy forwards `/mcp` and `/mcp/*` directly to the C++ backend on lab-web;
these public endpoints require a backend JWT. Other requests go to web:3000.
Set knowledge_fetch.public_base_url to https://emoslab.online so signed PDF
links use the current domain. Keep the Waku hostname in
/srv/waku-simple/accounts.json synchronized with its Caddy route.

Waku connects privately through lab-knowledge to the compatibility gateway:

- lab_knowledge: http://mcp:3100/mcp/knowledge
- lab_web: http://mcp:3100/mcp/web

Both use LAB_KNOWLEDGE_MCP_TOKEN from lab-mcp.env. The gateway exchanges that
service token for a tenant-checked backend JWT and refreshes it automatically.
Each connection exposes its native search and fetch tools. The original
/mcp gateway endpoint remains compatible with older clients. Existing unrelated
MCP entries, including Treg, must be preserved.

The deployment enables web_search.enabled and mcp.allow_user_web_search;
the latter permits authenticated non-admin users to use the web tools as well.
The backend's lab-web network attachment and the pinned gateway image are in
/srv/lab-knowledge/backend.integration.json. Preserve these overrides when
updating. integrate-waku.py is the legacy single-tool setup helper; do not run
it over this configuration, as it does not prepare the native dual connection.

## Verify

Run the frontend build and lint, MCP tests and typecheck, and:

```bash
python3 -m unittest discover -s tests -p test_server_deploy.py
```

After deployment, check HTTPS, administrator login, anonymous API rejection,
a short remote-model chat and retrieval from Waku. Keep credentials and
retrieved knowledge on the server; report only pass/fail. Verification leaves
a chat titled Deployment verification in the administrator's history.

## Manual production release

Production release accepts a full image SHA from a successful Staging E2E run.
It pulls the three images, saves previous image IDs and Compose settings in
backups/release-*.json, recreates backend, MCP and frontend, and waits for health.
It restores the previous images if deployment fails. It preserves database,
model configuration, PDF directories, Waku containers and MCP network settings.
This rollback does not reverse SQL migrations; apply migrations separately.

The installed root-owned /usr/local/sbin/lab-production-release wrapper invokes
/usr/local/lib/lab-production-release/release.py. It accepts only the immutable
image revision and uses /home/gha-runner/.docker for registry authentication.
A narrowly scoped sudoers entry allows gha-runner to invoke that wrapper.
Changes to this privileged helper require installation by the server operator.
The Actions workflow never runs a checkout's Python code through sudo.

Preview image selections without changing the server:

```bash
python3 deploy/server/release.py --revision FULL_40_CHARACTER_SHA
```

After a successful release, verify HTTPS, login, chat and Waku retrieval.
The automated health checks confirm readiness, not answer quality or every
application route. Existing manually issued certificates still need renewal.

Install the privileged helper from the reviewed checkout as a server operator:

```bash
sudo install -d -o root -g root -m 755 /usr/local/lib/lab-production-release
sudo install -o root -g root -m 644 deploy/server/release.py /usr/local/lib/lab-production-release/release.py
sudo install -o root -g root -m 755 deploy/server/lab-production-release /usr/local/sbin/lab-production-release
```

The sudoers entry is `gha-runner ALL=(root) NOPASSWD: /usr/local/sbin/lab-production-release *`.
Install it through visudo with mode 0440 after explicitly approving this
privilege. The wrapper clears inherited environment variables, isolates Python
imports and accepts exactly one argument. The helper rejects non-SHA arguments
before it reads production files or runs Docker.
