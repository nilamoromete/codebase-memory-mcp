# Hybrid Agent Runtime Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Ship and install the maintained fork as a traceable hybrid code-intelligence product in which Codex, Claude Code, and Pi use the exact fork build plus one pinned, shared, lifecycle-safe jCodeMunch runtime.

**Architecture:** Keep codebase-memory-mcp's native per-account daemon unchanged and integrate current upstream so all three clients receive its latest adapters, including Pi. Add a fork-owned Windows companion under `integrations/jcodemunch/`: every client launches a lightweight STDIO bridge, the bridge acquires a PID/start-time lease, verifies or starts one authenticated loopback jCodeMunch HTTP runtime, runs a pinned STDIO-to-HTTP proxy, and releases on exit. A transactional installer stages verified dependencies and edits all client surfaces as one recoverable cutover.

**Tech Stack:** C11/C++17 native codebase-memory-mcp, PowerShell 7.2+, Python 3.10+, jCodeMunch `1.108.291`, Node.js plus `mcp-remote` `0.1.43`, pytest, the repository C test runner, GitHub CLI/git.

---

## Task 1: Integrate the current upstream baseline without losing fork contracts

**Files:**
- Modify through merge: `src/cli/cli.c`, `src/mcp/mcp.c`, related upstream files
- Verify: `tests/test_cli.c`
- Verify: `tests/test_agent_clients.c`
- Verify: `tests/test_daemon_runtime.c`
- Verify: `tests/test_daemon_frontend.c`
- Verify: `tests/test_workflow_tools.c`

**Step 1: Record the exact inputs and prove the branch is clean**

Run:

```powershell
git status --short --branch
git rev-parse HEAD
git rev-parse upstream/main
git log --oneline --left-right HEAD...upstream/main
```

Expected: `feat/hybrid-agent-runtime` tracks the fork branch, the worktree is clean, and upstream resolves to the fetched baseline or newer explicitly recorded commit.

**Step 2: Merge upstream with a real merge commit**

Run:

```powershell
git merge --no-ff upstream/main
```

Expected: either a completed merge or only explicit conflicts in overlapping fork feature files. Preserve the fork's workflow tools and the upstream client/daemon fixes; do not solve conflicts by dropping either contract wholesale.

**Step 3: Run the narrow native regression suites**

Run from a shell that provides the repository build prerequisites:

```bash
scripts/test.sh --suites "cli agent_clients daemon_runtime daemon_frontend workflow_tools"
```

Expected: all selected suites pass. If `workflow_tools` is named differently, use `build/c/test-runner --list-suites` and select the exact fork suite.

**Step 4: Commit and push the integration checkpoint**

```powershell
git add -A
git diff --cached --check
git commit -m "merge: integrate upstream runtime and Pi fixes"
git push fork feat/hybrid-agent-runtime
```

## Task 2: Port the lifecycle state machine into the correct product scope

**Files:**
- Create: `integrations/jcodemunch/JCodeMunchLifecycle.psm1`
- Create: `integrations/jcodemunch/jcodemunch-lifecycle.ps1`
- Create: `integrations/jcodemunch/watch-leases.ps1`
- Create: `integrations/jcodemunch/watch-shutdown.ps1`
- Create: `tests/hybrid_runtime/test_lifecycle_contract.py`
- Create: `tests/hybrid_runtime/test_lifecycle_runtime.py`
- Create: `tests/hybrid_runtime/fixtures/fake_jcodemunch.py`
- Create: `tests/hybrid_runtime/fixtures/fake_jcodemunch_launcher.ps1`

**Step 1: Write failing product-scope and ownership tests**

Cover these contracts:

- default state root is `%LOCALAPPDATA%\\CodebaseMemoryMcp\\companions\\jcodemunch`, never `Aliat` or `FarmOS`;
- state and secret files receive owner-only permissions;
- owner identity includes PID, process start time, executable path/hash, command fingerprint, runtime identity, launch ID, version, and port;
- PID-only, stale PID, hash mismatch, unknown listener, and corrupt state fail closed;
- stale leases are removed only with disproven PID/start-time identity;
- last release arms a generation-bound grace shutdown;
- cleanup never kills by executable name.

Run:

```powershell
python -m pytest tests/hybrid_runtime/test_lifecycle_contract.py tests/hybrid_runtime/test_lifecycle_runtime.py -q
```

Expected: RED because the correct-repository lifecycle files do not yet exist.

**Step 2: Port the proven primitives and change the product boundary**

Port the already-tested FarmOS feature implementation as reference, then make these deliberate changes:

