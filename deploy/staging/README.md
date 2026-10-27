# Isolated staging

> Current automation trusts same-repository PR branches from maintainers/agents.
> Staging and production share a host-level trust boundary despite separate data
> and Compose resources; this is not a sandbox for untrusted code. Review runner
> isolation before accepting external contributions or changing visibility.
> See the [release checklist](../../docs/open-source-readiness.md).


Relevant PRs from branches in this repository, relevant pushes to master, and
manual runs on master build immutable backend, MCP and frontend
images on GitHub-hosted runners. A serialized job tests backend login, document
indexing and MCP retrieval on the Tencent server. The build job uploads a
checksummed bundle containing the tracked source and backend/MCP/MySQL images.
The server downloads that bundle, verifies its checksums and imports images
locally; it does not clone from github.com or pull from GHCR during CI staging.
Artifact-service connectivity is still required. Downloads and imports have
explicit timeouts, and temporary bundles are removed after each run.
The downloader uses parallel byte ranges with bounded retries and checks the
artifact's SHA-256 digest. GitHub API credentials are never forwarded to the
storage redirect URL. It needs only the workflow token's read access to Actions.
Fork PRs run ordinary hosted CI; the staging jobs reject fork sources.
Shared staging and production operations use the same concurrency group and do
not cancel an active deployment. GitHub concurrency can replace an older pending
run; it is not a FIFO guarantee or a separate preview environment for each PR. The frontend receives build and lint checks;
staging does not yet test browser chat or PDF processing.

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
MySQL and RAG data. Never use down --volumes to clean up staging.

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

Manual runs can reuse locally cached images. Missing images use three pull
attempts of at most 120 seconds each. STAGING_OFFLINE=1 instead fails immediately
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
