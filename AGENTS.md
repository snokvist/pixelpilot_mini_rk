# AGENTS Instructions

## Scope
These instructions apply to the entire repository. Update this file whenever new conventions or project expectations arise.

## Agentic AI Workflow (Boris Cherny Best Practices)

The following practices are adapted from Boris Cherny's agentic coding methodology and apply when using AI agents (Claude Code or similar) to contribute to this project.

### Plan before executing
Start every non-trivial task in plan mode. Iterate on the plan until it is specific and complete, then switch to execution. This forces explicitness, prevents unwanted changes, and enables single-pass implementations without back-and-forth revisions.

### Verification loops
Verification is the single most important factor for output quality. Every change must be verified before it is considered done:
- Build the project (`make`) and confirm it compiles cleanly.
- Run available tests or describe manual verification steps if no automated tests cover the change.
- Review the diff before committing — ensure no unrelated modifications, debug artifacts, or regressions slipped in.

### Self-correcting memory (this file)
When a code review or CI run reveals a mistake, do not just fix the code — update this `AGENTS.md` with a new rule or clarification so the same mistake is never repeated. Every correction becomes a convention. This file is the project's institutional memory for agentic workflows.

### Subagents for specialized tasks
Break complex work into focused phases and delegate each to a purpose-built subagent or slash command when available:
- **Research / explore** — gather context before modifying code.
- **Implement** — make the changes against a clear plan.
- **Simplify** — review the result for unnecessary complexity, duplicated logic, or over-engineering.
- **Verify** — run builds, tests, and integration checks before landing.

### Parallel independent work
When multiple files or features are independent, work on them concurrently. Do not serialize tasks that have no data dependency. Parallelism applies to both agent sessions and tool invocations within a single session.

### Prefer quality over speed
Use the most capable model available (e.g. Opus with thinking) for non-trivial work. A wrong fast answer costs more total time than a correct slow answer because of the rework cycle.

### Minimize blast radius
- Read code before modifying it.
- Prefer editing existing files over creating new ones.
- Do not add features, refactor surrounding code, or improve formatting beyond what was explicitly requested.
- Keep solutions at the minimum complexity needed for the current task.

### Hooks and automation
Use pre-commit hooks or post-tool-use hooks to enforce formatting and linting automatically so that CI never fails on trivial style issues. Do not bypass hooks or safety checks to make something build faster.

## Coding Guidelines
- Prefer self-documenting code with descriptive identifiers.
- Keep functions short and focused; refactor shared logic into helpers when appropriate.
- When adding headers or source files, update the `Makefile` accordingly.
- Follow existing formatting conventions (indentation, brace style) observed in surrounding files.
- Document non-obvious behavior with concise comments placed near the relevant code.

## Testing and Verification
- Run `make` after every meaningful change to confirm the build succeeds.
- Run available test scripts (e.g. `python3 tests/test_udp_osd_controls.py`) when touching OSD or UDP code paths.
- If automated tests are unavailable, describe manual verification steps in the commit or PR summary.

## Git and PR Process
- Group related modifications into a single commit with a descriptive message.
- Summaries should highlight functional changes and any tests executed.
- After committing, generate a PR message via the provided automation tools, ensuring the summary matches the committed changes.

## Documentation and Updates
- Update README or inline documentation whenever behavior or setup steps change.
- Expand this `AGENTS.md` with new project-specific conventions as they emerge to assist future work.

## Known Pitfalls (learned from past reviews)
- The `osd_ext_feed` module is archived and excluded from builds — do not reference or re-enable it.
- The build system uses `Makefile` (not CMake) — always update `Makefile` when adding sources.
- OSD asset IDs must be unique across all `[osd.element.*]` sections (range 0–7).
- SSE shutdown requires atomic flag checks to avoid race conditions — use GLib atomics, not bare booleans.
