# Proposal: `now execute` — Graph-Aware Task Runner

**Status**: Proposal (not yet implemented)
**Date**: 2026-03-07

---

## Summary

Extend `now` with a task execution system that runs arbitrary commands/scripts
as graph-aware nodes. Tasks are described in Pasta format ("pastlets") and can
declare dependencies on build phases, other tasks, or specific module builds.

---

## Motivation

Build tools frequently need to run project-specific scripts (key generation,
database migrations, code formatting, deployment) that are not compilation but
still depend on build artifacts or must run in a specific order. Today these
live in Makefiles or shell scripts outside `now`'s dependency graph.

`now execute` brings these into the same DAG, giving them:
- Correct ordering (a deploy script runs after link, not before)
- Incremental skip (don't re-run if inputs haven't changed)
- Workspace awareness (tasks can depend on specific module builds)
- Consistent variable expansion (same `${target}`, `${now.version}` everywhere)

---

## Pastlet Format

A **pastlet** is a `.pasta` file describing a runnable task.

### Minimal form

```pasta
{
  run: "clang-format -i ${sources.dir}/**.c"
}
```

### Full form

```pasta
{
  name:        "gen-keys",
  description: "Generate test key material",
  run:         "scripts/genkeys.sh ${target}/test-keys",
  env:         { KEY_BITS: "2048", VERBOSE: "1" },
  needs:       ["build"],
  inputs:      ["scripts/genkeys.sh"],
  outputs:     ["${target}/test-keys/"],
  parallel:    false,
  hook:        "post-build"
}
```

### Field reference

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `string?` | filename stem | Task identifier, used in `now execute <name>` |
| `description` | `string?` | — | Human-readable description for `--list` |
| `run` | `string!` | — | Shell command with variable expansion |
| `env` | `map?` | `{}` | Extra environment variables |
| `needs` | `string[]?` | `[]` | Phase names or task names this depends on |
| `inputs` | `string[]?` | `[]` | Files/globs — changes trigger re-execution |
| `outputs` | `string[]?` | `[]` | Files produced — used for incremental skip |
| `parallel` | `bool?` | `false` | Can run concurrently with sibling tasks in same wave |
| `hook` | `string?` | — | Attach to lifecycle phase (e.g. `pre-build`, `post-test`) |
| `timeout` | `string?` | `"60s"` | Maximum execution time |
| `continue_on_error` | `bool?` | `false` | Record failure but don't stop the chain |

### Variable expansion

All `now.pasta` project properties are available in `run`, `inputs`, `outputs`:

| Variable | Value |
|----------|-------|
| `${now.group}` | Project group |
| `${now.artifact}` | Project artifact |
| `${now.version}` | Project version |
| `${now.basedir}` | Project root directory |
| `${target}` | `target/` directory |
| `${sources.dir}` | Main sources directory |
| `${triple}` | Active target triple |

---

## Discovery

### File-based pastlets

Pastlets live in a `tasks/` directory at the project root:

```
project/
  now.pasta
  tasks/
    gen-keys.pasta
    deploy-staging.pasta
    lint.pasta
  src/
    main/c/...
```

Task name defaults to the filename stem (`gen-keys.pasta` → `gen-keys`).

### Inline tools

Small tasks can be declared inline in `now.pasta` under `tools:`:

```pasta
{
  group: "org.acme",
  artifact: "myapp",
  version: "1.0.0",
  lang: "c",
  tools: {
    fmt:      { run: "clang-format -i ${sources.dir}/**.c" },
    gen-keys: { run: "scripts/genkeys.sh", needs: ["build"] }
  }
}
```

Both forms are merged at load time. File-based pastlets override inline tools
with the same name.

---

## CLI Interface

```sh
now execute <task> [<task>...]     # Run named tasks (+ their dependencies)
now execute --list                 # List available tasks with descriptions
now execute --graph                # Print task DAG (dot format)
now execute --dry-run <task>       # Show what would run without executing
```

Examples:

```sh
now execute gen-keys               # Runs: procure → build → gen-keys
now execute lint fmt               # Runs lint and fmt (possibly in parallel)
now execute deploy-staging         # Runs full chain up to deploy
```

---

## Graph Integration

Tasks form nodes in a unified DAG alongside build phases. The `needs` field
creates edges. Kahn's algorithm (already implemented for workspace modules)
produces parallel waves.

