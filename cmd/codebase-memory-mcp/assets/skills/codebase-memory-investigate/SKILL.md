---
name: codebase-memory-investigate
description: >
  ALWAYS invoke this skill when the user asks why something fails,
  where a behavior originates, which code path triggers a symptom,
  or to diagnose before editing. Do not start patching before the probable
  root cause is clear.
---

# Investigate Playbook

## Default call

```
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="investigate")
```

## Flow

1. Start with `get_edit_plan(..., task_type="investigate")`.
2. Confirm callers with `get_callers` or `trace_call_path(..., direction="both")`.
3. Use `get_file_context` when you need the raw file-level picture.
4. Use `search_graph` / `get_code_snippet` only after you know the likely path.
5. Edit code only after the probable root cause and blast radius are clear.

## Guardrails

- Do not start with a broad patch when the cause is still unclear.
- Prefer narrowing and evidence gathering first.
- If the issue spans multiple files, finish with `get_change_risks(paths=[...])`.