- product-scoped default root;
- exact executable SHA-256 and runtime version in owner validation;
- bridge PID/start-time as the lease owner;
- no client-specific hook as the required lifecycle boundary;
- token redaction before every diagnostic write;
- runtime cache TTL left at jCodeMunch's upstream default `0`;
- no process-name enumeration or termination.

**Step 3: Make lifecycle tests green**

Run:

```powershell
python -m pytest tests/hybrid_runtime/test_lifecycle_contract.py tests/hybrid_runtime/test_lifecycle_runtime.py -q
```

Expected: PASS, including forced PID reuse, bootstrap rollback, stale lease reconciliation, grace cancellation, and exact-generation shutdown.

**Step 4: Commit**

```powershell
git add integrations/jcodemunch tests/hybrid_runtime
git diff --cached --check
git commit -m "feat: add product-scoped jcodemunch lifecycle"
```

## Task 3: Pin, stage, and verify the companion runtime and STDIO proxy

**Files:**
- Create: `integrations/jcodemunch/versions.json`
- Create: `integrations/jcodemunch/stage-runtime.ps1`
- Create: `integrations/jcodemunch/verify-runtime.ps1`
- Create: `tests/hybrid_runtime/test_runtime_staging.py`

**Step 1: Write failing manifest and supply-chain tests**

Assert exact pins and immutable registry metadata:

- `jcodemunch-mcp==1.108.291`;
- wheel `jcodemunch_mcp-1.108.291-py3-none-any.whl`;
- wheel SHA-256 `41a25402cb755daa55be060d1e8d65183f8868ac3c0b201079d3fb64d8e09f5d`;
- `mcp-remote==0.1.43`;
- npm integrity `sha512-N2pGvTAPlHSH64iftgaVsR9J2+QUgCkTbaFb8XMxRbOFvJTCFONJOUNOxfnxPc0FUKyWUE/jpdhabfBI3g/gPw==`;
- loopback-only Streamable HTTP and `http-only` proxy transport;
- stage into side-by-side version directories without replacing a live version.

Run:

```powershell
python -m pytest tests/hybrid_runtime/test_runtime_staging.py -q
```

Expected: RED.

**Step 2: Implement verified side-by-side staging**

`stage-runtime.ps1` must download to a private temporary location, verify hash/integrity before install, create a versioned Python venv and a versioned npm prefix, verify both generated entrypoints, then atomically publish the completed directory. A failed stage leaves the currently active runtime untouched.

**Step 3: Run staging tests and a real disposable stage**

```powershell
python -m pytest tests/hybrid_runtime/test_runtime_staging.py -q
$stage = Join-Path $env:TEMP ("cbm-hybrid-stage-" + [guid]::NewGuid().ToString('N'))
pwsh -NoProfile -File integrations/jcodemunch/stage-runtime.ps1 -DestinationRoot $stage
pwsh -NoProfile -File integrations/jcodemunch/verify-runtime.ps1 -RuntimeRoot $stage
```

Expected: tests pass and verification emits only non-secret version, path, transport, and digest data.

**Step 4: Commit**

```powershell
git add integrations/jcodemunch tests/hybrid_runtime/test_runtime_staging.py
git diff --cached --check
git commit -m "feat: pin and verify hybrid companion runtime"
```

## Task 4: Add the bridge that owns each client lease

**Files:**
- Create: `integrations/jcodemunch/jcodemunch-bridge.ps1`
- Create: `tests/hybrid_runtime/test_stdio_bridge.py`
- Modify: `tests/hybrid_runtime/fixtures/fake_jcodemunch.py`

**Step 1: Write failing bridge tests**

Exercise the bridge as an actual STDIO MCP subprocess and assert:

- runtime is ready before the client `initialize` response;
- `mcp-remote` runs with `--transport http-only` and receives the bearer token only through a process-scoped environment variable;
- workspace and client labels become one lease owned by the bridge's exact PID/start time;
- proxy exit, stdin EOF, Ctrl-C, and parent death all attempt release in `finally`;
- three bridges share one runtime owner but have three leases;
- a bridge in a second repository receives that repository's jCodeMunch project context;
- after the final bridge exits and the short test grace expires, no owned runtime, proxy, listener, monitor, lease, or owner record remains.

Run:

```powershell
python -m pytest tests/hybrid_runtime/test_stdio_bridge.py -q
```

Expected: RED.

**Step 2: Implement the bridge**

The bridge must acquire in-process through `jcodemunch-lifecycle.ps1`, resolve the persisted endpoint only after ownership validation, launch the pinned proxy with inherited STDIO, supervise the exact proxy child, release in `finally`, and keep stdout exclusively for MCP JSON-RPC.

Do not pass the bearer token in argv. Do not use `npx` network resolution at client startup; invoke only the staged, pinned proxy entrypoint.

