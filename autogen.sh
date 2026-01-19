#!/usr/bin/env bash
set -euo pipefail

autoreconf -i

echo "Now run: ./configure && make && make check"
