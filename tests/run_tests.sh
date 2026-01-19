#!/usr/bin/env bash
set -euo pipefail

# Minimal smoke test: binary runs and prints scaffold message
./bin/aicli --help >/dev/null
./bin/aicli chat "hi" >/dev/null 2>&1 || true

# execute MVP: cat only
out=$(./bin/aicli _exec --file README.md "cat README.md" 2>/dev/null | head -c 4)
test "$out" = "# ai"

# path traversal should be rejected unless it resolves to allowed realpath
mkdir -p tests/tmp
echo "X" > tests/tmp/x.txt
ln -sf ../tmp/x.txt tests/tmp/link.txt

allowed_real="$(realpath tests/tmp/x.txt)"

# allowed: using the allowed file path
./bin/aicli _exec --file tests/tmp/x.txt "cat tests/tmp/x.txt" >/dev/null

# allowed: cat via symlink should succeed only if in allowlist as target realpath
set +e
./bin/aicli _exec --file tests/tmp/x.txt "cat tests/tmp/link.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -eq 0

# rejected: allowlist is README only
set +e
./bin/aicli _exec --file README.md "cat tests/tmp/x.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -ne 0

rm -rf tests/tmp

echo "OK (scaffold)"
