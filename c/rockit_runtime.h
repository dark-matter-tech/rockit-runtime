// rockit_runtime.h
// Rockit Native Runtime — C runtime library for LLVM-compiled Rockit programs
// Copyright © 2026 Dark Matter Tech. All rights reserved.

#ifndef ROCKIT_RUNTIME_H
#define ROCKIT_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

// ── Null Sentinel ───────────────────────────────────────────────────────────
// In native code, null is represented as this non-zero sentinel value
// so that integer 0 and null are distinguishable (unlike in untagged i64).
// Value chosen to be: non-zero, below heap pointer range, too large for any index.
#define ROCKIT_NULL ((int64_t)0xCAFEBABE)

// ── Immortal String Refcount ────────────────────────────────────────────────
// String literals emitted by the compiler use this refcount value.
// retain/release become no-ops for immortal strings — zero malloc/free.
#define ROCKIT_IMMORTAL_REFCOUNT INT64_MAX

// ── Value Tags ──────────────────────────────────────────────────────────────

#define ROCKIT_TAG_INT     0
#define ROCKIT_TAG_FLOAT   1
#define ROCKIT_TAG_BOOL    2
#define ROCKIT_TAG_STRING  3
#define ROCKIT_TAG_NULL    4
#define ROCKIT_TAG_OBJECT  5
#define ROCKIT_TAG_UNIT    6

// ── Forward Declarations ────────────────────────────────────────────────────

typedef struct RockitString RockitString;
typedef struct RockitObject RockitObject;

// ── RockitString ────────────────────────────────────────────────────────────

struct RockitString {
    int64_t refCount;
    int64_t length;
    const char* chars;   // Active data pointer (owned: points to data[], slice: into base)
    RockitString* base;  // Non-NULL for slices (base is retained)
    int64_t capacity;    // Allocated data capacity in bytes (owned strings only)
    char data[];         // UTF-8, null-terminated, flexible array member (owned strings only)
};

RockitString* rockit_string_new(const char* utf8);
RockitString* rockit_string_concat(RockitString* a, RockitString* b);
RockitString* rockit_string_concat_consume(RockitString* a, RockitString* b);
RockitString* rockit_string_concat_n(int64_t n, RockitString** parts);
void rockit_string_retain(RockitString* s);
void rockit_string_release(RockitString* s);
int64_t rockit_string_length(RockitString* s);

// ── RockitObject ────────────────────────────────────────────────────────────

struct RockitObject {
    const char* typeName;
    int64_t     refCount;
    int32_t     fieldCount;
    uint32_t    ptrFieldBits;  // bitmask: bit i set = field i is a pointer (needs cascade release)
    int64_t     fields[];   // flexible array member — stores all field values as i64
};

RockitObject* rockit_object_alloc(const char* typeName, int32_t fieldCount);
int64_t  rockit_object_get_field(RockitObject* obj, int32_t index);
void     rockit_object_set_field(RockitObject* obj, int32_t index, int64_t value);
void     rockit_retain(RockitObject* obj);
void     rockit_release(RockitObject* obj);

// ── Universal Value ARC ─────────────────────────────────────────────────────
// Retain/release for any ref-counted value stored as i64 (type detected at runtime).
void     rockit_retain_value(int64_t val);
void     rockit_release_value(int64_t val);

// ── Runtime Type Checking ──────────────────────────────────────────────────

/// Type hierarchy entry: maps a child type name to its parent type name.
/// The compiler emits an array of these as @rockit_type_hierarchy.
typedef struct RockitTypeEntry {
    const char* child;
    const char* parent;
} RockitTypeEntry;

/// Set the type hierarchy table (called once from generated code at program start).
void rockit_set_type_hierarchy(const RockitTypeEntry* table, int32_t count);

/// Check if obj's runtime type matches targetType (or is a subtype of it).
/// Returns 1 if match, 0 otherwise. Returns 0 for null objects.
int8_t rockit_is_type(RockitObject* obj, const char* targetType);

