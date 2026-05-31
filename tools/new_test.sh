#!/bin/sh
# new_test.sh — scaffold a new host unit test.
#
# Usage:  tools/new_test.sh <name>      e.g.  tools/new_test.sh socket
#
# Creates tests/test_<name>.c from a template and prints the lines to add to
# tests/Makefile (a TESTS entry and a build rule). The test Makefile keeps an
# explicit list on purpose — each test links a hand-picked set of kernel
# sources — so wiring is intentionally a manual, visible step.
set -e

[ -n "$1" ] || { echo "usage: $0 <name>"; exit 1; }
NAME="$1"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/tests/test_${NAME}.c"

[ -e "$SRC" ] && { echo "error: $SRC already exists"; exit 1; }

# Template matches the framework.h API: RUN_SUITE() runs a void test function,
# ASSERT_EQ(a, b, "msg") (and friends) record pass/fail, and TEST_SUMMARY()
# prints totals and returns the process exit code from main().
cat > "$SRC" <<EOF
/* test_${NAME}.c — host unit tests for ${NAME}. */
#include "framework.h"

static void test_${NAME}_smoke(void)
{
    ASSERT_EQ(1, 1, "framework sanity");
}

int main(void)
{
    RUN_SUITE(test_${NAME}_smoke);
    TEST_SUMMARY();
}
EOF

echo "created tests/test_${NAME}.c"
echo
echo "Now wire it into tests/Makefile:"
echo "  1) append to the TESTS list:   \$(BINDIR)/test_${NAME}"
echo "  2) add a build rule (add include vars / kernel .c files as needed):"
echo "       \$(BINDIR)/test_${NAME}: test_${NAME}.c framework.h"
echo "       \t\$(CC) \$(CFLAGS) \$(KERNEL_INCLUDES) -o \$@ test_${NAME}.c"
