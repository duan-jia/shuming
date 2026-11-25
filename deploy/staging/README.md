# Isolated staging

> Current automation trusts same-repository PR branches from maintainers/agents.
> Staging and production share a host-level trust boundary despite separate data
> and Compose resources; this is not a sandbox for untrusted code. Review runner
> isolation before accepting external contributions or changing visibility.
> See the [release checklist](../../docs/open-source-readiness.md).


Relevant same-repository PRs, relevant master pushes and manual master runs build
backend, MCP and frontend images on GitHub-hosted runners. Only a source snapshot,
REVISION and SHA256SUMS are uploaded as the staging-bundle Actions artifact.
**Image archives must never be attached to that artifact.**

Hosted builds have per-ref concurrency and cancel obsolete builds. Only the
self-hosted deployment job holds the shared `lab-server-release` group, with
cancellation disabled, so an earlier server download cannot block new hosted
builds. Production release and staging smoke retain the same shared server lock.

The Tencent runner downloads and verifies the source-only bundle, then pulls the
backend/MCP images from GHCR and MySQL from the Tencent mirror with bounded retries.
Registry login uses this job's short-lived GITHUB_TOKEN in a per-run Docker config;
the config is removed after pulls and again by an always-run cleanup step.
The subsequent deploy runs offline from the cached images. Frontend is built but
not started by staging. Existing production services and their image names do not change.

GHCR package visibility/ACLs are separately managed; this workflow does not set
packages private or prove their visibility. If package access is denied or registry
pulls time out, staging fails without falling back to an image-bearing artifact.
Validate this path on the real runner before merging. Public repository/fork
workflow permissions still need a separate security review: job conditions are
not a sandbox or a proof that private packages cannot be accessed by forks.

Source-bundle downloads preserve the runner's existing default transport. The fallback
transfer path accepts only a source-only bundle for the same run. Legacy bundles
containing images are rejected by the exact file allowlist.

Old Actions bundles can still contain images until they expire or are explicitly
removed; a workflow change cannot retroactively redact them. Keep the repository
private until legacy artifacts and actual GHCR visibility/access inheritance have
been reviewed. This change neither deletes old bundles nor changes package settings.

| Resource | Production | CI staging |
|---|---|---|
| Compose project | fast-llm-reasoner-staging | fast-llm-reasoner-ci-staging |
| Root | /opt/fast-llm-reasoner/staging | /srv/fastllm-staging |
| MySQL volume | fast-llm-reasoner-staging_mysql-data | fast-llm-reasoner-ci-staging_mysql-data |
| Backend loopback port | 9006 | 19006 |
| MCP loopback port | 3100 | 13100 |
| Network | fast-llm-reasoner-staging_staging | fast-llm-reasoner-ci-staging_staging |

The existing production project retains its historical staging name so that
this change does not migrate or replace its database. CI staging joins neither
lab-web nor lab-knowledge. Its MySQL publishes no port. Credentials and test
documents belong only to its own volume and directories. Tests require no
remote-model key and do not copy production data or model configuration.

Create /srv/fastllm-staging for the gha-runner user before the first run.
guard.py refuses other roots, Compose overrides, symlinked data directories and
unexpected environment entries before the script creates files or runs Docker.
The script and workflow stop test containers after every run; they preserve
MySQL and RAG data. A nonblocking deployment lock also prevents concurrent
manual/CI runs from changing the same stack. Fallback cleanup takes the same
lock and leaves another deployment's containers alone. Never use down --volumes.

### Upgrade and runtime preflight

Before starting application containers, staging runs a synthetic PDF through
the backend image as `HOST_UID:HOST_GID`, without network access or host mounts.
It checks script access, text extraction/cache and PNG rendering. The image
makes its code directories readable by arbitrary deployment UIDs; credentials
remain in the existing protected config mount.

Staging then stops backend/MCP writers, starts only MySQL and reapplies
`sql/init.sql` for missing tables. `migrate.py` inspects the existing schema,
saves a mode-0600 database snapshot under `/srv/fastllm-staging/backups`, and
applies only missing quota/auth columns and required BIGINT widening. It compares
identities/password hashes and daily usage before/after. Fresh databases are a
no-op; retries resume partial DDL without adding duplicate columns/tables.
Unexpected column types or an incomplete session table stop deployment for
inspection. Only the isolated staging project/root can use this command.
Production migrations remain a separate reviewed operation.

New test service passwords use 16 hex characters. Existing 48-character staging
credentials are rotated through the admin API and saved before MCP is recreated.
This never changes production service-account credentials.

