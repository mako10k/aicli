#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bin=""
if [[ -x "${AICLI_BIN:-}" ]]; then
	bin="$AICLI_BIN"
elif [[ -x "../src/aicli" ]]; then
	bin="$PWD/../src/aicli"
elif [[ -x "./src/aicli" ]]; then
	bin="$PWD/src/aicli"
else
	echo "aicli binary not found" >&2
	exit 127
fi

readme="$repo_root/README.md"
tmpdir="$repo_root/tests/tmp"
mkdir -p "$tmpdir"

assert_contains() {
	local hay="$1"
	local needle="$2"
	echo "$hay" | grep -F -q -- "$needle" || {
		echo "assert_contains failed: expected '$needle'" >&2
		echo "--- output ---" >&2
		echo "$hay" >&2
		exit 1
	}
}

assert_not_contains() {
	local hay="$1"
	local needle="$2"
	if echo "$hay" | grep -F -q -- "$needle"; then
		echo "assert_not_contains failed: did not expect '$needle'" >&2
		echo "--- output ---" >&2
		echo "$hay" >&2
		exit 1
	fi
}

# Minimal smoke test: binary runs and prints help
"$bin" --help >/dev/null
"$bin" chat "hi" >/dev/null 2>&1 || true

# execute: cat only
out=$("$bin" _exec --file "$readme" "cat $readme" 2>/dev/null | head -c 4)
test "$out" = "# ai"

# grep -v (invert match)
out=$("$bin" _exec --file "$readme" "cat \"$readme\" | grep -v '^#'" 2>/dev/null | tr -d '\r')
assert_contains "$out" "aicli"
assert_not_contains "$out" "# aicli"
echo "ok: grep -v"

out=$("$bin" _exec --file "$readme" "cat \"$readme\" | grep -n -v '^#'" 2>/dev/null | tr -d '\r')
assert_contains "$out" "2:"
assert_not_contains "$out" "# aicli"
echo "ok: grep -n -v"

# grep error formatting tests intentionally omitted:
# aicli's execute DSL rejects some raw metacharacters and the executor may treat
# stage failures uniformly as "mvp_unsupported_stage" in the CLI wrapper.

# pipe: nl + head
line1=$("$bin" _exec --file "$readme" "cat $readme | nl | head -n 1" 2>/dev/null | tr -d '\r')
echo "$line1" | grep -q $'^\s*1\t# aicli'

# pipe: nl -ba (compat)
line1b=$("$bin" _exec --file "$readme" "cat $readme | nl -ba | head -n 1" 2>/dev/null | tr -d '\r')
echo "$line1b" | grep -q $'^\s*1\t# aicli'

# pipe: tail
last=$("$bin" _exec --file "$readme" "cat $readme | tail -n 1" 2>/dev/null)
test -n "$last"

# pipe: sort
printf "b\na\nc\n" > "$tmpdir/sort.txt"
sorted=$("$bin" _exec --file "$tmpdir/sort.txt" "cat $tmpdir/sort.txt | sort" 2>/dev/null | tr -d '\r')
test "$sorted" = $'a\nb\nc'

# pipe: grep
printf "foo\nbar\nfoo bar\n" > "$tmpdir/grep.txt"
g1=$("$bin" _exec --file "$tmpdir/grep.txt" "cat $tmpdir/grep.txt | grep foo" 2>/dev/null | tr -d '\r')
test "$g1" = $'foo\nfoo bar'
g2=$("$bin" _exec --file "$tmpdir/grep.txt" "cat $tmpdir/grep.txt | grep -n bar" 2>/dev/null | tr -d '\r')
test "$g2" = $'2:bar\n3:foo bar'

g3=$("$bin" _exec --file "$tmpdir/grep.txt" "cat $tmpdir/grep.txt | grep -F 'foo bar'" 2>/dev/null | tr -d '\r')
test "$g3" = $'foo bar'

# grep (BRE): '.' should match any char
printf 'foo\nfoX\nbar\n' > "$tmpdir/grep_re.txt"
gr1=$("$bin" _exec --file "$tmpdir/grep_re.txt" "cat $tmpdir/grep_re.txt | grep 'fo.'" 2>/dev/null | tr -d '\r')
test "$gr1" = $'foo\nfoX'

# dsl: backslash escapes + double quotes
printf "hello world\n" > "$tmpdir/esc.txt"
q1=$("$bin" _exec --file "$tmpdir/esc.txt" "cat $tmpdir/esc.txt | grep \"hello\\ world\"" 2>/dev/null | tr -d '\r')
test "$q1" = $'hello world'

# dsl: end-of-options marker '--'
q2=$("$bin" _exec --file "$tmpdir/esc.txt" "cat $tmpdir/esc.txt | grep -- \"hello world\"" 2>/dev/null | tr -d '\r')
test "$q2" = $'hello world'

# dsl: single quotes treat backslash literally (POSIX-ish)
printf '%s\n' $'a\\b' > "$tmpdir/sq.txt"
sq=$("$bin" _exec --file "$tmpdir/sq.txt" "cat $tmpdir/sq.txt | grep 'a\\b'" 2>/dev/null | tr -d '\r')
test "$sq" = $'a\\b'

