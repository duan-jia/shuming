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

The optional loopback proxy still applies to source-bundle downloads. The fallback
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
[deploy/proxy](../proxy/README.md). The download step uses it when port 7890
is listening; otherwise direct download and the offline fallback remain.
Subscription credentials stay on the server outside the repository.

The distributed backend image disables GGML_NATIVE so the embedding backend
uses explicit AVX2/FMA/F16C features instead of inheriting the build runner's
instructions, including Intel AMX. Backend CI uses the same setting. The live
staging retrieval check exercises an embedding on the deployment CPU and
catches instruction incompatibility that build-runner-only tests cannot.

### GHCR image transfer on this server

Docker image pulls run in the daemon, so exporting a proxy in a CI shell does
not change the shared daemon's network path. GHCR downloads therefore use the
SHA256-pinned crane v0.22.1 client and the existing loopback proxy when available.
The job's temporary Docker authentication is reused, with no permanent token or
daemon reconfiguration. Only the transfer subprocess disables Go HTTP/2 using
`GODEBUG=http2client=0`, following the stalled proxy transfers observed during
the release. Each image has two attempts with a 30-second manifest deadline and
180-second archive deadline. Retries discard incomplete archives.

The tarball retains original compressed layers and uses no uncompressed
filesystem cache. Before import, `verify-image.py` checks the registry config
digest, each compressed blob's size/digest and each uncompressed `rootfs.diff_id`.
A subsequent unstarted container probe verifies Docker can unpack the layers;
neither `docker load` exiting zero nor `image inspect` alone counts as success.
The probe removes only its own container/anonymous volumes and never starts an
application. A corrupt cached image is rejected offline or fetched again online.

Archives and raw diagnostics stay in a mode-0700 per-job temporary workspace and
are deleted on exit. Printed transfer errors redact URLs, including signed query
parameters. Image archives never enter Actions artifacts. Always-run cleanup
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
