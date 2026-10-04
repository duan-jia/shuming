# Isolated staging

PRs from branches in this repository build immutable backend, MCP and frontend
images on GitHub-hosted runners. A serialized job tests backend login, document
indexing and MCP retrieval on the Tencent server. Fork PRs run CI without
executing code on that server. The frontend receives build and lint checks;
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

Production release runs only through workflow_dispatch on master. Run Staging
E2E manually on master after merging, then use that successful run's full image
SHA in Production release. PR image tags can name GitHub's synthetic merge
commit, so do not infer an image tag from the PR head SHA. The release retains
model settings, database volumes and MCP network overrides. Database schema
migrations require a separate reviewed procedure.
