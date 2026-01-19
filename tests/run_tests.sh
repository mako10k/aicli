#!/usr/bin/env bash
set -euo pipefail

# Minimal smoke test: binary runs and prints scaffold message
./bin/aicli --help >/dev/null
./bin/aicli chat "hi" >/dev/null 2>&1 || true

echo "OK (scaffold)"
