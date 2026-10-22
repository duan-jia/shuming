#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
umask 077
python3 configure.py
# Keep mihomo diagnostics private: providers can include subscription URLs.
if ! sudo -n docker compose -f compose.yaml run --rm --no-deps mihomo -t -d /root/.config/mihomo -f /root/.config/mihomo/config.json >validation.log 2>&1; then
    echo 'Mihomo validation failed. Inspect the private validation.log on this server.' >&2
    exit 1
fi
sudo -n docker compose -f compose.yaml up -d --force-recreate --pull never
provider_ready() {
    sudo -n docker compose -f compose.yaml exec -T mihomo wget -T 3 -qO- \
        http://127.0.0.1:9090/providers/proxies 2>/dev/null | python3 -c '
import json, sys
try:
    nodes = json.load(sys.stdin)["providers"]["subscription"]["proxies"]
    sys.exit(0 if nodes and all(n.get("type", "").lower() not in ("direct", "reject") for n in nodes) else 1)
except (ValueError, KeyError):
    sys.exit(1)
'
}
for attempt in {1..12}; do
    if provider_ready && curl --silent --fail --proxy http://127.0.0.1:7890 --noproxy '' \
        --connect-timeout 5 --max-time 10 https://api.github.com/ >/dev/null; then
        echo 'Proxy ready at http://127.0.0.1:7890; GitHub API reachable.'
        exit 0
    fi
    sleep 2
done
sudo -n docker compose -f compose.yaml stop
echo 'Proxy stopped: subscription nodes or GitHub connectivity could not be verified. Check the private logs and subscription.' >&2
exit 1
