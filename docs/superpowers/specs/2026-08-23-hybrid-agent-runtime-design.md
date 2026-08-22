# Hybrid Agent Runtime Design

Status: approved for implementation on 2026-08-23

Target repository: `https://github.com/nilamoromete/codebase-memory-mcp`

Target branch: `feat/hybrid-agent-runtime`

## Purpose

The maintained fork is the product boundary for a hybrid code-intelligence workflow. Codex, Claude Code, and Pi must all receive the fork's evidence-backed codebase-memory tools and jCodeMunch's symbol-oriented tools. The two engines remain independently versioned, but installation, client configuration, lifecycle, provenance, and acceptance are owned by this fork.

The design corrects an earlier implementation that was committed to the FarmOS repository and that configured a direct HTTP endpoint which could be unavailable when a client started. No FarmOS application behavior belongs in this change.

## Current evidence

- The installed `codebase-memory-mcp.exe` reports `dev`, was written on 2026-03-21, and predates the current fork branch.
- `feat/evidence-workflow-tools` contains 44 fork commits and is 11 upstream commits behind the fetched `upstream/main` baseline at `010569fa`.
- Those upstream commits include the Pi adapter fixes merged on 2026-08-23.
- The globally installed jCodeMunch is `0.2.22`; the latest stable PyPI and GitHub release observed on 2026-08-23 is `1.108.291`.
- Codex currently has both MCP entries, but its on-disk jCodeMunch entry points at an inactive HTTP endpoint.
- Claude has only the inactive jCodeMunch HTTP entry.
- Pi has jCodeMunch through `pi-mcp-extension`, but no installed codebase-memory adapter.
- Five active Codex hosts own five legacy jCodeMunch STDIO children and five codebase-memory frontends. These processes must not be terminated while their host sessions are active.

## Goals

1. Build and install one traceable executable from the latest fork commit after integrating the latest upstream main.
2. Configure Codex, Claude Code, and Pi to expose both codebase-memory and jCodeMunch tools.
3. Pin the latest verified stable jCodeMunch release at installation time; do not silently follow prereleases.
4. Share one heavy jCodeMunch HTTP runtime across concurrent clients and projects.
5. Start that runtime before each client's MCP handshake and stop it after the final live client detaches.
6. Recover from crashed clients, PID reuse, port conflicts, partial configuration writes, and failed runtime starts.
7. Preserve existing indexes and unrelated client configuration.
8. Produce real three-client acceptance evidence and leave zero idle jCodeMunch runtime, bridge, monitor, or listener processes.

## Non-goals

- Vendoring or copying jCodeMunch source into the codebase-memory binary.
- Changing FarmOS product code or business behavior.
- Automatically installing prerelease versions.
- Killing processes merely because their executable name matches.
- Treating a synthetic harness as proof that Codex, Claude Code, and Pi work.
- Replacing the native codebase-memory coordination daemon.

## Architecture

### 1. Fork provenance and installation

Implementation starts from `feat/evidence-workflow-tools` and integrates the fetched `upstream/main`. Fork functionality must survive that integration. The Windows build is produced from the exact resulting commit and installed user-scoped through a staged, atomic replacement.

An installation receipt records:

- fork URL and branch;
- fork commit and integrated upstream commit;
- binary SHA-256 and reported version;
- jCodeMunch version;
- bridge version;
- install timestamp and architecture;
- client configuration snapshot identifiers.

The executable path used by clients is stable, while the receipt proves which source commit supplied it. A failed build or verification never replaces the active executable.

### 2. Native codebase-memory plane

The fork keeps its native per-account coordination daemon and thin-client behavior.

- Codex uses the installed `codebase-memory-mcp.exe` STDIO server.
- Claude Code uses the same installed executable through its supported MCP configuration.
- Pi uses the generated `cbmem.ts` adapter from the integrated upstream implementation. The adapter calls the same installed executable and exposes the supported tool schema for Pi 0.84.2.

The installer preserves unrelated MCP servers, hooks, skills, agents, and project-specific configuration.

### 3. Shared jCodeMunch companion plane

jCodeMunch runs once as a loopback-only streamable HTTP server. Clients do not connect directly to a possibly stopped URL. Instead, each client launches a small STDIO bridge installed by this fork.

The bridge performs this sequence before forwarding the MCP handshake:

1. Validate its client identity, parent PID/start time, and workspace.
2. Acquire a lease under the user-scoped hybrid runtime directory.
3. Under an exclusive owner lock, verify or start the exact pinned jCodeMunch runtime.
4. Verify runtime identity, version, executable provenance, loopback bind, bearer authentication, and MCP readiness.
5. Connect a pinned STDIO-to-streamable-HTTP proxy to the shared endpoint.
6. Forward STDIO until the client closes the bridge.
7. Release the lease and request delayed shutdown when no verified leases remain.

This makes client process creation itself the lifecycle boundary. It does not depend on whether a client's `SessionStart` hook runs before or after MCP initialization.

### 4. Ownership, ports, and authentication

Runtime state is user-scoped and product-scoped, not FarmOS-scoped. It lives under the platform's user application-data directory for codebase-memory companions.

The installer selects an available loopback port transactionally and persists it with the client configuration. Startup never kills an unknown listener. If the recorded port is occupied by an unverifiable process, configuration fails closed and offers a new transactional selection.

