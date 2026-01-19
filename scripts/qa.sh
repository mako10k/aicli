#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="${1:-check}"
# Modes:
#   check : fail on issues (CI/default)
#   fix   : apply formatting fixes where possible

if [[ "$MODE" != "check" && "$MODE" != "fix" ]]; then
	echo "usage: $0 [check|fix]" >&2
	exit 2
fi

have() { command -v "$1" >/dev/null 2>&1; }

fail=0

step() {
	echo "==> $*" >&2
}

warn() {
	echo "[warn] $*" >&2
}

# 1) Formatting: clang-format
if have clang-format; then
	step "clang-format ($MODE)"
	mapfile -t files < <(cd "$ROOT_DIR" && git ls-files | grep -E '\\.(c|h)$')
	if [[ ${#files[@]} -gt 0 ]]; then
		if [[ "$MODE" == "fix" ]]; then
			clang-format -i "${files[@]}"
		else
			# clang-format --dry-run exits non-zero on diffs in newer versions; fall back to diff check.
			if ! clang-format --dry-run -Werror "${files[@]}" >/dev/null 2>&1; then
				warn "clang-format reported differences"
				fail=1
			fi
		fi
	fi
else
	warn "clang-format not found; skipping format step"
fi

# 2) Lint: clang-tidy (requires compile_commands.json)
# Prefer a compile database from autotools. If not present, try to generate via bear.
if have clang-tidy; then
	step "clang-tidy (check)"
	if [[ ! -f "$ROOT_DIR/compile_commands.json" ]]; then
		if have bear; then
			warn "compile_commands.json missing; generating via bear (make)"
			( cd "$ROOT_DIR" && bear -- make -j"$(nproc)" >/dev/null ) || true
		else
			warn "compile_commands.json missing and bear not found; skipping clang-tidy"
			clang_tidy_skip=1
		fi
	fi

	if [[ -z "${clang_tidy_skip:-}" && -f "$ROOT_DIR/compile_commands.json" ]]; then
		# Run clang-tidy on our sources only.
		mapfile -t cfiles < <(cd "$ROOT_DIR" && git ls-files | grep -E '^src/.*\\.c$')
		if [[ ${#cfiles[@]} -gt 0 ]]; then
			# Keep it strict but practical; configuration lives in .clang-tidy.
			if ! clang-tidy -p "$ROOT_DIR" "${cfiles[@]}" -- >/dev/null; then
				fail=1
			fi
		fi
	fi
else
	warn "clang-tidy not found; skipping lint step"
fi

# 3) Unused includes / dead code: best-effort
# IWYU is optional; enable automatically if installed.
if have include-what-you-use && have iwyu_tool.py; then
	step "include-what-you-use (best-effort)"
	if [[ -f "$ROOT_DIR/compile_commands.json" ]]; then
		# iwyu_tool.py returns non-zero when it finds suggestions.
		python3 "$(command -v iwyu_tool.py)" -p "$ROOT_DIR" src >/dev/null || fail=1
	else
		warn "compile_commands.json missing; skipping IWYU"
	fi
else
	warn "IWYU tools not found; skipping unused-include analysis"
fi

# 4) Tests
step "make check"
( cd "$ROOT_DIR" && ./autogen.sh >/dev/null && ./configure >/dev/null && make check >/dev/null ) || fail=1

if [[ $fail -ne 0 ]]; then
	echo "QA FAILED" >&2
	exit 1
fi

echo "QA OK" >&2
