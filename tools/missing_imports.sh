#!/bin/bash
# usage: tools/missing_imports.sh -- imports in gen/stubs.c with no host implementation yet
cd "$(dirname "$0")/.."
grep -ho "void imp_[A-Za-z0-9_]*" runtime/*.c runtime/*/*.c 2>/dev/null | sed 's/void //' | sort -u > /tmp/.impl.$$
grep -o "void imp_[A-Za-z0-9_]*" gen/stubs.c | sed 's/void //' | sort -u | comm -23 - /tmp/.impl.$$
rm -f /tmp/.impl.$$