The runner has Docker access on the production host. Container and data
separation prevent accidental overlap but do not provide a security boundary
against malicious code. Only trusted repository members should execute PR
code on this runner. Use a separate VM when accepting untrusted contributors.

Production release remains manual through workflow_dispatch on master; neither
PRs, master pushes nor publishing a GitHub Release automatically deploy production. Run Staging
E2E manually on master after merging, then use that successful run's full image
SHA in Production release. PR image tags can name GitHub's synthetic merge
commit, so do not infer an image tag from the PR head SHA. The release retains
model settings, database volumes and MCP network overrides. Database schema
migrations require a separate reviewed procedure.

Manual runs can reuse locally cached images after a layer-unpack probe. Missing
non-GHCR images use three pull attempts of at most 120 seconds each.
STAGING_OFFLINE=1 instead fails immediately
when an image is missing; CI uses this mode so it cannot silently fall back to
registry downloads. Compose startup always uses --pull never.

If the server cannot download the artifact within ten minutes, an operator can
download this run's `staging-bundle` ZIP through GitHub and transfer it over SSH
to `/srv/fastllm-staging/bundles/<run-id>.zip`. Install it with mode 0600 for
gha-runner, using a temporary filename followed by an atomic rename. The job
uses this file after a download failure; retry only the deployment job if it
has already ended. It verifies checksums and the exact workflow commit before
importing anything. Both download paths run the same isolation checks and E2E.
The ZIP is deleted after the run. This fallback needs no registry credentials
or production configuration.

An optional loopback-only mihomo download proxy can be prepared with
[deploy/proxy](../proxy/README.md). Downloads do not select it merely because port 7890 is listening. They preserve
the existing default/inherited transport; the offline fallback remains.
Subscription credentials stay on the server outside the repository.

The distributed backend image disables GGML_NATIVE so the embedding backend
uses explicit AVX2/FMA/F16C features instead of inheriting the build runner's
instructions, including Intel AMX. Backend CI uses the same setting. The live
staging retrieval check exercises an embedding on the deployment CPU and
catches instruction incompatibility that build-runner-only tests cannot.

### GHCR image transfer on this server

Docker image pulls run in the daemon, so exporting a proxy in a CI shell does
not change the shared daemon's network path. GHCR downloads therefore use the
SHA256-pinned crane v0.22.1 client using the runner's existing default/inherited transport.
The job's temporary Docker authentication is reused, with no permanent token or
daemon reconfiguration. Only the transfer subprocess disables Go HTTP/2 using
`GODEBUG=http2client=0`, following the stalled proxy transfers observed during
the release. Each image has two attempts with a 30-second manifest deadline and
180-second archive deadline. The two-attempt limit and deadlines are unchanged.
Retries discard incomplete archives, but keep verified complete compressed layers
in a job-private native crane cache. Before every use, the helper removes unknown,
non-private, partial or SHA/size-mismatching entries against the current manifest.
This is complete-layer reuse, NOT byte-range resume of an incomplete blob. The
cache is not shared across jobs or persisted on the host; existing Docker images
still require the unstarted-container unpack probe. Cache plus tarball can use
about twice the compressed size, unlike the previous tarball-only transfer. A
manifest-size capacity gate keeps 20% of current free space outside the estimated
transfer working set. This is not a promise that Docker unpacking fits; measure
remaining space and unpacking needs before authorizing another run. No host
cache pruning or network/configuration changes are performed.

The tarball retains original compressed layers and uses no uncompressed
filesystem cache. Before import, `verify-image.py` checks the registry config
digest, each compressed blob's size/digest and each uncompressed `rootfs.diff_id`.
A subsequent unstarted container probe verifies Docker can unpack the layers;
neither `docker load` exiting zero nor `image inspect` alone counts as success.
The probe removes only its own container/anonymous volumes and never starts an
application. A corrupt cached image is rejected offline or fetched again online.

Archives and raw diagnostics stay in a mode-0700 per-job temporary workspace and
are deleted on exit. Transfer telemetry reports only stage, exit code, elapsed seconds, archive/cache
bytes and fixed error-category labels. Raw errors, headers, URLs, signed query
parameters and authorization text are never printed. Manifest, pull, verify,
load, inspect and unpack are separate stages; verification/import/unpack failures
fail closed without a second download. No verbose HTTP tracing is enabled.
The manifest summary reports expected compressed layer bytes; cache_bytes includes
partial bytes and must not be misreported as verified bytes or network throughput.
Exit 124 identifies a command deadline; 137 means killed (forced deadline kill
or another SIGKILL cannot be distinguished). Neither proves the cause of a CDN
stall. Image archives never enter Actions artifacts. Always-run cleanup
also removes this run/attempt's registry authentication and downloads. MySQL
continues using the Tencent mirror and bounded Docker pull.

