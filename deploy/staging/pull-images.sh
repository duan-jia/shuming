#!/usr/bin/env bash
# Source this file only after the staging isolation guard has passed.
# GHCR transfers use a client-side proxy, not the shared Docker daemon's network.
pull_private_image() (
  set -Eeuo pipefail
  : "${DOCKER_CONFIG:?Temporary registry configuration is required}"
  local image="$1" work attempt
  umask 077
  work=$(mktemp -d "$DOCKER_CONFIG/pull.XXXXXX")
  trap 'rm -rf -- "$work"' EXIT
  if timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/7890' 2>/dev/null; then
    export HTTPS_PROXY=http://127.0.0.1:7890 HTTP_PROXY=http://127.0.0.1:7890
    export https_proxy="$HTTPS_PROXY" http_proxy="$HTTP_PROXY"
    export NO_PROXY=localhost,127.0.0.1 no_proxy=localhost,127.0.0.1
    echo 'Using existing loopback proxy for client-side GHCR image transfer.'
  fi
  # Fixed upstream release and SHA256; never execute an unverified download.
  curl --fail --silent --show-error --location --retry 2 \
    --connect-timeout 15 --max-time 120 \
    https://github.com/google/go-containerregistry/releases/download/v0.22.1/go-containerregistry_Linux_x86_64.tar.gz \
    --output "$work/tool.tar.gz"
  printf '%s  %s\n' \
    '0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0' \
    "$work/tool.tar.gz" | sha256sum --check
  tar --extract --gzip --file "$work/tool.tar.gz" --directory "$work" crane
  for attempt in 1 2; do
    echo "Downloading $image with client-side transport (attempt $attempt/2)"
    # Keep the original compressed layers. An interrupted uncompressed cache
    # can retain a zero-byte layer that later loads but cannot be unpacked.
    if timeout --kill-after=10s 600s "$work/crane" pull \
      --platform linux/amd64 --format tarball \
      "$image" "$work/image.tar"; then
      # This archive exists only in a private per-job directory on the server.
      # It is never uploaded as an Actions artifact and is removed by EXIT trap.
      timeout --kill-after=10s 180s docker load --input "$work/image.tar"
      docker image inspect "$image" >/dev/null
      return 0
    fi
    [[ "$attempt" == 2 ]] || sleep 5
  done
  echo "Client-side GHCR transfer failed after two bounded attempts: $image" >&2
  return 1
)

pull_image() {
  local image="$1"
  if docker image inspect "$image" >/dev/null 2>&1; then
    return 0
  fi
  if [[ "${STAGING_OFFLINE:-0}" == 1 ]]; then
    echo "Missing image in offline staging cache: $image" >&2
    return 1
  fi
  if [[ "$image" == ghcr.io/* ]]; then
    pull_private_image "$image"
    return
  fi
  local attempt
  for attempt in 1 2 3; do
    echo "Pulling $image (attempt $attempt/3)"
    if timeout --kill-after=10s 120s docker pull "$image"; then
      return 0
    fi
    [[ "$attempt" == 3 ]] || sleep $((attempt * 5))
  done
  echo "Image pull failed after three bounded attempts: $image" >&2
  return 1
}