**Step 3: Make bridge tests green**

```powershell
python -m pytest tests/hybrid_runtime/test_stdio_bridge.py -q
```

Expected: PASS on Windows with no orphaned fixture processes.

**Step 4: Commit**

```powershell
git add integrations/jcodemunch/jcodemunch-bridge.ps1 tests/hybrid_runtime
git diff --cached --check
git commit -m "feat: bridge stdio clients to shared jcodemunch"
```

## Task 5: Add transactional Codex, Claude Code, and Pi installation

**Files:**
- Create: `integrations/jcodemunch/install-hybrid.ps1`
- Create: `integrations/jcodemunch/rollback-hybrid.ps1`
- Create: `integrations/jcodemunch/write-install-receipt.ps1`
- Create: `tests/hybrid_runtime/test_client_configs.py`
- Create: `tests/hybrid_runtime/fixtures/config_sandboxes.py`
- Modify: `integrations/jcodemunch/jcodemunch-lifecycle.ps1`

**Step 1: Write failing configuration-sandbox tests**

For empty, normal, non-ASCII, conflicting, and malformed fixtures, assert:

- Codex receives `codebase-memory-mcp` STDIO plus `jcodemunch` bridge STDIO;
- Claude receives both entries and keeps unrelated servers/hooks;
- Pi receives upstream-generated `cbmem.ts`, the jCodeMunch bridge through its MCP extension, and deterministic packages;
- existing unrelated bytes/objects survive the merge;
- every target is snapshotted before the first write;
- candidate TOML/JSON/TypeScript assets validate before atomic replacement;
- injected failure after every write position restores all target bytes and relevant environment values;
- no token value appears in configs, logs, receipts, diffs, or test output.

Run:

```powershell
python -m pytest tests/hybrid_runtime/test_client_configs.py -q
```

Expected: RED.

**Step 2: Implement one idempotent cutover command**

The installer accepts explicit config roots for tests and defaults to the current user's Codex, Claude, and Pi roots. It snapshots first, invokes the verified fork candidate's native install into the same user scope, stages the companion, installs bridge assets, writes all three jCodeMunch STDIO entries, validates, and commits the receipt last.

The receipt records fork URL/branch/commit, integrated upstream commit, binary SHA-256/version, jCodeMunch and proxy pins, architecture, timestamp, and snapshot transaction ID. It never records the bearer token.

**Step 3: Implement rollback**

Rollback restores snapshots byte-for-byte first, refuses removal while any lease is live or unverifiable, then removes only receipt-owned assets when the exact runtime is idle.

**Step 4: Make sandbox tests green**

```powershell
python -m pytest tests/hybrid_runtime/test_client_configs.py -q
```

Expected: PASS for all fixtures and failure injection points.

**Step 5: Commit and push the implementation checkpoint**

```powershell
git add integrations/jcodemunch tests/hybrid_runtime
git diff --cached --check
git commit -m "feat: install hybrid runtime for codex claude and pi"
git push fork feat/hybrid-agent-runtime
```

## Task 6: Document operations and close automated quality gates

**Files:**
- Create: `docs/hybrid-runtime-runbook.md`
- Create: `integrations/jcodemunch/README.md`
- Create: `tests/hybrid_runtime/test_documentation_contract.py`
- Modify: `README.md`

**Step 1: Add failing documentation contract tests**

Require the runbook to document status, upgrade, collision-safe port reselection, multi-project behavior, graceful shutdown, exact-owner cleanup, rollback, receipt inspection, and the mandatory client restart after cutover.

**Step 2: Write the runbook and product README section**

Make the fork's hybrid boundary explicit: two independently versioned engines, one fork-owned installer and lifecycle contract.

**Step 3: Run all hybrid and targeted native gates**

```powershell
python -m pytest tests/hybrid_runtime -q
```

```bash
scripts/test.sh --suites "cli agent_clients daemon_runtime daemon_frontend workflow_tools"
```

Expected: PASS.

**Step 4: Run change-risk inspection and commit**

Use `get_change_risks` for all modified implementation paths, resolve any untested high-risk edge, then:

```powershell
git add README.md docs integrations tests/hybrid_runtime
git diff --cached --check
git commit -m "docs: add hybrid runtime operations and verification"
```

## Task 7: Build, verify, and install the exact fork commit

**Files:**
- Generated: `build/c/codebase-memory-mcp.exe`
- User-scoped installed binary and assets
- Generated user-scoped installation receipt

**Step 1: Build the shipped composition from a clean native build**

```bash
scripts/build.sh --with-ui
```

Expected: `build/c/codebase-memory-mcp.exe` exists and the build exits zero.

