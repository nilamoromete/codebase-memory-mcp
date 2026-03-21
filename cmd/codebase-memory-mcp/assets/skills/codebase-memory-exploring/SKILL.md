---
name: codebase-memory-exploring
description: >
  This skill should be used when the user asks to "explore the codebase",
  "understand the architecture", "what functions exist", "show me the structure",
  "how is the code organized", "find functions matching", "search for classes",
  "list all routes", "show API endpoints", or needs codebase orientation.
---

# Codebase Exploration via Knowledge Graph

Use the high-level context tools first. Fall back to graph primitives only when you need more detail.

## Workflow

### Step 1: Check if project is indexed

```
list_projects
```

If the project is missing from the list:

```
index_repository(repo_path="/path/to/project")
```

If already indexed, skip — auto-sync keeps the graph fresh.

### Step 2: Start with a pre-edit plan

```
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="fix")
```

Use this before editing. It combines file context and change-risk guidance into one response.

Choose task type by intent:

```
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="refactor")
get_edit_plan(path="src/path/to/file.ext", mode="compact", task_type="investigate")
```

If you need more granular detail for the same file:

```
get_file_context(path="src/path/to/file.ext")
```

If you want the full composed payload instead of the compact default:

```
get_edit_plan(path="src/path/to/file.ext", mode="detailed")
```

### Step 3: Expand local blast radius

```
get_related_files(path="src/path/to/file.ext")
```

### Step 4: Get a structural overview

```
get_graph_schema
```

This returns node label counts (functions, classes, routes, etc.), edge type counts, and relationship patterns. Use it to understand what's in the graph before querying.

### Step 5: Find specific code elements

Find functions by name pattern:
```
search_graph(label="Function", name_pattern=".*Handler.*")
```

Find classes:
```
search_graph(label="Class", name_pattern=".*Service.*")
```

Find all REST routes:
```
search_graph(label="Route")
```

Find modules/packages:
```
search_graph(label="Module")
```

Scope to a specific directory:
```
search_graph(label="Function", qn_pattern=".*services\\.order\\..*")
```

### Step 6: Read source code

After finding a function via search, read its source:
```
get_code_snippet(qualified_name="project.path.to.FunctionName")
```

## When to Use Grep Instead

- Searching for **string literals** or error messages → `search_code` or Grep
- Finding a file by exact name → Glob
- The graph doesn't index text content, only structural elements

## Key Tips

- Use `get_tests(paths=[...])` before declaring an edit safe.
- Use `get_change_risks(paths=[...])` after multi-file edits.
- Results default to 10 per page. Check `has_more` and use `offset` to paginate.
- Use `project` parameter when multiple repos are indexed.
- Route nodes have a `properties.handler` field with the actual handler function name.
- `exclude_labels` removes noise (e.g., `exclude_labels=["Route"]` when searching by name pattern).
