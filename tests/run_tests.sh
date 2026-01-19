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

# pipe: nl + head
line1=$("$bin" _exec --file ../README.md "cat ../README.md | nl | head -n 1" 2>/dev/null | tr -d '\r')
echo "$line1" | grep -q $'^\s*1\t# aicli'

# pipe: tail
last=$("$bin" _exec --file ../README.md "cat ../README.md | tail -n 1" 2>/dev/null)
test -n "$last"

# pipe: sort
mkdir -p tmp
printf "b\na\nc\n" > tmp/sort.txt
sorted=$("$bin" _exec --file tmp/sort.txt "cat tmp/sort.txt | sort" 2>/dev/null | tr -d '\r')
test "$sorted" = $'a\nb\nc'

# pipe: grep
printf "foo\nbar\nfoo bar\n" > tmp/grep.txt
g1=$("$bin" _exec --file tmp/grep.txt "cat tmp/grep.txt | grep foo" 2>/dev/null | tr -d '\r')
test "$g1" = $'foo\nfoo bar'
g2=$("$bin" _exec --file tmp/grep.txt "cat tmp/grep.txt | grep -n bar" 2>/dev/null | tr -d '\r')
test "$g2" = $'2:bar\n3:foo bar'

# pipe: sed (line address)
printf "l1\nl2\nl3\n" > tmp/sed.txt
s1=$("$bin" _exec --file tmp/sed.txt "cat tmp/sed.txt | sed -n '2p'" 2>/dev/null | tr -d '\r')
test "$s1" = $'l2'
s2=$("$bin" _exec --file tmp/sed.txt "cat tmp/sed.txt | sed -n '2d'" 2>/dev/null | tr -d '\r')
test "$s2" = $'l1\nl3'

s3=$("$bin" _exec --file tmp/sed.txt "cat tmp/sed.txt | sed -n '2,3p'" 2>/dev/null | tr -d '\r')
test "$s3" = $'l2\nl3'
s4=$("$bin" _exec --file tmp/sed.txt "cat tmp/sed.txt | sed -n '2,3d'" 2>/dev/null | tr -d '\r')
test "$s4" = $'l1'

# pipe: wc
bytes=$("$bin" _exec --file ../README.md "cat ../README.md | wc -c" 2>/dev/null | tr -d '\n')
echo "$bytes" | grep -qE '^[0-9]+$'

lines=$("$bin" _exec --file ../README.md "cat ../README.md | wc -l" 2>/dev/null | tr -d '\n')
echo "$lines" | grep -qE '^[0-9]+$'

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
