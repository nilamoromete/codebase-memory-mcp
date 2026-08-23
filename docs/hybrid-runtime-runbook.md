# Hybrid runtime operations runbook

This runbook covers the maintained fork's Windows composition: the native
codebase-memory-mcp graph engine plus one pinned jCodeMunch companion shared by
Codex, Claude Code, and Pi.

## State and receipt

The default state root is:

```text
%LOCALAPPDATA%\CodebaseMemoryMcp\companions\jcodemunch
```

`install-receipt.json` records the fork URL, branch and commit, integrated
upstream commit, candidate/installed binary SHA-256 and version, companion pins,
installed-code manifest hashes, architecture, timestamp, config hashes, and
the exact integration-asset manifest hash/file count. It never
contains the bearer token. `secrets\token.json`, state documents, backups,
logs, and the receipt receive current-owner access on Windows.

Inspect the receipt without displaying secrets:

```powershell
$root = Join-Path $env:LOCALAPPDATA 'CodebaseMemoryMcp\companions\jcodemunch'
Get-Content (Join-Path $root 'install-receipt.json') -Raw | ConvertFrom-Json |
  Select-Object product,fork,binary,jcodemunch,stdio_proxy,installed_at
```

## Status

```powershell
$root = Join-Path $env:LOCALAPPDATA 'CodebaseMemoryMcp\companions\jcodemunch'
pwsh -NoProfile -File (Join-Path $root 'integration\jcodemunch-lifecycle.ps1') `
  -Command status -StateRoot $root -ConfigRoot $root -Json
```

Healthy status means the listener is loopback-only on `127.0.0.1` and the recorded PID, start
time, executable path/hash, version, command fingerprint, launch/runtime
identity, launch ID, and port all validate. A configured but idle runtime may
legitimately have no owner after the grace shutdown.

## Multiple clients and projects

Every bridge lease includes client, session, canonical workspace root,
workspace hash, bridge PID, and bridge start time. Codex, Claude, and Pi may work
in different repositories simultaneously: they share the heavy HTTP server,
but each STDIO workspace proxy rewrites repository/path arguments to its own
canonical root and rejects lexical, symlink, and junction traversal outside it.
Global companion operations
that could enumerate or mutate another repository (`list_repos`, `index_repo`,
session snapshots, and cache invalidation) are intentionally unavailable through
the shared bridge. Closing one client removes only its lease. The runtime remains
while any live, verifiable lease exists.

After the final bridge exits, a generation-bound grace watcher stops only the
exact owned runtime. A new acquire during grace cancels that generation, so a
stale watcher cannot stop a newly active server.

## Port collision and explicit reselection

No common port is hard-coded. Configuration rejects active, excluded, reserved,
or OS dynamic-range candidates and never stops an unknown listener. If the
persisted port becomes foreign, close all three clients and run a full
transactional reselection so every config stays consistent:

```powershell
$root = Join-Path $env:LOCALAPPDATA 'CodebaseMemoryMcp\companions\jcodemunch'
$lifecycle = Join-Path $root 'integration\jcodemunch-lifecycle.ps1'
pwsh -NoProfile -File $lifecycle -Command configure -StateRoot $root `
  -ConfigRoot $root -ReselectPort -ApplyClientConfigs `
  -CodexConfig (Join-Path $env:USERPROFILE '.codex\config.toml') `
  -ClaudeConfig (Join-Path $env:USERPROFILE '.claude.json') `
  -ClaudeSettingsConfig (Join-Path $env:USERPROFILE '.claude\settings.json') `
  -PiConfig (Join-Path $env:USERPROFILE '.pi\agent\mcp.json') `
  -PiSettingsConfig (Join-Path $env:USERPROFILE '.pi\agent\settings.json') `
  -IntegrationRoot (Join-Path $root 'integration') -Json
```

Restart Codex, Claude Code, and Pi after reselection.

## Upgrade

Fetch and merge the desired upstream baseline into the maintained fork, run the
full native and hybrid gates, and commit every source file. The provenance
writer then starts the fixed canonical `scripts/build.sh` entrypoint itself,
passes the full commit as `--version`, uses an isolated attested build directory,
and refuses pre-build dirt, post-build dirt, source identity drift, or a binary
whose `--version` lacks that commit.
Invoke
`install-hybrid.ps1` with the candidate and provenance file. Versions are
published side-by-side; an existing live version is never overwritten. The
receipt is the commit point. Restart already-open clients after the installer
succeeds; they are intentionally not killed during cutover.
Reuse is fail-closed: all three versioned components must exist, their complete
file sets must match the prior receipt, and no executable scope may contain a
link/reparse point or generated Python bytecode. Every installed integration
asset is similarly inventoried and anchored before it can locate or execute the
runtime.
The runtime dependency closure is hash-locked for CPython 3.13 on Windows x64.
Integration assets come only from the attested Git tree; ignored files are
rejected and obsolete installed files are transactionally removed on upgrade.

## Exact-owner cleanup

Normal cleanup is automatic. For an idle verified runtime:

```powershell
pwsh -NoProfile -File $lifecycle -Command clean -StateRoot $root `
  -ConfigRoot $root -Now -Json
```

Cleanup refuses if the owner tuple is incomplete or changed. Do not use
`Stop-Process -Name`, Task Manager bulk termination, or `taskkill /IM`; those
can kill another client's or user's process. Inspect the failed ownership checks
and resolve the recorded state explicitly.

## Rollback

First close/restart the affected clients so no bridge lease remains, then run:

```powershell
pwsh -NoProfile -File (Join-Path $root 'integration\rollback-hybrid.ps1') `
  -StateRoot $root -ConfigRoot $root
```

Rollback refuses live leases, stops the identity-validated runtime while the
installed lifecycle still exists, then restores or removes the journaled binary
through the same native activation barrier used by forward installation, restores native
agent assets, client configuration, integration files, receipt, exact bytes,
hashes, empty legacy directories, and Windows security descriptors. It removes only
runtime paths explicitly recorded as newly published by that transaction. It
reuses the captured forward-install client-home environment for the native
activation barrier, holds the same exclusive install lock throughout, and refuses unverifiable
ownership. Restart all three clients after
rollback.

## Verification after cutover

1. Compare the installed binary SHA-256 and `--version` with the receipt.
2. Run a real MCP `initialize` and `tools/list` against the installed binary.
3. Verify `get_edit_plan` and `get_change_risks` exist, and Pi has
   `~/.pi/agent/extensions/cbmem.ts`.
4. In fresh Codex, Claude, and Pi sessions, invoke one CBM tool and one
   jCodeMunch tool in separate disposable repositories.
5. While all are live, verify one owner and three exact leases. After the final
   exit and grace period, verify no owner, live lease, monitor, watcher, proxy,
   bridge, or listener remains.
