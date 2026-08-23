# jCodeMunch hybrid companion

This directory is the maintained fork's Windows integration boundary between
codebase-memory-mcp and jCodeMunch. It does not merge the two engines or replace
the native CBM daemon.

## Shipped composition

- codebase-memory-mcp: the exact native binary built from the fork commit
  recorded in the installation receipt;
- jCodeMunch: `jcodemunch-mcp==1.108.291`, installed from the wheel and SHA-256
  pinned in `versions.json`; its complete CPython 3.13/Windows x64 dependency
  closure is installed with `--require-hashes` from `runtime-requirements.lock`;
- STDIO proxy: `mcp-remote==0.1.43`, installed with `npm ci` from the committed
  lockfile and forced to `http-only` transport;
- Pi adapter: the integrity-verified local `pi-mcp-extension==1.5.0` tarball,
  never a registry-resolved package spec, plus the native fork-generated
  `~/.pi/agent/extensions/cbmem.ts` graph adapter.

`stage-runtime.ps1` publishes versioned runtime directories only after their
artifact, package metadata, executable, transport, proxy lock entry, and exact
installed-code file sets verify. A partial three-component composition is never
reused or left published after a staging failure.
Executable scopes reject links/reparse points and generated `.pyc` files; Python
verification and runtime launch disable bytecode writes. An existing composition
is reusable only when its complete file manifests still match the prior
owner-private installation receipt.
The jCodeMunch process is launched with the relocatable venv Python entrypoint,
not pip's absolute-path Windows console launcher.

## Lifecycle boundary

Codex, Claude Code, and Pi each launch `jcodemunch-bridge.ps1` as an MCP STDIO
server. The bridge acquires a workspace-labelled lease owned by its exact PID
and start time, starts only the staged proxy through `workspace-proxy.mjs`, puts
the bearer secret only in the proxy child's environment, supervises the exact
proxy child and client parent, and releases in `finally`. The workspace proxy
forces every repository/path argument to remain inside that bridge's canonical
working directory and blocks global jCodeMunch tools such as `list_repos` and
`index_repo`, so simultaneous projects cannot bind to the first client's cwd.
Existing ancestors are realpath-resolved so symlink and junction escapes are
rejected too.

`jcodemunch-lifecycle.ps1` maintains owner-private state under:

```text
%LOCALAPPDATA%\CodebaseMemoryMcp\companions\jcodemunch
```

It selects an available non-dynamic loopback port, validates the complete owner
tuple before reuse or stop, reaps only disproven leases, and never enumerates or
kills processes by name. The jCodeMunch cache TTL is not overridden; upstream's
default `0` remains in force.

## Development verification

```powershell
python -m pytest tests/hybrid_runtime -q

$stage = Join-Path $env:TEMP ("cbm-hybrid-stage-" + [guid]::NewGuid().ToString("N"))
pwsh -NoProfile -File integrations/jcodemunch/stage-runtime.ps1 -DestinationRoot $stage
pwsh -NoProfile -File integrations/jcodemunch/verify-runtime.ps1 -RuntimeRoot $stage
```

Build and attest the native candidate in one fixed operation from a completely
clean tree (including no untracked files), then perform the user-scoped cutover:

```powershell
$provenance = Join-Path $env:TEMP 'codebase-memory-mcp-candidate-provenance.json'
$attestation = pwsh -NoProfile -File integrations/jcodemunch/write-candidate-provenance.ps1 `
  -SourceRoot $PWD -OutputPath $provenance | ConvertFrom-Json
pwsh -NoProfile -File integrations/jcodemunch/install-hybrid.ps1 `
  -CandidateBinary $attestation.candidate.path -CandidateProvenance $provenance `
  -ClientHome $env:USERPROFILE
```

One owner-only exclusive lock covers planning through receipt or verified
rollback, so concurrent installers cannot overwrite one another's journal. The
installer stages dependencies and snapshots the binary, all native plan
targets, all five client configuration targets, every installed integration
asset, and the previous receipt in one journal. It validates candidate
TOML/JSON, publishes the exact fork binary through the native activation barrier
(`--binary-only`), configures `claude,codex,pi`, performs a real
MCP initialize handshake, verifies all three companion pins, and writes the
non-secret receipt last. Installed-code manifest hashes are anchored in that
receipt. A separate manifest anchors every installed bridge, lifecycle, proxy,
configuration helper, and pinned-version asset. Both runtime and integration
assets are reverified before every real runtime launch. On failure, exact-owned
runtime shutdown happens while the lifecycle still exists, then rollback
restores exact bytes and Windows security descriptors and removes only runtime
paths recorded as newly published by that transaction. Native rollback is
executed with the same client-home environment captured by the forward
transaction, so alternate test or operator homes cannot redirect restoration.
Only files enumerated from the attested Git tree may enter the integration
directory; ignored/untracked source files are rejected and obsolete installed
assets are removed transactionally. Python starts isolated (`-I -B`) with user
injection variables cleared, and Node children drop `NODE_OPTIONS`/`NODE_PATH`.

See `docs/hybrid-runtime-runbook.md` for status, port reselection, upgrades,
rollback, and cleanup.
