#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"
STAGING_ROOT="${STAGING_ROOT:-/srv/fastllm-staging}"
export STAGING_ROOT
# Refuse production paths before creating files or invoking Docker.
python3 "$SCRIPT_DIR/guard.py"
ENV_FILE="$STAGING_ROOT/.env"
CONFIG_FILE="$STAGING_ROOT/config.json"
COMPOSE_FILE="$SCRIPT_DIR/compose.json"

umask 077
mkdir -p "$STAGING_ROOT/models" "$STAGING_ROOT/rag-data"

random_hex() {
  openssl rand -hex "$1"
}

if [[ ! -f "$ENV_FILE" ]]; then
  mysql_password="$(random_hex 24)"
  jwt_secret="$(random_hex 32)"
  admin_password="$(random_hex 24)"
  service_password="$(random_hex 24)"
  mcp_token="$(random_hex 32)"
  mcp_token_sha="$(printf '%s' "$mcp_token" | sha256sum | awk '{print $1}')"

  cat >"$ENV_FILE" <<ENV
STAGING_ROOT=$STAGING_ROOT
HOST_UID=$(id -u)
HOST_GID=$(id -g)
MYSQL_ROOT_PASSWORD=$mysql_password
JWT_SECRET=$jwt_secret
ADMIN_PASSWORD=$admin_password
SERVICE_ACCOUNT_PASSWORD=$service_password
MCP_BEARER_TOKEN=$mcp_token
MCP_BEARER_TOKEN_SHA256=$mcp_token_sha
ENV
  chmod 600 "$ENV_FILE"
fi

# Values are generated as hex/numeric/path-only strings, so sourcing does not
# evaluate user-controlled shell syntax.
# shellcheck disable=SC1090
source "$ENV_FILE"
export REPO_ROOT STAGING_ROOT HOST_UID HOST_GID MYSQL_ROOT_PASSWORD JWT_SECRET
export BACKEND_IMAGE MCP_IMAGE
export ADMIN_PASSWORD SERVICE_ACCOUNT_PASSWORD MCP_BEARER_TOKEN MCP_BEARER_TOKEN_SHA256

for required in MYSQL_ROOT_PASSWORD JWT_SECRET ADMIN_PASSWORD SERVICE_ACCOUNT_PASSWORD \
                MCP_BEARER_TOKEN MCP_BEARER_TOKEN_SHA256 BACKEND_IMAGE MCP_IMAGE; do
  [[ -n "${!required:-}" ]] || { echo "Missing $required in $ENV_FILE" >&2; exit 1; }
done

if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
  printf '::add-mask::%s\n' "$MYSQL_ROOT_PASSWORD"
  printf '::add-mask::%s\n' "$JWT_SECRET"
  printf '::add-mask::%s\n' "$ADMIN_PASSWORD"
  printf '::add-mask::%s\n' "$SERVICE_ACCOUNT_PASSWORD"
  printf '::add-mask::%s\n' "$MCP_BEARER_TOKEN"
fi

sed \
  -e "s/__MYSQL_ROOT_PASSWORD__/$MYSQL_ROOT_PASSWORD/g" \
  -e "s/__JWT_SECRET__/$JWT_SECRET/g" \
  -e "s/__ADMIN_PASSWORD__/$ADMIN_PASSWORD/g" \
  "$SCRIPT_DIR/config.staging.json.tpl" >"$CONFIG_FILE"
chmod 600 "$CONFIG_FILE"

compose() {
  docker compose --env-file "$ENV_FILE" --file "$COMPOSE_FILE" "$@"
}

compose config --quiet
# Preserve the database and RAG volume while releasing RAM after each run.
trap 'compose stop --timeout 20 >/dev/null' EXIT
pull_images() {
  if docker image inspect "$BACKEND_IMAGE" "$MCP_IMAGE" \
       mirror.ccs.tencentyun.com/library/mysql:8.4 >/dev/null 2>&1; then
    return 0
  fi
  compose pull backend mcp mysql
}
for attempt in 1 2 3; do
  pull_images && break
  [[ "$attempt" == 3 ]] && { echo 'Image pull failed after three attempts' >&2; exit 1; }
  sleep $((attempt * 10))
