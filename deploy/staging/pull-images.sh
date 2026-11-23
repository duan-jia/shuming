#!/usr/bin/env bash
# Source this file only after the staging isolation guard has passed.
# GHCR transfers use the client's existing transport, not daemon reconfiguration.
STAGING_TRANSFER_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

probe_image() (
  # Creating an unstarted container forces layer unpacking. inspect/load alone
  # can succeed for the corrupt empty layer observed on the deployment host.
  local probe="shuming-image-probe-${BASHPID}-${RANDOM}"
  trap 'docker rm --volumes "$probe" >/dev/null 2>&1 || true' EXIT
  docker create --name "$probe" --label org.shuming.purpose=image-unpack-check \
    --network none --pull never --entrypoint /bin/true "$1" >/dev/null
)

# Prepare only a pinned archive in the job-private workspace. An explicit offline
# input fails closed; never replace a bad/missing file with an online download.
prepare_crane() {
  local work="$1" offline="${STAGING_CRANE_ARCHIVE:-}"
  if [[ -n "$offline" ]]; then
    if [[ "$offline" != /* || ! -f "$offline" || -L "$offline" ]] \
      || [[ "$(readlink -f -- "$offline")" != "$offline" ]] \
      || [[ "$(stat -c '%u:%a' -- "$offline")" != "$(id -u):600" ]]; then
      echo 'Offline crane archive must be a canonical regular file owned by this user with mode 0600.' >&2
      return 1
    fi
    cp -- "$offline" "$work/tool.tar.gz" || return 1
    chmod 600 "$work/tool.tar.gz" || return 1
  else
    curl --fail --silent --show-error --location --retry 1 \
      --connect-timeout 10 --max-time 60 \
      https://github.com/google/go-containerregistry/releases/download/v0.22.1/go-containerregistry_Linux_x86_64.tar.gz \
      --output "$work/tool.tar.gz" || return 1
  fi
  printf '%s  %s\n' \
    '0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0' \
    "$work/tool.tar.gz" | sha256sum --check || return 1
  tar --extract --gzip --file "$work/tool.tar.gz" --directory "$work" crane || return 1
}

pull_private_image() (
  set -Eeuo pipefail
  : "${DOCKER_CONFIG:?Temporary registry configuration is required}"
  local image="$1" work attempt started
  umask 077
  work=$(mktemp -d "$DOCKER_CONFIG/pull.XXXXXX")
  trap 'rm -rf -- "$work"' EXIT
  # This proxy path stalled with Go HTTP/2; scope the workaround to this client.
  export GODEBUG="${GODEBUG:+${GODEBUG},}http2client=0"
  # Preserve the runner's default/inherited transport; a listening proxy may
  # have broken upstream TLS. Never override proxy variables automatically.
  run_stage() {
    local stage="$1" budget="$2" stage_started=$SECONDS rc
    shift 2
    if timeout --kill-after=10s "$budget" "$@" 2>"$work/transfer.err"; then
      rc=0
    else
      rc=$?
    fi
    python3 "$STAGING_TRANSFER_DIR/transfer-state.py" report "$work" \
      --stage "$stage" --rc "$rc" --seconds "$((SECONDS - stage_started))" >&2 || return 1
    return "$rc"
  }
  case "${STAGING_IMAGE_TRANSPORT:-crane}" in
    range)
      run_stage ranges 390s python3 "$STAGING_TRANSFER_DIR/ranged-image.py" "$image" "$work" || return 1
      run_stage verify 120s python3 "$STAGING_TRANSFER_DIR/verify-image.py" "$work/image.tar" "$work/manifest.json" || return 1
      run_stage load 120s docker load --input "$work/image.tar" || return 1
      run_stage inspect 30s docker image inspect "$image" >/dev/null || return 1
      local probe_started=$SECONDS probe_rc
      if probe_image "$image" 2>"$work/transfer.err"; then probe_rc=0; else probe_rc=$?; fi
      python3 "$STAGING_TRANSFER_DIR/transfer-state.py" report "$work" --stage unpack --rc "$probe_rc" --seconds "$((SECONDS - probe_started))" >&2
      return "$probe_rc"
      ;;
    crane) prepare_crane "$work" ;;
    *) echo 'Unsupported staging image transport.' >&2; return 1 ;;
  esac
  for attempt in 1 2; do
    started=$SECONDS
    rm -f -- "$work/image.tar" "$work/manifest.json" "$work/transfer.err"
    echo "Downloading $image with client-side transport (attempt $attempt/2)"
    if ! run_stage manifest 30s "$work/crane" manifest --platform linux/amd64 \
        "$image" >"$work/manifest.json"; then
      # No layer data is consumed after a failed/partial manifest.
      [[ "$attempt" == 2 ]] || sleep 5
      continue
    fi
    # The native crane cache can contain partial files after termination. Keep
    # only complete manifest-matching compressed layers; never reuse diff_id or
    # unknown entries. This private cache survives only these two attempts.
    python3 "$STAGING_TRANSFER_DIR/transfer-state.py" cache "$work"
    if run_stage pull 180s "$work/crane" pull --platform linux/amd64 \
        --format tarball --cache_path "$work/cache" "$image" "$work/image.tar"; then
      # Integrity/import/unpack errors fail closed rather than redownloading.
      run_stage verify 120s python3 "$STAGING_TRANSFER_DIR/verify-image.py" \
        "$work/image.tar" "$work/manifest.json" || return 1
      run_stage load 120s docker load --input "$work/image.tar" || return 1
      run_stage inspect 30s docker image inspect "$image" >/dev/null || return 1
      # probe_image is a shell function; retain its existing cleanup trap.
      local probe_started=$SECONDS probe_rc
      if probe_image "$image" 2>"$work/transfer.err"; then probe_rc=0; else probe_rc=$?; fi
      python3 "$STAGING_TRANSFER_DIR/transfer-state.py" report "$work" \
        --stage unpack --rc "$probe_rc" --seconds "$((SECONDS - probe_started))" >&2
      [[ "$probe_rc" == 0 ]] || return 1
      echo "Verified/imported $image in $((SECONDS - started))s"
      return 0
    fi
    echo "Image transfer failed after $((SECONDS - started))s; retry reuses verified complete layers only." >&2
    [[ "$attempt" == 2 ]] || sleep 5
  done
  echo "Client-side GHCR transfer failed after two bounded attempts: $image" >&2
  return 1
)

pull_image() {
  local image="$1"
  if docker image inspect "$image" >/dev/null 2>&1; then
    if probe_image "$image"; then
      echo "Verified cached image: $image"
      return 0
    fi
    echo "Cached image cannot be unpacked: $image" >&2
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
    if timeout --kill-after=10s 120s docker pull "$image" && probe_image "$image"; then
      return 0
    fi
    [[ "$attempt" == 3 ]] || sleep $((attempt * 5))
  done
  echo "Image pull failed after three bounded attempts: $image" >&2
  return 1
}
