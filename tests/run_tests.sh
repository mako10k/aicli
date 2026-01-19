#!/usr/bin/env bash
set -euo pipefail

bin=""
if [[ -x "${AICLI_BIN:-}" ]]; then
	bin="$AICLI_BIN"
elif [[ -x "./bin/aicli" ]]; then
	bin="./bin/aicli"
elif [[ -x "../src/aicli" ]]; then
	bin="../src/aicli"
else
	echo "aicli binary not found" >&2
	exit 127
fi

# Minimal smoke test: binary runs and prints help
"$bin" --help >/dev/null
"$bin" chat "hi" >/dev/null 2>&1 || true

# execute MVP: cat only
out=$("$bin" _exec --file ../README.md "cat ../README.md" 2>/dev/null | head -c 4)
test "$out" = "# ai"

# path traversal should be rejected unless it resolves to allowed realpath
mkdir -p tmp
echo "X" > tmp/x.txt
ln -sf ../tmp/x.txt tmp/link.txt

allowed_real="$(realpath tmp/x.txt)"

# allowed: using the allowed file path
"$bin" _exec --file tmp/x.txt "cat tmp/x.txt" >/dev/null

# allowed: cat via symlink should succeed only if in allowlist as target realpath
set +e
"$bin" _exec --file tmp/x.txt "cat tmp/link.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -eq 0

# rejected: allowlist is README only
set +e
"$bin" _exec --file ../README.md "cat tmp/x.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -ne 0

rm -rf tmp

echo "OK (scaffold)"
