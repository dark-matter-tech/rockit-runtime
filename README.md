# Rockit Runtime

The native runtime for the Rockit programming language. Provides ARC memory management, a cooperative coroutine scheduler, actor message dispatch, and the platform bridge used by all compiled Rockit programs.

## Architecture

The runtime has two layers:

1. **C runtime** (`c/rockit_runtime.c`) -- The core runtime linked into every native Rockit binary. Implements reference-counted strings, objects, lists, maps, exception handling (setjmp/longjmp), a cooperative task scheduler with event loop, actor isolation, math builtins, file I/O, process utilities, and OpenSSL-backed TLS/crypto/X.509 operations. Compiled with any C11 compiler.

2. **Rockit freestanding modules** (`rockit/`) -- The same runtime functions rewritten in Rockit itself, compiled with `--no-runtime` (freestanding mode). These modules use `Ptr<T>`, `alloc`/`free`, `bitcast`, `extern`, `@CRepr`, and `unsafe` blocks to operate without any runtime dependency. They are concatenated into a single `rockit_runtime.rok` and compiled to a linkable object file.

## Building

The Rockit freestanding runtime requires the Rockit compiler (Stage 1 `command` binary or an installed `rockit`).

```
ROCKIT=/path/to/rockit bash rockit/build.sh
```

If `ROCKIT` is not set, the script looks for `rockit` on PATH.

The build script concatenates all `.rok` modules in dependency order, compiles them to LLVM IR with `--no-runtime --emit-llvm`, then uses `clang` to produce `rockit_runtime.o`.

To compile the C runtime separately:

```
clang -c -O2 -o c/rockit_runtime.o c/rockit_runtime.c
```

## Modules

### C Runtime

| File | Description |
|------|-------------|
| `rockit_runtime.c` | Full C runtime: strings, objects, ARC, lists, maps, I/O, exceptions, scheduler, actors, math, TLS/crypto |
| `rockit_runtime.h` | Public API header for the C runtime |

### Rockit Freestanding Modules

| Module | Description |
|--------|-------------|
| `memory.rok` | malloc/free wrappers, ARC retain/release |
| `string.rok` | RockitString @CRepr struct, new/eq/neq/concat/length |
| `string_ops.rok` | charAt, indexOf, substring, split, trim, replace, and more |
| `object.rok` | RockitObject allocation, field get/set, type checking |
| `list.rok` | RockitList dynamic array: create/append/get/set/remove/size |
| `map.rok` | RockitMap hash table: create/put/get/keys/remove |
| `io.rok` | println and print for int, float, string, and any |
| `exception.rok` | setjmp/longjmp-based exception stack |
| `file.rok` | fileRead, fileWrite, fileExists, fileDelete |
| `process.rok` | processArgs, getEnv, platformOS, systemExec |
| `math.rok` | sqrt, sin, cos, tan, floor, ceil, round, pow, log, and more |
| `network.rok` | Networking primitives |
| `concurrency.rok` | Task scheduler, frame alloc/free, cooperative event loop |

## License

Proprietary -- Dark Matter Tech. All rights reserved.