### Build reuse and regression coverage

Hosted backend CI caches compiler results with a 1 GiB ccache limit and compiler
content checking. Pinned FetchContent sources use a separate CMake-definition
key; exact source-cache hits configure without re-downloading dependencies.
Quota integration tests and fresh/legacy/interrupted-schema migration tests run
against the job's isolated MySQL. PDF-cache and browser-auth client regressions
also run in normal CI.

Staging images use Buildx registry caches under each existing private image
package's `buildcache` tag. Multi-stage build results are reused in `max` mode;
image-layer caches are not exported to the public repository's Actions cache or
artifacts. Build-record artifact uploads are disabled. New caches start cold;
the workflow reports compiler-cache statistics instead of claiming an unmeasured
speedup. Package visibility and trusted-PR gates remain unchanged.

### Bundle downloader bootstrap failures

Before importing the source bundle, CI fetches `download-bundle.py` at the exact
workflow revision. A listening loopback proxy is not proof of healthy upstream
TLS. The bootstrap retries transient network failures and HTTP 408/429/5xx
(500/502/503/504) up to three attempts (30 seconds each, 1/2 second backoff).
Authorization errors and redirects fail closed; no TLS checks are disabled.
Persistent failure still requires the existing run-specific offline ZIP fallback;
it is not treated as a successful transfer. All bundle digest/revision/content
checks remain mandatory. GitHub's `continue-on-error` can show the download step
as successful even when its **outcome** is failure; inspect the actual log.

Offline regression check (no network, containers, or deployment):
`python3 -m unittest discover -s tests -p test_staging_bundle_bootstrap.py -v`.

### Pinned offline crane input

CI's image-fetch step selects the fixed archive
`/srv/fastllm-staging/tools/go-containerregistry-v0.22.1-linux-x86_64.tar.gz`
through `STAGING_CRANE_ARCHIVE`. Before authorizing a new run, an operator must
place the original upstream v0.22.1 Linux x86_64 archive there using a task-specific
temporary filename and atomic publication (do not overwrite a different file).
The file must be owned by the runner user, mode 0600, a regular file, and its
canonical absolute path must not traverse symlinks. Do not change directory,
server, proxy or credential permissions to accommodate this input.

Expected SHA256:
`0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0`.
The archive is copied to the existing mode-0700 job-private transfer directory,
checked against the fixed digest, and only then is `crane` extracted. A missing,
unsafe or mismatched explicit offline input fails closed without online fallback.
If no offline variable is selected, non-CI callers retain the existing bounded
online download. Cached valid images continue to bypass tool preparation.
The credential cleanup removes job-private copies, not the operator's source
archive. Do not commit the binary or attach image archives to Actions artifacts.

Offline unit check:
`python3 -m unittest discover -s tests -p test_staging_crane_offline.py -v`.
The matching-fixture unit test mocks only the pinned digest verifier; the bad
archive test uses real SHA256. Validate any actual upstream input against the
fixed digest separately before transfer. No unit test contacts Docker/registries.

This input only removes the crane-tool download dependency. Bundle retrieval,
GHCR manifests/blobs and MySQL pulls may still fail; it does not prove registry
network health. Re-running an old Actions run still uses its old scripts and
cannot acquire this feature. A reviewed update and separately authorized new
execution are needed; neither publishing this code nor placing the file is a
standing deployment authorization.

Rollback: revert the offline-input change and remove only its task-owned source
archive after confirming no run is consuming it. Do not remove database volumes,
restore data, or change production as part of tool rollback. A deployment may
migrate staging data and seed test records; those effects need a separate reviewed
recovery plan. Existing cleanup stops the CI staging stack even after a failure.
Logs saying `Stopped` alone do not prove that the attempt caused a fresh outage:
inspect container state/timestamps before attributing availability changes.

When automatic source-bundle download fails, the same-run offline fallback now
waits at most 180 seconds for the operator's atomic file publication. The step
has a five-minute ceiling; no network retry, extraction or deployment occurs
while waiting. This window allows copying the new hosted artifact without
racing the self-hosted job. The revision and content checks still follow before
any use. Missing input after the deadline fails closed. Upload the ZIP associated
with that exact new run; never relabel an older ZIP or its REVISION.

### Preserve the existing default transport

