#!/bin/sh
# Run all host-side unit tests.
# Usage: ./test.sh
set -e
cd "$(dirname "$0")/tests"
make run
