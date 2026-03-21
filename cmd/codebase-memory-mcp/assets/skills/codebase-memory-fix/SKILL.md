---
name: codebase-memory-fix
description: >
  ALWAYS invoke this skill when the user asks to fix a bug, patch a regression,
  stop a failure, or make a narrow behavior change with focused validation.
  Do not start with a broad refactor.
---

# Fix Playbook

## Default call

```
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="fix")
```

## Flow

1. Start with `get_edit_plan(..., task_type="fix")`.
2. Inspect inbound callers with `get_callers` if behavior might change.
3. Run `get_tests(paths=[...])` for the touched file set.
4. After multi-file edits, run `get_change_risks(paths=[...])`.

## Guardrails

- Keep the patch as narrow as possible.
- Preserve caller-visible contracts unless the task explicitly changes them.
- Switch to `mode="detailed"` only when the compact plan leaves ambiguity.
