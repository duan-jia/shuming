# Optional staging download proxy

Mihomo runs as an independent Compose project on the deployment server. Only
127.0.0.1:7890 is published. It does not change production containers, Docker
routing, DNS, the host default route, or the Docker daemon's proxy settings.
The staging artifact download step detects this listener and uses HTTP proxy
environment variables; without it the existing direct/offline paths remain.

Install compose.yaml, configure.py and start.sh in
/home/ubuntu/mihomo-private, owned by ubuntu with directory mode 0700. Create
an empty subscription.url with mode 0600. Private subscriptions, generated
configs and logs stay outside the checkout and deployment artifacts.

On that server, as ubuntu:

```sh
nano ~/mihomo-private/subscription.url
bash ~/mihomo-private/start.sh
```

Paste an HTTPS Clash/Mihomo subscription URL or a
clash://install-config?url=... import link into the file. It must return a
Clash/Mihomo YAML configuration or provider with a proxies list; plain Base64
subscriptions need conversion by the provider. Never send the URL to a public
subscription converter. The generated JSON is valid YAML for mihomo and uses
an HTTP provider refreshed hourly, with automatic node selection. The
subscription's own routing rules are not imported: all requests explicitly
sent to this dedicated download proxy use its selected node.

If the subscription endpoint rejects the server with HTTP 403, download its
Clash/Mihomo configuration on a trusted local machine and extract its `proxies`
list into a private YAML file. Copy that file over SSH to
`~/mihomo-private/subscription.yaml` with mode 0600, then run start.sh.
A nonempty local subscription.yaml takes precedence over subscription.url.
The server loads those nodes from disk, bypassing the blocked endpoint. This
snapshot does not automatically renew; repeat the local download and SSH copy
when nodes expire or credentials change. To return to hourly HTTP updates,
move subscription.yaml out of the directory and restart with a working URL.
Do not put node files or subscription URLs in Git, CI artifacts or public logs.

The startup script validates configuration with the pinned mihomo image before
starting, checks the live provider has actual proxy nodes, then tests GitHub API
connectivity. It stops the proxy on readiness failure so an empty provider cannot
silently pass through a direct connection. The controller listens only inside
the container on 127.0.0.1:9090 and is not published. Diagnostic output is kept in the
private validation.log because it may contain subscription credentials. The
first image pull still needs a working path to Docker Hub or an offline image
import. Downloading a real staging artifact must be verified after activation;
API connectivity alone does not establish artifact download reliability.

Stop only this proxy:

```sh
cd ~/mihomo-private
sudo docker compose -f compose.yaml stop
```