### Phase names as dependencies

Tasks can depend on any lifecycle phase by name:

| `needs` value | Waits for |
|---------------|-----------|
| `"procure"` | Dependency resolution complete |
| `"generate"` | Code generation complete |
| `"compile"` | All sources compiled |
| `"build"` | Compile + link complete |
| `"test"` | Tests executed |
| `"package"` | Archive assembled |

### Task-to-task dependencies

Tasks can depend on other tasks:

```pasta
{ name: "deploy", run: "deploy.sh", needs: ["package", "gen-keys"] }
```

### Example DAG

```
                    ┌─────────┐
                    │ procure │
                    └────┬────┘
                         │
                    ┌────▼────┐
                    │generate │
                    └────┬────┘
                         │
        ┌────────────────┼────────────────┐
   ┌────▼────┐      ┌───▼───┐       ┌────▼────┐
   │ compile │      │  lint  │       │   fmt   │
   └────┬────┘      └───────┘       └─────────┘
        │
   ┌────▼────┐
   │  link   │
   └────┬────┘
        │
   ┌────▼────┐
   │gen-keys │  (needs: ["build"])
   └────┬────┘
        │
   ┌────▼──────┐
   │  deploy   │  (needs: ["gen-keys", "package"])
   └───────────┘
```

---

## Workspace Awareness

In a multi-module workspace, tasks respect module scope:

### Module-level tasks

A pastlet in `modules/core/tasks/` only sees core's build context:

```
workspace/
  now.pasta              # modules: ["core", "app"]
  tasks/
    integration.pasta    # workspace-level task
  core/
    now.pasta
    tasks/
      gen-headers.pasta  # core-only task
  app/
    now.pasta
```

### Cross-module dependencies

Root-level tasks can depend on specific module builds using `module:phase` syntax:

```pasta
{ name: "integration", run: "test-integration.sh", needs: ["core:build", "app:build"] }
```

### Scoped execution

```sh
now execute integration               # Runs from workspace root
cd core && now execute gen-headers     # Runs from module context
```

---

## Incremental Skip

Tasks with `inputs` and `outputs` support incremental execution:

1. If all `outputs` exist and are newer than all `inputs`, skip
2. Otherwise, execute the task and update timestamps
3. Tasks without `inputs`/`outputs` always run (side-effect tasks)

This reuses the same mtime-based approach as the build manifest.

---

## Hooked Tasks

Tasks with a `hook` field run automatically at the specified lifecycle point:

```pasta
{
  name: "check-format",
  run:  "clang-format --dry-run --Werror ${sources.dir}/**.c",
  hook: "pre-compile"
}
```

Available hooks: `pre-procure`, `post-procure`, `pre-generate`, `post-generate`,
`pre-compile`, `post-compile`, `pre-link`, `post-link`, `pre-test`, `post-test`,
`pre-package`, `post-package`, `pre-publish`, `post-publish`.

Non-zero exit from a hooked task fails the build at that phase.

---

## Relationship to Existing Features

| Feature | Scope | How it differs |
|---------|-------|----------------|
| **Plugins** (`plugins:`) | Code generation, deep integration | Pasta IPC protocol, source injection |
| **Tools** (`tools:`) | Inline simple commands | No separate file, no incremental skip |
| **Pastlets** (`tasks/`) | Complex, graph-aware scripts | Own files, DAG dependencies, inputs/outputs |

All three share the same lifecycle hooks and variable expansion. Pastlets are
the "files on disk" counterpart to inline tools, with added graph awareness.

---

## Implementation Path

1. **Task loading** — parse `tasks/*.pasta` + `tools:` from `now.pasta`
2. **Variable expansion** — substitute `${...}` from project model
3. **Unified DAG** — merge build phases + tasks into single graph
4. **Kahn's scheduling** — reuse existing wave-based topo sort
5. **Incremental skip** — inputs/outputs mtime comparison
6. **`now execute` CLI** — task name resolution, `--list`, `--graph`, `--dry-run`
7. **Hook integration** — hooked tasks dispatch alongside plugins

Steps 1-4 reuse existing infrastructure (Pasta parsing, POM loading, Kahn's
algorithm). The main new work is variable expansion and the unified DAG.