Source bootstrap and GHCR client transfers no longer overwrite proxy/no_proxy
variables just because a loopback listener exists. The runner's existing default
path is tried with unchanged request deadlines and retry bounds. Explicit
inherited proxy settings are preserved, not erased or replaced. There is no
automatic probe-based switch and no system/daemon/network configuration change.
This follows server evidence: default public GitHub API returned200 and GHCR
returned401, while explicit loopback proxy returned curl35/HTTP000. These are
connectivity observations, not private-manifest/layer authorization proof.
Private requests still use only the executing CI job's own temporary token.

Regression: `python3 -m unittest discover -s tests -p test_staging_default_transport.py -v`.
Tests simulate an unhealthy listening proxy, verify unchanged unset/inherited
proxy variables, no automatic listener probe/network call, and private cleanup.

### Strict ranged GHCR transport (review implementation; private compatibility unverified)

CI selects `STAGING_IMAGE_TRANSPORT=range`; callers without the selector retain
the previous pinned-crane implementation. There is no automatic fallback from a
failed range transfer to another whole-image download.

The helper reads only this job's mode0600 `DOCKER_CONFIG/config.json` under its
canonical mode0700 directory. Supported images are the backend/MCP packages with
full40-character revision tags. Basic authentication goes only to the exact
`https://ghcr.io/token` realm, with read-only repository scope; Bearer goes only
to the registry origin. No credential helper, offline refresh token, new secret,
token export, or cross-job token reuse is supported.

A single linux/amd64 manifest is pinned before consuming config/layer digests.
Blob requests follow at most5 HTTPS hops restricted to ghcr.io and
pkg-containers.githubusercontent.com, with no userinfo, fragment, alternate port,
or Authorization on any redirected request. Short-lived storage401/403 responses
restart from the original registry endpoint, never from a saved signed URL.

Ranges are128KiB, at most8 workers, with2 attempts per chunk. Retained prefixes
resume at their exact offset; completed chunks remain while others retry.
Every response must be206 with exact Content-Range/total and identity encoding.
A server ignoring Range with200 fails closed, without appending its body.
The540-second image-wide budget is rechecked between socket reads;570-second
outer timeout is a hard backstop. No whole-image retry extends that budget.
Limits:128 layers,4MiB metadata/config,2GiB per blob,4GiB total compressed data.
A2-copy compressed working-set estimate retains20% of current free space; it
does not promise Docker unpacking will fit or prune anything.

Blobs are published only after exact size/full SHA256 and atomic rename. The
assembled Docker archive is then subject to the existing config/layer/diff_id
verifier, Docker load, image inspection and unstarted unpack probe. Helpers remove
job-private blob/partial data; outer workflow cleanup removes the original
temporary auth config. Diagnostics are fixed categories and numeric progress,
never URL/header/token/error-body text. Host networking/permissions are unchanged.

Synthetic protocol and shell tests cover interruption/resume, expired locations,
cross-origin auth stripping, unsupported redirects, ignored Range, bad totals/
hashes, platform selection, capacity and deadlines. A synthetic assembled archive
also passes the actual pinned crane validator offline. **These are not evidence
that real private GHCR storage accepts these Range requests or meets the budget.**
Only a separately authorized feature-branch merge and one new staging run can
verify that; it may migrate the isolated DB/write fixtures/finally stop services.
No old run should be retried to pick up this implementation.


#### Measured Range budget calibration (Nov25, converged)
Run45352557117 retained about90.67/111.86MB in360s. The linear~445s estimate
is NOT a throughput guarantee. Range now has a540s image-wide budget and570s
hard backstop;8 workers/128KiB/2 chunk attempts and full integrity checks remain.
Each GHCR whole pipeline has720s+10s kill grace, including download, verify45s,
load45s, inspect10s and probe20s plus its bounded cleanup. No whole-image retry.

The fetch step remains30min. Inclusive ceilings (kill grace already included):
backend730s +MCP730s +MySQL130s +login30s +guard25s +mkdir10s +logout15s
+auth-directory removal25s +bookkeeping allowance30s =1725s.
Explicit unused margin:75s below1800s. There is no double-counted grace.
Cleanup logout/removal have their own reserved windows, not a reliance on the
top-level30min kill. A cleanup timeout is failure, not claimed clean.
The whole-image720s window includes post-download checks; any slow stage fails
closed rather than skipping integrity. Frontend is hosted-build-only, not part
of the CI staging runtime. Deploy/E2E10min and finalstop2min are separate existing
steps. No actual deployment/E2E success is implied by this budget calculation.