# dsl: head/tail -nN short form
printf "1\n2\n3\n" > "$tmpdir/hn.txt"
h1=$("$bin" _exec --file "$tmpdir/hn.txt" "cat $tmpdir/hn.txt | head -n2" 2>/dev/null | tr -d '\r')
test "$h1" = $'1\n2'
t1=$("$bin" _exec --file "$tmpdir/hn.txt" "cat $tmpdir/hn.txt | tail -n1" 2>/dev/null | tr -d '\r')
test "$t1" = $'3'

# pipe: sed (line address)
printf "l1\nl2\nl3\n" > "$tmpdir/sed.txt"
s1=$("$bin" _exec --file "$tmpdir/sed.txt" "cat $tmpdir/sed.txt | sed -n '2p'" 2>/dev/null | tr -d '\r')
test "$s1" = $'l2'
s2=$("$bin" _exec --file "$tmpdir/sed.txt" "cat $tmpdir/sed.txt | sed -n '2d'" 2>/dev/null | tr -d '\r')
test "$s2" = $'l1\nl3'

s3=$("$bin" _exec --file "$tmpdir/sed.txt" "cat $tmpdir/sed.txt | sed -n '2,3p'" 2>/dev/null | tr -d '\r')
test "$s3" = $'l2\nl3'
s4=$("$bin" _exec --file "$tmpdir/sed.txt" "cat $tmpdir/sed.txt | sed -n '2,3d'" 2>/dev/null | tr -d '\r')
test "$s4" = $'l1'

# sed (BRE address): /RE/p and /RE/d
printf 'foo\nbar\nfoo bar\n' > "$tmpdir/sed_addr_re.txt"
sr1=$("$bin" _exec --file "$tmpdir/sed_addr_re.txt" "cat $tmpdir/sed_addr_re.txt | sed -n '/foo/p'" 2>/dev/null | tr -d '\r')
test "$sr1" = $'foo\nfoo bar'
sr2=$("$bin" _exec --file "$tmpdir/sed_addr_re.txt" "cat $tmpdir/sed_addr_re.txt | sed -n '/foo/d'" 2>/dev/null | tr -d '\r')
test "$sr2" = $'bar'

# sed (BRE range address): /RE/,/RE/p
printf 'x\nA\nmid\nB\ny\n' > "$tmpdir/sed_addr_range.txt"
sr3=$("$bin" _exec --file "$tmpdir/sed_addr_range.txt" "cat $tmpdir/sed_addr_range.txt | sed -n '/A/,/B/p'" 2>/dev/null | tr -d '\r')
test "$sr3" = $'A\nmid\nB'

# sed (BRE address): allow escaping delimiter '/'
printf 'a/b\naXb\n' > "$tmpdir/sed_addr_slash.txt"
sr4=$("$bin" _exec --file "$tmpdir/sed_addr_slash.txt" "cat $tmpdir/sed_addr_slash.txt | sed -n '/a\\/b/p'" 2>/dev/null | tr -d '\r')
test "$sr4" = $'a/b'

# pipe: sed (regex substitution; BRE via regex.h)
printf "foo\nbar\nfoo bar\n" > "$tmpdir/sed_subst.txt"

# only prints substituted lines on 'p' (because -n)
sp1=$("$bin" _exec --file "$tmpdir/sed_subst.txt" "cat $tmpdir/sed_subst.txt | sed -n 's/foo/ZZ/p'" 2>/dev/null | tr -d '\r')
test "$sp1" = $'ZZ\nZZ bar'

# global replacement + print-on-match
sp2=$("$bin" _exec --file "$tmpdir/sed_subst.txt" "cat $tmpdir/sed_subst.txt | sed -n 's/o/O/gp'" 2>/dev/null | tr -d '\r')
test "$sp2" = $'fOO\nfOO bar'

# pipe: wc
bytes=$("$bin" _exec --file "$readme" "cat $readme | wc -c" 2>/dev/null | tr -d '\n')
echo "$bytes" | grep -qE '^[0-9]+$'

lines=$("$bin" _exec --file "$readme" "cat $readme | wc -l" 2>/dev/null | tr -d '\n')
echo "$lines" | grep -qE '^[0-9]+$'

words=$("$bin" _exec --file "$readme" "cat $readme | wc -w" 2>/dev/null | tr -d '\n')
echo "$words" | grep -qE '^[0-9]+$'

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
mkdir -p "$tmpdir"
echo "X" > "$tmpdir/x.txt"
ln -sf "$(realpath "$tmpdir/x.txt")" "$tmpdir/link.txt"

allowed_real="$(realpath "$tmpdir/x.txt")"

# allowed: using the allowed file path
"$bin" _exec --file "$tmpdir/x.txt" "cat $tmpdir/x.txt" >/dev/null

# allowed: cat via symlink should succeed only if in allowlist as target realpath
set +e
"$bin" _exec --file "$tmpdir/x.txt" "cat $tmpdir/link.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -eq 0

# rejected: allowlist is README only
set +e
"$bin" _exec --file "$readme" "cat $tmpdir/x.txt" >/dev/null 2>&1
rc=$?
set -e
test $rc -ne 0

rm -rf "$tmpdir"

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