/// Get the type name of an object. Returns NULL for null objects.
const char* rockit_object_get_type_name(RockitObject* obj);

// ── RockitList ──────────────────────────────────────────────────────────────

typedef struct RockitList {
    int64_t refCount;
    int64_t size;
    int64_t capacity;
    int64_t* data;
} RockitList;

RockitList* rockit_list_create(void);
RockitList* rockit_list_create_filled(int64_t size, int64_t value);
void     rockit_list_append(RockitList* list, int64_t value);
int64_t  rockit_list_get(RockitList* list, int64_t index);
void     rockit_list_set(RockitList* list, int64_t index, int64_t value);
int64_t  rockit_list_size(RockitList* list);
int8_t   rockit_list_is_empty(RockitList* list);
void     rockit_list_release(RockitList* list);
void     rockit_list_clear(RockitList* list);
int8_t   rockit_list_contains(RockitList* list, int64_t value);
int64_t  rockit_list_remove_at(RockitList* list, int64_t index);

// ── RockitMap ───────────────────────────────────────────────────────────────

typedef struct RockitMapEntry {
    int64_t key;
    int64_t value;
    int8_t  occupied;
} RockitMapEntry;

typedef struct RockitMap {
    int64_t        refCount;
    int64_t        size;
    int64_t        capacity;
    RockitMapEntry* entries;
} RockitMap;

RockitMap* rockit_map_create(void);
void     rockit_map_put(RockitMap* map, int64_t key, int64_t value);
int64_t  rockit_map_get(RockitMap* map, int64_t key);
int8_t   rockit_map_contains_key(RockitMap* map, int64_t key);
int64_t  rockit_map_size(RockitMap* map);
int8_t   rockit_map_is_empty(RockitMap* map);
void     rockit_map_release(RockitMap* map);
RockitList* rockit_map_keys(RockitMap* map);
RockitList* rockit_map_values(RockitMap* map);
void     rockit_map_remove(RockitMap* map, int64_t key);

// ── I/O ─────────────────────────────────────────────────────────────────────

void rockit_println_int(int64_t value);
void rockit_println_float(double value);
void rockit_println_bool(int8_t value);
void rockit_println_string(RockitString* s);
void rockit_println_null(void);
void rockit_print_int(int64_t value);
void rockit_print_float(double value);
void rockit_print_bool(int8_t value);
void rockit_print_string(RockitString* s);

// ── Conversion ──────────────────────────────────────────────────────────────

RockitString* rockit_int_to_string(int64_t value);
RockitString* rockit_float_to_string(double value);
RockitString* rockit_bool_to_string(int8_t value);

// ── Exception Handling (setjmp/longjmp) ─────────────────────────────────────

#include <setjmp.h>

#define ROCKIT_MAX_EXC_DEPTH 64

void* rockit_exc_push(void);       // Push frame, return jmp_buf pointer
void  rockit_exc_pop(void);        // Pop the current exception frame
void  rockit_exc_throw(int64_t value);  // Store value + longjmp
int64_t rockit_exc_get(void);      // Get the thrown exception value

// ── Process ─────────────────────────────────────────────────────────────────

void rockit_panic(const char* message);

// ── File Operations ─────────────────────────────────────────────────────

int64_t fileDelete(RockitString* path);

// ── Actor Runtime (Stage 0 — synchronous) ──────────────────────────────

/// In Stage 0, actors are implemented as regular objects.
/// Method calls on actors are synchronous (no actual concurrency).
/// This provides the correct semantics for state isolation while keeping
/// the implementation simple for bootstrap.

typedef struct RockitActor {
    RockitObject* object;    // The underlying object with fields
} RockitActor;

/// Create a new actor instance.
RockitActor* rockit_actor_create(const char* typeName, int32_t fieldCount);

/// Get the underlying object pointer from an actor (for field access).
RockitObject* rockit_actor_get_object(RockitActor* actor);