done
compose up --detach --no-build --wait --wait-timeout 240 mysql backend mcp

# Reapply the idempotent schema so an existing staging volume receives newly
# added CREATE TABLE statements without requiring destructive resets.
compose exec --no-TTY \
  --env MYSQL_PWD="$MYSQL_ROOT_PASSWORD" \
  mysql mysql --user=root <"$REPO_ROOT/sql/init.sql"

backend_url='http://127.0.0.1:19006'
admin_response="$(
  curl --fail-with-body --silent --show-error \
    --header 'Content-Type: application/json' \
    --data "$(printf '{\"username\":\"admin\",\"password\":\"%s\"}' "$ADMIN_PASSWORD")" \
    "$backend_url/api/auth/login"
)"
admin_token="$(printf '%s' "$admin_response" | jq --raw-output '.token // empty')"
[[ -n "$admin_token" ]] || { echo 'Admin login did not return a token' >&2; exit 1; }

create_user_payload="$(
  jq --compact-output --null-input \
    --arg password "$SERVICE_ACCOUNT_PASSWORD" \
    '{username:"mcp_staging", password:$password, tenant_id:1, role:"user"}'
)"
create_user_status="$(
  curl --silent --show-error --output "$STAGING_ROOT/create-user-response.json" \
    --write-out '%{http_code}' \
    --header "Authorization: Bearer $admin_token" \
    --header 'Content-Type: application/json' \
    --data "$create_user_payload" \
    "$backend_url/api/users"
)"
if [[ "$create_user_status" != 200 && "$create_user_status" != 409 ]]; then
  echo "Service-account creation failed with HTTP $create_user_status" >&2
  exit 1
fi

list_documents() {
  curl --fail-with-body --silent --show-error \
    --header "Authorization: Bearer $admin_token" \
    "$backend_url/api/documents"
}

documents="$(list_documents)"
doc_id="$(printf '%s' "$documents" | jq --raw-output \
  '.documents[]? | select(.filename == "staging-e2e.md") | .id' | head -n1)"
doc_status="$(printf '%s' "$documents" | jq --raw-output \
  '.documents[]? | select(.filename == "staging-e2e.md") | .status' | head -n1)"

if [[ -n "$doc_id" && "$doc_status" == failed ]]; then
  echo 'The staging fixture failed. Inspect it without deleting stored data.' >&2
  exit 1
fi

if [[ -z "$doc_id" ]]; then
  seed_payload="$(jq --compact-output --null-input \
    '{filename:"staging-e2e.md", visibility:"group", content:"The staging verification codename is ORCHID-NEBULA-7429. This deterministic document exists only to validate tenant-bound MCP retrieval."}')"
  seed_response="$(
    curl --fail-with-body --silent --show-error \
      --header "Authorization: Bearer $admin_token" \
      --header 'Content-Type: application/json' \
      --data "$seed_payload" \
      "$backend_url/api/documents"
  )"
  doc_id="$(printf '%s' "$seed_response" | jq --raw-output '.id // empty')"
  [[ -n "$doc_id" ]] || { echo 'Document upload did not return an id' >&2; exit 1; }
fi

for _ in $(seq 1 60); do
  doc_status="$(list_documents | jq --raw-output --argjson id "$doc_id" \
    '.documents[]? | select(.id == $id) | .status')"
  [[ "$doc_status" == ready ]] && break
  [[ "$doc_status" == failed ]] && { echo 'Staging document processing failed' >&2; exit 1; }
  sleep 2
done
[[ "$doc_status" == ready ]] || { echo 'Timed out waiting for staging document indexing' >&2; exit 1; }

compose exec --no-TTY \
  --env MCP_E2E_TOKEN="$MCP_BEARER_TOKEN" \
  --env MCP_E2E_URL='http://127.0.0.1:3100/mcp' \
  mcp node /app/e2e.mjs

compose ps
echo 'Staging deployment and MCP E2E completed successfully.'
