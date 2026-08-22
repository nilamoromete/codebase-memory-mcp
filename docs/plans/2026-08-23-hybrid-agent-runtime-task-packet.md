# Hybrid Agent Runtime Task Packet

- **Task ID:** hybrid-agent-runtime
- **Date:** 2026-08-23
- **Owner / Agent:** Codex
- **Priority:** P1
- **Type:** feature / bugfix / release hardening
- **Target module/domain:** shared platform / agent tooling
- **Branch / worktree:** `feat/hybrid-agent-runtime` / `C:\opencoder\aliat\.worktrees\codebase-memory-mcp-upstream`
- **Affected journey:** Start Codex, Claude Code, or Pi in any project and use both code-intelligence engines without leaving idle processes.

## 1. Objective

Install the latest verified fork build and latest stable jCodeMunch companion for Codex, Claude Code, and Pi. All clients must expose both toolsets, preserve project isolation, share one heavy jCodeMunch runtime, and leave no integration-owned process after the last client exits.

## 2. Scope

In scope:

- integrate current upstream main into the fork feature line;
- build, verify, and install the exact fork commit;
- add fork-owned jCodeMunch companion lifecycle and client configuration;
- migrate Codex, Claude, and Pi transactionally;
- validate real clients in different projects;
- clean the erroneous FarmOS branch after successful acceptance.

Out of scope:

- FarmOS product behavior;
- vendoring jCodeMunch source;
- unrelated client upgrades;
- automatic prerelease adoption.

## 3. First files to inspect

- `src/cli/agent_clients.c`
- `src/cli/client_adapter.c`
- `src/cli/cli.c`
- `src/daemon/runtime.c`
- `src/main.c`
- `tests/test_agent_clients.c`
- `tests/test_cli.c`
- `tests/test_daemon_runtime.c`
- prior lifecycle source in the isolated FarmOS feature worktree, read-only

## 4. Suspect runtime path

1. Client starts configured STDIO MCP entries.
2. Native codebase-memory frontend joins its daemon session.
3. jCodeMunch bridge acquires a client/workspace lease.
4. Owner logic verifies or starts the pinned shared HTTP runtime.
5. Bridge proxies STDIO to that runtime.
6. Client exit releases the lease.
7. Last-lease grace expiry stops the exact runtime and monitor generation.

## 5. Contracts to preserve

- existing codebase-memory tool schemas and fork workflow tools;
- native daemon admission and shutdown behavior;
- unrelated Codex, Claude, and Pi configuration;
- existing index compatibility;
- loopback-only authenticated jCodeMunch HTTP access;
- exact process identity before cleanup.

## 6. Constraints

- Do not kill by process name.
- Do not expose tokens in config diffs, arguments, logs, or tests.
- Do not replace the installed fork binary before build and handshake verification.
- Do not delete the FarmOS branch before correct-fork acceptance and push.
- Do not treat synthetic acceptance as real-client evidence.

## 7. Discovery plan

- `rg`: client registry, Pi adapter, daemon ownership, installer transactions, version reporting.
- jCodeMunch/codebase graph: callers and related files for client and daemon entrypoints.
- Full reads only for narrowed implementation files and their tests.

## 8. Implementation outline

1. Integrate upstream and preserve fork workflow contracts.
2. Add version/provenance manifest and verified build receipt.
3. Port and product-scope the lifecycle state machine.
4. Add the STDIO bootstrap bridge to the shared HTTP runtime.
5. Extend transactional installers for all three clients.
6. Add contract and runtime tests.
7. Build/install to a staged user scope.
8. Run real-client acceptance and idle cleanup.
9. Commit, push, and then remove the erroneous FarmOS branch.

## 9. Verification plan

- **Primary profile:** repository-native full release gates, equivalent to `quality-all`.
- **Tier 1:** targeted C/Python/PowerShell contract tests, formatting, lint, build.
- **Tier 2:** real MCP handshakes and exact-version/provenance checks.
- **Tier 3:** real Codex, Claude, and Pi sessions in different directories.
- **Tier 4:** fork CI/release-oriented smoke gates as supported locally.
- Screenshot required: no.

## 10. Definition of done

- [ ] Latest upstream is integrated without losing fork tools.
- [ ] Exact fork build is installed and traceable.
- [ ] Latest stable jCodeMunch is pinned and verified.
- [ ] Codex, Claude, and Pi expose both toolsets.
- [ ] Real multi-project acceptance passes.
- [ ] Last-client cleanup leaves zero owned idle processes/listeners.
- [ ] Correct branch is committed and pushed.
- [ ] Erroneous FarmOS branch is removed only after successful cutover.

## 11. Risks / rollback

- **Primary risk:** lifecycle cleanup terminates a live or unrelated process.
- **Secondary risk:** upstream integration drops fork workflow behavior.
- **Rollback:** restore the complete client snapshots, keep the prior verified binaries, and refuse runtime removal while verified leases exist.

## 12. Handoff note

- **Current truth:** installed clients are inconsistent and both engines are outdated relative to their intended sources.
- **Preserved contract:** both engines remain available; no FarmOS product changes.
- **Verification run:** read-only version, config, process, remote, and graph audit completed.
- **Remaining risk:** active clients still own legacy processes and require controlled restart after cutover.
- **Safe next step:** create the detailed implementation plan and execute it on the correct branch.
- **Unsafe next step:** delete the FarmOS branch or kill matching process names before acceptance.
