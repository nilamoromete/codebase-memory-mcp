---
name: codebase-memory-refactor
description: >
  ALWAYS invoke this skill when the user asks to refactor, reorganize code,
  split files, simplify structure, or reduce complexity while preserving behavior.
  Do not change external contracts unless the task explicitly requires it.
---

# Refactor Playbook

## Default call

```
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="refactor")
```

## Flow

1. Start with `get_edit_plan(..., task_type="refactor")`.
2. Inspect `get_related_files` before moving logic across file boundaries.
3. Use `get_callers` to preserve boundary contracts.
4. Run `get_tests(paths=[...])` plus broader regression coverage.
5. Run `get_change_risks(paths=[...])` before finalizing larger edits.

## Guardrails

- Keep external interfaces stable unless the task explicitly changes them.
- Prefer incremental cleanup over broad rewrites.
- Use `mode="detailed"` when compact output hides relevant structure.