**Step 2: Verify exact candidate provenance before activation**

Record `git rev-parse HEAD`, candidate SHA-256, `--version`, `install --help`, and a real STDIO MCP initialize/tools-list handshake. Confirm `get_edit_plan`, `get_change_risks`, and the upstream Pi adapter assets exist.

**Step 3: Run the transactional hybrid installer**

```powershell
pwsh -NoProfile -File integrations/jcodemunch/install-hybrid.ps1 `
  -CandidateBinary build/c/codebase-memory-mcp.exe `
  -ForkUrl https://github.com/nilamoromete/codebase-memory-mcp `
  -ForkBranch feat/hybrid-agent-runtime `
  -ForkCommit (git rev-parse HEAD) `
  -UpstreamCommit (git rev-parse upstream/main)
```

Expected: one committed receipt, both engines configured for all detected target clients, and a message requiring restart of already-open clients. Existing active sessions are not killed.

**Step 4: Verify the installed bytes**

Compare installed SHA-256 to the verified candidate and run its real MCP handshake. Verify the staged jCodeMunch and proxy versions without printing secrets.

## Task 8: Run real three-client, multi-project, and cleanup acceptance

**Files:**
- Create: `tests/hybrid_runtime/real_client_acceptance.ps1`
- Generated evidence: user-scoped redacted acceptance report referenced by the receipt

**Step 1: Prepare three disposable repositories and isolated logs**

Create three temporary git projects with unique symbols. Select/probe the runtime port through the lifecycle manager; do not hard-code a commonly used port.

**Step 2: Launch fresh client processes**

Launch current `codex`, `claude`, and `pi` processes in separate projects using their real installed configuration. Keep them non-interactive where supported and hide background helper windows.

**Step 3: Prove both toolsets per client**

For every client, invoke one codebase-memory discovery tool and one jCodeMunch discovery tool, and assert each response names only the selected temporary project. Record client versions, fork receipt identity, and companion versions with secrets redacted.

**Step 4: Prove singleton and lease behavior**

While all clients are alive, assert one verified heavy jCodeMunch runtime and three bridge leases. Close one client at a time; the runtime must remain while any verified lease exists.

**Step 5: Prove final cleanup**

After the last client closes and the grace expires, assert zero integration-owned jCodeMunch runtime, bridge, proxy, watcher, monitor, and port listeners, plus no live lease or owner record. Assert no unrelated same-name or same-port process was touched.

**Step 6: Run the full repository gate**

```bash
scripts/test.sh
```

Expected: the complete local CI-equivalent suite passes. If an external-only release gate cannot run locally, record it as a follow-up issue rather than claiming it passed.

## Task 9: Land the correct fork and remove only the erroneous FarmOS artifacts

**Files:**
- Update: `docs/plans/2026-08-23-hybrid-agent-runtime-task-packet.md`
- Update: issue records through `bd`

**Step 1: Record acceptance in the task packet and close completed issues**

Update affected module, preserved contract, exact verification path, and remaining risk. Close `aliat-53wh` and `aliat-c551` only if their acceptance conditions are proven; file issues for any external-only follow-up.

**Step 2: Complete the mandatory fork push workflow**

```powershell
git pull --rebase --autostash fork feat/hybrid-agent-runtime
bd sync
git push fork feat/hybrid-agent-runtime
git status --short --branch
```

Expected: clean and up to date with `fork/feat/hybrid-agent-runtime`.

**Step 3: Verify the exact erroneous FarmOS targets before deletion**

Confirm all of the following again:

- worktree: `C:\\opencoder\\aliat\\.worktrees\\jcodemunch-shared-lifecycle`;
- branch: `feat/jcodemunch-shared-lifecycle`;
- remote repo: `https://github.com/nilamoromete/farmos.git`;
- commit: the erroneous implementation is not merged into FarmOS main;
- worktree is clean;
- unrelated FarmOS working files and stashes are outside the target.

**Step 4: Remove the verified FarmOS feature branch and worktree**

Remove the clean linked worktree with `git worktree remove` from the FarmOS root, delete the exact local branch, then delete only `origin/feat/jcodemunch-shared-lifecycle`. Prune worktree metadata and fetch/prune remote refs. Do not clear stashes or touch unrelated FarmOS changes.

**Step 5: Final verification and handoff**

Re-run fork `git status`, remote branch lookup, installed receipts, client MCP listings, lifecycle status, and process/listener inspection. The handoff must state:

- affected module: shared agent tooling;
- preserved contracts: fork workflow tools, native CBM daemon, indexes, unrelated configs, FarmOS behavior;
- verification actually run;
- any remaining risk or required restart.

