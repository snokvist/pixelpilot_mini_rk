# AGENTS Instructions

## Scope
These instructions apply to the entire repository. Update this file whenever new conventions or project expectations arise.

## Coding Guidelines
- Prefer self-documenting code with descriptive identifiers.
- Keep functions short and focused; refactor shared logic into helpers when appropriate.
- When adding headers or source files, update the corresponding `CMakeLists.txt` or build scripts if required.
- Follow existing formatting conventions (e.g., indentation, brace style) observed in the surrounding files.
- Document non-obvious behavior with concise comments placed near the relevant code.

## Testing and Verification
- Run available unit or integration tests after making meaningful changes. Use the `Makefile` targets if present (e.g., `make`, `make test`).
- If tests are unavailable, describe any manual verification steps in the final summary.

## Git and PR Process
- Group related modifications into a single commit with a descriptive message.
- Summaries should highlight functional changes and new tests executed.
- After committing, generate a PR message via the provided automation tools, ensuring the summary matches the committed changes.

## Documentation and Updates
- Update README or inline documentation whenever behavior or setup steps change.
- Expand this `AGENTS.md` with new project-specific conventions as they emerge to assist future work.