/// Release an actor.
void rockit_actor_release(RockitActor* actor);

// ── Async Runtime (Cooperative Task Scheduler) ──────────────────────────────

#define ROCKIT_COROUTINE_SUSPENDED ((int64_t)-9999)

/// Allocate a continuation frame of the given size (zero-initialized).
void* rockit_frame_alloc(int64_t size_bytes);

/// Free a continuation frame.
void rockit_frame_free(void* frame);

/// Schedule a task for execution. resume_fn is a state machine function pointer,
/// frame is its continuation frame, result is the value to pass on resume.
void rockit_task_schedule(void* resume_fn, void* frame, int64_t result);

/// Set up parent continuation in child frame and schedule the child.
/// Returns ROCKIT_COROUTINE_SUSPENDED.
int64_t rockit_await(void* child_resume_fn, void* child_frame,
                     void* parent_resume_fn, void* parent_frame);

/// Run the cooperative event loop until all tasks complete.
void rockit_run_event_loop(void);

/// Check if a return value indicates suspension.
int64_t rockit_is_suspended(int64_t value);

// ── Math Functions ──────────────────────────────────────────────────────────

double rockit_math_sqrt(double x);
double rockit_math_sin(double x);
double rockit_math_cos(double x);
double rockit_math_tan(double x);
double rockit_math_pow(double base, double exp);
double rockit_math_floor(double x);
double rockit_math_ceil(double x);
double rockit_math_round(double x);
double rockit_math_log(double x);
double rockit_math_exp(double x);
double rockit_math_abs(double x);
double rockit_math_atan2(double y, double x);
RockitString* formatFloat(double value, int64_t decimals);
double toFloat(int64_t value);
void listSetFloat(RockitList* list, int64_t index, double value);
double listGetFloat(RockitList* list, int64_t index);

// ── OpenSSL: TLS, Crypto, X.509 ────────────────────────────────────────────

// TLS context
int64_t tlsCreateContext(void);
int64_t tlsCreateServerContext(void);
int64_t tlsSetCertificate(int64_t ctx, const char *path);
int64_t tlsSetPrivateKey(int64_t ctx, const char *path);
void    tlsSetVerifyPeer(int64_t ctx, int64_t verify);
int64_t tlsSetAlpn(int64_t ctx, const char *protos);

// TLS connection
int64_t      tlsConnect(int64_t ctx, const char *host, int64_t port);
int64_t      tlsSend(int64_t conn, const char *data);
RockitString* tlsRecv(int64_t conn, int64_t maxBytes);
void         tlsClose(int64_t conn);
RockitString* tlsGetAlpn(int64_t conn);
int64_t      tlsGetPeerCert(int64_t conn);

// TLS server
int64_t tlsListen(int64_t ctx, int64_t port);
int64_t tlsAccept(int64_t ctx, int64_t serverFd);

// Crypto hashing
RockitString* cryptoSha256(const char *data);
RockitString* cryptoSha1(const char *data);
RockitString* cryptoSha512(const char *data);
RockitString* cryptoMd5(const char *data);

// Crypto HMAC
RockitString* cryptoHmacSha256(const char *key, const char *data);
RockitString* cryptoHmacSha1(const char *key, const char *data);

// Crypto random
RockitString* cryptoRandomBytes(int64_t count);

// Crypto AES
RockitString* cryptoAesEncrypt(const char *key, const char *iv, const char *data, int64_t mode);
RockitString* cryptoAesDecrypt(const char *key, const char *iv, const char *data, int64_t mode);

// X.509
int64_t      x509ParsePem(const char *pemData);
RockitString* x509Subject(int64_t handle);
RockitString* x509Issuer(int64_t handle);
int64_t      x509NotBefore(int64_t handle);
int64_t      x509NotAfter(int64_t handle);
RockitString* x509SerialNumber(int64_t handle);
void         x509Free(int64_t handle);

// Error reporting
RockitString* tlsLastError(void);

#endif // ROCKIT_RUNTIME_H
