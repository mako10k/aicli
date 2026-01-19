- Verify scaffold exists.
- Keep the project single-binary oriented.
- `execute` must be read-only and limited to allowlisted files.
- Prefer environment variables for secrets.

- Commit gate (for Copilot commits): before committing any changes, run `make check` and `make qa`.
	- If tools are missing (e.g., `clang-format`/`clang-tidy`), `make qa` must still pass (it should warn and continue).
	- If either fails, fix the issue or ask the user how to proceed; do not commit.
