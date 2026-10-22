#!/usr/bin/env bash
# Source this file from deploy.sh after its isolation guard has passed.
pull_image() {
  local image="$1"
  if docker image inspect "$image" >/dev/null 2>&1; then
    return 0
  fi
  if [[ "${STAGING_OFFLINE:-0}" == 1 ]]; then
    echo "Missing image in offline staging bundle: $image" >&2
    return 1
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
