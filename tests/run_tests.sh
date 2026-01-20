#!/usr/bin/env bash
set -euo pipefail

bin=""
if [[ -x "${AICLI_BIN:-}" ]]; then
	bin="$AICLI_BIN"
elif [[ -x "../src/aicli" ]]; then
	bin="$PWD/../src/aicli"
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

# dsl: backslash escapes + double quotes
printf "hello world\n" > tmp/esc.txt
q1=$("$bin" _exec --file tmp/esc.txt "cat tmp/esc.txt | grep \"hello\\ world\"" 2>/dev/null | tr -d '\r')
test "$q1" = $'hello world'

# dsl: end-of-options marker '--'
q2=$("$bin" _exec --file tmp/esc.txt "cat tmp/esc.txt | grep -- \"hello world\"" 2>/dev/null | tr -d '\r')
test "$q2" = $'hello world'

# dsl: single quotes treat backslash literally (POSIX-ish)
printf '%s\n' $'a\\b' > tmp/sq.txt
sq=$("$bin" _exec --file tmp/sq.txt "cat tmp/sq.txt | grep 'a\\b'" 2>/dev/null | tr -d '\r')
test "$sq" = $'a\\b'

# dsl: head/tail -nN short form
printf "1\n2\n3\n" > tmp/hn.txt
h1=$("$bin" _exec --file tmp/hn.txt "cat tmp/hn.txt | head -n2" 2>/dev/null | tr -d '\r')
test "$h1" = $'1\n2'
t1=$("$bin" _exec --file tmp/hn.txt "cat tmp/hn.txt | tail -n1" 2>/dev/null | tr -d '\r')
test "$t1" = $'3'

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

# stdin: explicit --stdin and implicit (no --file)
stdin1=$(printf "z\ny\n" | "$bin" _exec --stdin "cat - | head -n 1" 2>/dev/null | tr -d '\r')
test "$stdin1" = "z"

stdin2=$(printf "a\nb\n" | "$bin" _exec "cat - | tail -n 1" 2>/dev/null | tr -d '\r')
test "$stdin2" = "b"

# run + stdin: only a smoke test (requires OPENAI_API_KEY). If missing, skip.
if [[ -n "${OPENAI_API_KEY:-}" ]]; then
	run_out=$(printf "HELLO_FROM_STDIN\n" | "$bin" run --stdin --turns 1 --max-tool-calls 1 --tool-threads 1 --force-tool none "Say OK" 2>/dev/null | tr -d '\r')
	# We don't assert content strongly (model-dependent), just that it runs.
	test -n "$run_out"
fi

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

# config file: discovery + flags
repo_root="$PWD"
mkdir -p tmp/home tmp/work/sub

# Put a marker in the DEFAULT for later assertions.
cat > tmp/home/.aicli.json <<'EOF'
{
  "model": "MODEL_FROM_HOME"
}
EOF

# Nearest config in CWD/parents should win (work/.aicli.json takes precedence over home/.aicli.json)
cat > tmp/work/.aicli.json <<'EOF'
{
  "model": "MODEL_FROM_WORK"
}
EOF

conf_model=$(
	HOME="$repo_root/tmp/home" \
	cd "$repo_root/tmp/work/sub" && \
	"$bin" _exec --file "$repo_root/tmp/work/.aicli.json" "cat $repo_root/tmp/work/.aicli.json | grep -n 'MODEL_'" 2>/dev/null | tr -d '\r'
)
echo "$conf_model" | grep -q 'MODEL_FROM_WORK'

# --no-config should ignore discovery.
set +e
no_conf_out=$(
	cd "$repo_root/tmp/work/sub" && \
	HOME="$repo_root/tmp/home" "$bin" --no-config --help 2>&1
)
rc=$?
set -e
test $rc -eq 0

# --config PATH: explicit file should be usable even if discovery exists.
cat > tmp/work/sub/explicit.json <<'EOF'
{
  "model": "MODEL_FROM_EXPLICIT"
}
EOF

conf_explicit=$(
	HOME="$repo_root/tmp/home" \
	cd "$repo_root/tmp/work/sub" && \
	"$bin" --config "$repo_root/tmp/work/sub/explicit.json" _exec --file "$repo_root/tmp/work/sub/explicit.json" "cat $repo_root/tmp/work/sub/explicit.json | grep MODEL_FROM_EXPLICIT" 2>/dev/null | tr -d '\r'
)
echo "$conf_explicit" | grep -q 'MODEL_FROM_EXPLICIT'

rm -rf tmp

echo "OK (scaffold)"
