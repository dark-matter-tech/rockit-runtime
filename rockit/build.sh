#!/bin/bash
# build.sh — Build the Rockit runtime from modular .rok sources
# Concatenates all modules in dependency order, compiles with --no-runtime
#
# Usage:
#   ROCKIT=/path/to/rockit bash rockit/build.sh
#
# If ROCKIT is not set, the script looks for `rockit` on PATH.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$SCRIPT_DIR"

# Find the Rockit compiler
if [ -n "$ROCKIT" ]; then
    COMMAND="$ROCKIT"
elif command -v rockit >/dev/null 2>&1; then
    COMMAND="rockit"
else
    echo "Error: Rockit compiler not found."
    echo "Set the ROCKIT environment variable or add rockit to your PATH."
    exit 1
fi

echo "Using compiler: $COMMAND"

# Concatenate modules in dependency order
cat \
    math.rok \
    memory.rok \
    string.rok \
    string_ops.rok \
    object.rok \
    list.rok \
    map.rok \
    io.rok \
    exception.rok \
    file.rok \
    process.rok \
    network.rok \
    concurrency.rok \
    > rockit_runtime.rok

echo "Concatenated runtime -> rockit_runtime.rok"

# Compile with --no-runtime to produce LLVM IR
"$COMMAND" compile rockit_runtime.rok --emit-llvm --no-runtime -o rockit_runtime.ll

echo "Generated rockit_runtime.ll"

# Compile LLVM IR to linkable object file
clang -c -O1 -w rockit_runtime.ll -o rockit_runtime.o

echo "Built rockit_runtime.o -> $SCRIPT_DIR/rockit_runtime.o"