A cryptographically random bearer token is stored with user-only permissions. Tokens are never printed, committed, placed in command-line arguments, or copied into logs. Client configuration references an environment/file-backed credential where the client format supports it; the bridge supplies the credential internally otherwise.

Runtime ownership records include PID plus process start time, executable path/hash, product/version identity, owner nonce, and launch generation. PID-only ownership is invalid.

### 5. Crash and idle recovery

Every bridge supervises its proxy child and watches its launching parent. A stale-lease monitor is elected from the current owner generation. It:

- removes a lease only when PID and start-time identity prove the client is gone;
- preserves leases it cannot verify;
- refuses shutdown while any verified live lease exists;
- stops only the exact runtime generation recorded by the owner state;
- exits after the runtime is stopped and the owner state is removed.

Shutdown has a short configurable grace period to coalesce rapid client restarts. There is no permanent heavy runtime or permanent monitor at idle.

The jCodeMunch index-cache TTL keeps its upstream default of `0` while verified clients are attached. The shared singleton already removes duplicate hydrated caches, and final-client shutdown releases its memory without imposing a surprise rehydration delay on a live client.

### 6. Transactional client cutover

The configure operation snapshots all target files before writing anything. It then generates complete candidate configurations for Codex, Claude, and Pi and validates their syntax before replacement.

The transaction either installs all required surfaces or restores all of them:

- Codex: codebase-memory STDIO plus the jCodeMunch bridge STDIO entry.
- Claude: codebase-memory STDIO plus the jCodeMunch bridge STDIO entry; compatible discovery hooks remain fail-open.
- Pi: generated codebase-memory adapter, jCodeMunch bridge entry for the MCP extension, and deterministic package ordering.

Existing unrelated configuration is preserved byte-for-byte where the active format permits. Secrets are compared by presence and identity, never emitted in test output.

Active clients continue using the configuration they loaded at startup. Cutover is considered complete only after those clients are restarted and the old child processes have exited naturally or have been proven orphaned.

### 7. Rollback and erroneous FarmOS branch

Rollback first restores the pre-cutover client snapshots. It then refuses to remove the companion runtime while verified live leases exist. When idle, it stops the exact owned generation and removes only integration-owned files.

The erroneous FarmOS branch remains available until the correct fork branch is built, installed, accepted, committed, and pushed. It is then removed from the FarmOS remote and its worktree is pruned. Unrelated FarmOS changes and existing stashes are not touched.

## Failure handling

- Version resolution failure: keep the current verified installation and report the unresolved source.
- Upstream integration conflict: resolve only against fork feature contracts and run the affected contract tests before continuing.
- Build or binary verification failure: do not replace the installed executable.
- jCodeMunch bootstrap failure: terminate only descendants created by that launch attempt and remove only its uncommitted state.
- Port conflict: fail closed; never terminate an unknown listener.
- Partial client configuration failure: restore the complete snapshot set.
- Client crash: parent watch and stale-lease reconciliation release only proven-dead identities.
- Rollback with live clients: refuse and report the exact verified owners.

## Verification

### Code-local gates

- Existing codebase-memory daemon, installer, client-adapter, and MCP suites.
- Fork evidence-workflow contract and benchmark gates.
- Pi adapter tests from the integrated upstream baseline.
- New lifecycle contract, configuration-sandbox, runtime, Windows identity, and rollback tests.
- Secret and token disclosure scan.

### Runtime gates

- Built binary reports the expected fork commit provenance and passes a real MCP handshake.
- jCodeMunch reports the pinned stable version and a real streamable-HTTP handshake through the STDIO bridge.
- Existing indexes remain listable and queryable through both engines.
- Two or more clients in different projects share one heavy jCodeMunch runtime without cross-project resolution errors.

### Real-client acceptance

Launch fresh, isolated Codex, Claude Code, and Pi sessions in three different project directories. For each client:

1. Invoke a codebase-memory discovery tool.
2. Invoke a jCodeMunch discovery tool.
3. Confirm tool output belongs to the selected project.
4. Confirm the installed fork and companion version receipts.

While all clients are open, assert one jCodeMunch runtime and three live bridge leases. Close clients one at a time and prove the runtime remains while a lease exists. After the last client closes and the grace period elapses, assert:

- zero jCodeMunch runtime processes owned by the integration;
- zero bridge/proxy/monitor processes;
- zero listener on the selected port;
- zero live leases and no owner state;
- native codebase-memory daemon/frontend behavior matches its existing contract.

## Preserved contracts

- The fork's evidence-backed edit planning and change-risk tools remain available.
- Native codebase-memory daemon admission and last-client shutdown semantics remain unchanged.
- jCodeMunch indexes and repository identifiers remain compatible.
- Codex, Claude, and Pi retain unrelated configuration.
- No FarmOS business or application contract changes.

## Risks

The integration touches installer/configuration code and process lifecycle, both high-risk surfaces. The largest risks are an upstream merge conflict that drops fork functionality, a proxy bootstrap race, accidental secret disclosure, and misidentifying an active process as stale. The design mitigates them with exact identity records, transactional writes, fail-closed ownership, real-client acceptance, and delayed cleanup of the erroneous branch.
