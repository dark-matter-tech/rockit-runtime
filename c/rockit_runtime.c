// rockit_runtime.c
// Rockit Native Runtime — C runtime library for LLVM-compiled Rockit programs
// Copyright © 2026 Dark Matter Tech. All rights reserved.

#include "rockit_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <malloc.h>
#endif


// ── RockitString ────────────────────────────────────────────────────────────

// String pool: free list for small owned strings to avoid malloc/free overhead.
#define POOL_MAX_LEN 48
#define POOL_BLOCK_SIZE (sizeof(RockitString) + POOL_MAX_LEN + 1)

static void* string_pool_free = NULL;

// Slice pool: free list for string slice headers (fixed sizeof(RockitString) bytes).
static void* slice_pool_free = NULL;

static inline RockitString* string_alloc(int64_t len) {
    if (len <= POOL_MAX_LEN && string_pool_free) {
        RockitString* s = (RockitString*)string_pool_free;
        string_pool_free = *(void**)string_pool_free;
        return s;
    }
    if (len <= POOL_MAX_LEN) {
        return (RockitString*)malloc(POOL_BLOCK_SIZE);
    }
    return (RockitString*)malloc(sizeof(RockitString) + len + 1);
}

static inline void string_dealloc(RockitString* s) {
    if (s->length <= POOL_MAX_LEN) {
        *(void**)s = string_pool_free;
        string_pool_free = (void*)s;
    } else {
        free(s);
    }
}

static inline RockitString* slice_alloc(void) {
    if (slice_pool_free) {
        RockitString* s = (RockitString*)slice_pool_free;
        slice_pool_free = *(void**)slice_pool_free;
        return s;
    }
    return (RockitString*)malloc(sizeof(RockitString));
}

static inline void slice_dealloc(RockitString* s) {
    *(void**)s = slice_pool_free;
    slice_pool_free = (void*)s;
}

RockitString* rockit_string_new(const char* utf8) {
    if (!utf8) utf8 = "";
    int64_t len = (int64_t)strlen(utf8);
    RockitString* s = string_alloc(len);
    s->refCount = 1;
    s->length = len;
    s->base = NULL;
    s->capacity = (len <= POOL_MAX_LEN) ? POOL_MAX_LEN : len;
    memcpy(s->data, utf8, len + 1);
    s->chars = s->data;
    return s;
}

RockitString* rockit_string_concat(RockitString* a, RockitString* b) {

    int64_t newLen = a->length + b->length;
    RockitString* s = string_alloc(newLen);

    s->refCount = 1;
    s->length = newLen;
    s->base = NULL;
    s->capacity = (newLen <= POOL_MAX_LEN) ? POOL_MAX_LEN : newLen;
    memcpy(s->data, a->chars, a->length);
    memcpy(s->data + a->length, b->chars, b->length);
    s->data[newLen] = '\0';
    s->chars = s->data;
    return s;
}

// Consume-concat: takes ownership of LHS for in-place append.
// Used by codegen for `s = s + expr` self-append patterns.
RockitString* rockit_string_concat_consume(RockitString* a, RockitString* b) {
    int64_t newLen = a->length + b->length;

    // Fast path: unique owner, not a slice, not immortal
    if (a->refCount == 1 && a->base == NULL && a->refCount != ROCKIT_IMMORTAL_REFCOUNT) {
        if (newLen <= a->capacity) {
            // Append in-place — no allocation
            memcpy((char*)a->chars + a->length, b->chars, b->length);
            ((char*)a->chars)[newLen] = '\0';
            a->length = newLen;
            return a;
        }
        // Grow: realloc with 2x capacity
        int64_t newCap = a->capacity * 2;
        if (newCap < newLen) newCap = newLen;
        RockitString* a2 = (RockitString*)realloc(a, sizeof(RockitString) + newCap + 1);
        a2->chars = a2->data;
        memcpy(a2->data + a2->length, b->chars, b->length);
        a2->length = newLen;
        a2->data[newLen] = '\0';
        a2->capacity = newCap;
        return a2;
    }

    // Slow path: not unique or slice — allocate new, release old
    RockitString* result = rockit_string_concat(a, b);
    rockit_string_release(a);
    return result;
}

// Single-allocation multi-string concatenation.
// parts is a pointer to an array of n RockitString pointers.
RockitString* rockit_string_concat_n(int64_t n, RockitString** parts) {
    int64_t totalLen = 0;
    for (int64_t i = 0; i < n; i++) {
        totalLen += parts[i]->length;
    }
    if (totalLen == 0) return rockit_string_new("");
    RockitString* s = string_alloc(totalLen);
    s->refCount = 1;
    s->length = totalLen;
    s->base = NULL;
    s->capacity = (totalLen <= POOL_MAX_LEN) ? POOL_MAX_LEN : totalLen;
    char* dst = s->data;
    for (int64_t i = 0; i < n; i++) {
        memcpy(dst, parts[i]->chars, parts[i]->length);
        dst += parts[i]->length;
    }
    s->data[totalLen] = '\0';
    s->chars = s->data;
    return s;
}

void rockit_string_retain(RockitString* s) {
    if (s && s->refCount != ROCKIT_IMMORTAL_REFCOUNT) s->refCount++;
}

void rockit_string_release(RockitString* s) {
    if (s && s->refCount != ROCKIT_IMMORTAL_REFCOUNT && --s->refCount <= 0) {
        if (s->base) {
            rockit_string_release(s->base);
            slice_dealloc(s);
        } else {
            string_dealloc(s);
        }
    }
}

// Called when refCount has already reached 0 — handles slice vs owned dealloc.
void rockit_string_dealloc(RockitString* s) {
    if (s->base) {
        rockit_string_release(s->base);
        slice_dealloc(s);
    } else {
        string_dealloc(s);
    }
}

int64_t rockit_string_length(RockitString* s) {
    return s ? s->length : 0;
}

// ── RockitObject ────────────────────────────────────────────────────────────

RockitObject* rockit_object_alloc(const char* typeName, int32_t fieldCount) {
    RockitObject* obj = (RockitObject*)malloc(sizeof(RockitObject) + fieldCount * sizeof(int64_t));
    obj->typeName = typeName;
    obj->refCount = 1;
    obj->fieldCount = fieldCount;
    obj->ptrFieldBits = 0xFFFFFFFF;  // unknown: release all fields (conservative default)
    // Zero-initialize fields
    for (int32_t i = 0; i < fieldCount; i++) {
        obj->fields[i] = 0;
    }
    return obj;
}

int64_t rockit_object_get_field(RockitObject* obj, int32_t index) {
    if (!obj) {
        rockit_panic("null pointer dereference in field access");
    }
    if (index < 0 || index >= obj->fieldCount) {
        rockit_panic("field index out of bounds");
    }
    return obj->fields[index];
}

void rockit_object_set_field(RockitObject* obj, int32_t index, int64_t value) {
    if (!obj) {
        rockit_panic("null pointer dereference in field set");
    }
    if (index < 0 || index >= obj->fieldCount) {
        rockit_panic("field index out of bounds");
    }
    obj->fields[index] = value;
}

void rockit_retain(RockitObject* obj) {
    if (obj && obj->refCount >= 0) obj->refCount++;
}

void rockit_release(RockitObject* obj) {
    if (!obj || obj->refCount < 0) return;  // null or stack-allocated sentinel (-1)
    if (--obj->refCount <= 0) {
        // Cascade: release pointer-typed field values before freeing.
        // If ptrFieldBits is set, only release fields marked as pointers.
        // If ptrFieldBits is 0 (legacy/unknown), release all fields (conservative).
        uint32_t bits = obj->ptrFieldBits;
        if (bits == 0xFFFFFFFF) {
            // Unknown (legacy): conservatively release all fields
            for (int32_t i = 0; i < obj->fieldCount; i++) {
                rockit_release_value(obj->fields[i]);
            }
        } else {
            // Known: only release fields marked as pointers
            for (int32_t i = 0; i < obj->fieldCount && i < 32; i++) {
                if (bits & (1u << i)) {
                    rockit_release_value(obj->fields[i]);
                }
            }
        }
        free(obj);
    }
}

// ── Universal Value ARC ─────────────────────────────────────────────────────
// These functions handle retain/release for any ref-counted value stored as i64.
// Used by write barriers where the compile-time type is unknown.

static int is_likely_heap_ptr(int64_t value) {
    if (value == 0 || value == ROCKIT_NULL) return 0;
    uint64_t uval = (uint64_t)value;
    return (uval > 0x100000000ULL && uval < 0x800000000000ULL);
}

void rockit_retain_value(int64_t val) {

    if (!is_likely_heap_ptr(val)) return;
    void* ptr = (void*)(intptr_t)val;
    // RockitObject has typeName (a pointer) as its first field.
    // String/List/Map have refCount (a small integer) as their first field.
    int64_t first_field = *(int64_t*)ptr;
    if (first_field == ROCKIT_IMMORTAL_REFCOUNT) return;  // immortal string literal
    if (is_likely_heap_ptr(first_field)) {
        // First field is a pointer → RockitObject (typeName is first, refCount is second)
        ((RockitObject*)ptr)->refCount++;
    } else {
        // First field is refCount → String, List, or Map
        (*(int64_t*)ptr)++;
    }
}

void rockit_release_value(int64_t val) {
    if (!is_likely_heap_ptr(val)) { return; }
    void* ptr = (void*)(intptr_t)val;
    int64_t first_field = *(int64_t*)ptr;
    if (first_field == ROCKIT_IMMORTAL_REFCOUNT) return;  // immortal string literal
    if (is_likely_heap_ptr(first_field)) {
        // RockitObject: refCount is at offset 8
        RockitObject* obj = (RockitObject*)ptr;
        if (--obj->refCount <= 0) {
            // Cascade: release all field values before freeing
            for (int32_t i = 0; i < obj->fieldCount; i++) {
                rockit_release_value(obj->fields[i]);
            }
            free(obj);
        }
    } else {
        // String/List/Map: refCount is at offset 0
        int64_t* refCount = (int64_t*)ptr;
        if (--(*refCount) <= 0) {
            // Check if it has an internal allocation at offset 24 (List data / Map entries).
            // Strings use a flexible array member (inline data), so offset 24 is just string bytes.
            // Lists/Maps have: [refCount:8][size:8][capacity:8][data/entries ptr:8] = 32 bytes fixed.
            // Short strings (length < 16) have allocations < 32 bytes, so offset 24 is OOB.
            // Guard with allocation size check to prevent heap-buffer-overflow.
#ifdef __APPLE__
            size_t alloc_size = malloc_size(ptr);
#elif defined(_WIN32)
            size_t alloc_size = _msize(ptr);
#else
            size_t alloc_size = 32;  // assume safe on Linux (most allocators round up)
#endif
            int64_t potential_ptr = (alloc_size >= 32) ? *((int64_t*)((char*)ptr + 24)) : 0;
            if (is_likely_heap_ptr(potential_ptr)) {
                // Likely a List or Map — cascade release through elements.
                // Read size and capacity from offsets 8 and 16.
                int64_t size = *((int64_t*)((char*)ptr + 8));
                int64_t capacity = *((int64_t*)((char*)ptr + 16));
                int64_t* data = (int64_t*)(intptr_t)potential_ptr;
                // Release elements: for lists, each 8-byte slot is a value.
                // For maps, entries are larger structs but key/value are at predictable offsets.
                // Use size for lists (release size elements) as a safe upper bound.
                if (size > 0 && size <= capacity && capacity < 100000000) {
                    for (int64_t i = 0; i < size; i++) {
                        rockit_release_value(data[i]);
                    }
                }
                free((void*)(intptr_t)potential_ptr);
            }
            free(ptr);
        }
    }
}

// ── Runtime Type Checking ──────────────────────────────────────────────────

static const RockitTypeEntry* g_type_hierarchy = NULL;
static int32_t g_type_hierarchy_count = 0;

void rockit_set_type_hierarchy(const RockitTypeEntry* table, int32_t count) {
    g_type_hierarchy = table;
    g_type_hierarchy_count = count;
}

/// Look up the parent type of `childName` in the hierarchy table.
/// Returns NULL if no parent is found.
static const char* find_parent_type(const char* childName) {
    for (int32_t i = 0; i < g_type_hierarchy_count; i++) {
        if (strcmp(g_type_hierarchy[i].child, childName) == 0) {
            return g_type_hierarchy[i].parent;
        }
    }
    return NULL;
}

int8_t rockit_is_type(RockitObject* obj, const char* targetType) {
    if (!obj) return 0;
    const char* objType = obj->typeName;
    if (!objType) return 0;
    // Walk up the hierarchy: check objType, then its parent, grandparent, etc.
    const char* current = objType;
    while (current != NULL) {
        if (strcmp(current, targetType) == 0) return 1;
        current = find_parent_type(current);
    }
    return 0;
}

const char* rockit_object_get_type_name(RockitObject* obj) {
    if (!obj) return NULL;
    return obj->typeName;
}

int64_t rockit_object_is_type(RockitObject* obj, const char* typeName) {
    if (!obj || !typeName) return 0;
    return strcmp(obj->typeName, typeName) == 0 ? 1 : 0;
}

// ── RockitList ──────────────────────────────────────────────────────────────

RockitList* rockit_list_create(void) {
    RockitList* list = (RockitList*)malloc(sizeof(RockitList));
    list->refCount = 1;
    list->size = 0;
    list->capacity = 8;
    list->data = (int64_t*)malloc(8 * sizeof(int64_t));
    return list;
}

RockitList* rockit_list_create_filled(int64_t size, int64_t value) {
    RockitList* list = (RockitList*)malloc(sizeof(RockitList));
    list->refCount = 1;
    list->size = size;
    list->capacity = size > 0 ? size : 8;
    list->data = (int64_t*)malloc(list->capacity * sizeof(int64_t));
    if (value == 0) {
        memset(list->data, 0, size * sizeof(int64_t));
    } else {
        for (int64_t i = 0; i < size; i++) {
            list->data[i] = value;
        }
    }
    return list;
}

void rockit_list_append(RockitList* list, int64_t value) {
    rockit_retain_value(value);
    if (list->size >= list->capacity) {
        list->capacity *= 2;
        list->data = (int64_t*)realloc(list->data, list->capacity * sizeof(int64_t));
    }
    list->data[list->size++] = value;
}

int64_t rockit_list_get(RockitList* list, int64_t index) {
    if (index < 0 || index >= list->size) {
        rockit_panic("list index out of bounds");
    }
    return list->data[index];
}

void rockit_list_set(RockitList* list, int64_t index, int64_t value) {
    if (index < 0 || index >= list->size) {
        rockit_panic("list index out of bounds");
    }
    rockit_retain_value(value);
    rockit_release_value(list->data[index]);
    list->data[index] = value;
}

int64_t rockit_list_size(RockitList* list) {
    return list ? list->size : 0;
}

int8_t rockit_list_is_empty(RockitList* list) {
    return !list || list->size == 0;
}

void rockit_list_release(RockitList* list) {
    if (list && --list->refCount <= 0) {
        // Cascade: release all elements before freeing
        for (int64_t i = 0; i < list->size; i++) {
            rockit_release_value(list->data[i]);
        }
        free(list->data);
        free(list);
    }
}

void rockit_list_clear(RockitList* list) {
    if (!list) return;
    for (int64_t i = 0; i < list->size; i++) {
        rockit_release_value(list->data[i]);
    }
    list->size = 0;
}

int8_t rockit_list_contains(RockitList* list, int64_t value) {
    if (!list) return 0;
    for (int64_t i = 0; i < list->size; i++) {
        if (list->data[i] == value) return 1;
    }
    return 0;
}

int64_t rockit_list_remove_at(RockitList* list, int64_t index) {
    if (!list || index < 0 || index >= list->size) return 0;
    int64_t removed = list->data[index];
    for (int64_t i = index; i < list->size - 1; i++) {
        list->data[i] = list->data[i + 1];
    }
    list->size--;
    return removed;
}

// ── RockitMap ───────────────────────────────────────────────────────────────

RockitMap* rockit_map_create(void) {
    RockitMap* map = (RockitMap*)malloc(sizeof(RockitMap));
    map->refCount = 1;
    map->size = 0;
    map->capacity = 16;
    map->entries = (RockitMapEntry*)calloc(16, sizeof(RockitMapEntry));
    return map;
}

static int64_t map_hash(int64_t key, int64_t capacity) {
    // Must match Runtime/rockit/map.rok int_hash exactly for bootstrap
    int64_t h = key;
    if (h < 0) h = -h;
    uint64_t uh = (uint64_t)h;
    uh = uh + uh * 65599ULL;
    h = (int64_t)uh;
    if (h < 0) h = -h;
    return h % capacity;
}

static void map_grow(RockitMap* map) {
    int64_t oldCap = map->capacity;
    RockitMapEntry* oldEntries = map->entries;
    map->capacity *= 2;
    map->entries = (RockitMapEntry*)calloc(map->capacity, sizeof(RockitMapEntry));
    map->size = 0;
    for (int64_t i = 0; i < oldCap; i++) {
        if (oldEntries[i].occupied) {
            int64_t idx = map_hash(oldEntries[i].key, map->capacity);
            while (map->entries[idx].occupied) {
                idx = (idx + 1) % map->capacity;
            }
            map->entries[idx].key = oldEntries[i].key;
            map->entries[idx].value = oldEntries[i].value;
            map->entries[idx].occupied = 1;
            map->size++;
        }
    }
    free(oldEntries);
}

void rockit_map_put(RockitMap* map, int64_t key, int64_t value) {
    if (map->size * 2 >= map->capacity) {
        map_grow(map);
    }
    int64_t idx = map_hash(key, map->capacity);
    while (map->entries[idx].occupied) {
        if (map->entries[idx].key == key) {
            rockit_retain_value(value);
            rockit_release_value(map->entries[idx].value);
            map->entries[idx].value = value;
            return;
        }
        idx = (idx + 1) % map->capacity;
    }
    rockit_retain_value(key);
    rockit_retain_value(value);
    map->entries[idx].key = key;
    map->entries[idx].value = value;
    map->entries[idx].occupied = 1;
    map->size++;
}

int64_t rockit_map_get(RockitMap* map, int64_t key) {
    int64_t idx = map_hash(key, map->capacity);
    int64_t start = idx;
    while (map->entries[idx].occupied) {
        if (map->entries[idx].key == key) {
            return map->entries[idx].value;
        }
        idx = (idx + 1) % map->capacity;
        if (idx == start) break;
    }
    rockit_panic("map key not found");
    return 0;
}

int8_t rockit_map_contains_key(RockitMap* map, int64_t key) {
    int64_t idx = map_hash(key, map->capacity);
    int64_t start = idx;
    while (map->entries[idx].occupied) {
        if (map->entries[idx].key == key) return 1;
        idx = (idx + 1) % map->capacity;
        if (idx == start) break;
    }
    return 0;
}

int64_t rockit_map_size(RockitMap* map) {
    return map ? map->size : 0;
}

int8_t rockit_map_is_empty(RockitMap* map) {
    return !map || map->size == 0;
}

void rockit_map_release(RockitMap* map) {
    if (map && --map->refCount <= 0) {
        // Cascade: release all occupied entries before freeing
        for (int64_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].occupied) {
                rockit_release_value(map->entries[i].key);
                rockit_release_value(map->entries[i].value);
            }
        }
        free(map->entries);
        free(map);
    }
}

RockitList* rockit_map_keys(RockitMap* map) {
    RockitList* list = rockit_list_create();
    if (!map) return list;
    for (int64_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            rockit_list_append(list, map->entries[i].key);
        }
    }
    return list;
}

RockitList* rockit_map_values(RockitMap* map) {
    RockitList* list = rockit_list_create();
    if (!map) return list;
    for (int64_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            rockit_list_append(list, map->entries[i].value);
        }
    }
    return list;
}

void rockit_map_remove(RockitMap* map, int64_t key) {
    if (!map || map->size == 0) return;
    int64_t h = map_hash(key, map->capacity);
    int64_t start = h;
    while (map->entries[h].occupied) {
        if (map->entries[h].key == key) {
            map->entries[h].occupied = 0;
            rockit_release_value(map->entries[h].key);
            rockit_release_value(map->entries[h].value);
            map->size--;
            return;
        }
        h = (h + 1) % map->capacity;
        if (h == start) break;
    }
}

// ── I/O ─────────────────────────────────────────────────────────────────────

void rockit_println_int(int64_t value) {
    printf("%lld\n", (long long)value);
}

void rockit_println_float(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    // Ensure at least one decimal place (match VM behavior: 4 → "4.0")
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        printf("%s.0\n", buf);
    } else {
        printf("%s\n", buf);
    }
}

void rockit_println_bool(int8_t value) {
    printf("%s\n", value ? "true" : "false");
}

void rockit_println_string(RockitString* s) {
    if (s) {
        fwrite(s->chars, 1, s->length, stdout);
        putchar('\n');
    } else {
        printf("null\n");
    }
}

void rockit_println_null(void) {
    printf("null\n");
}

void rockit_print_int(int64_t value) {
    printf("%lld", (long long)value);
}

void rockit_print_float(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        printf("%s.0", buf);
    } else {
        printf("%s", buf);
    }
}

void rockit_print_bool(int8_t value) {
    printf("%s", value ? "true" : "false");
}

void rockit_print_string(RockitString* s) {
    if (s) {
        fwrite(s->chars, 1, s->length, stdout);
    } else {
        printf("null");
    }
}

// ── Dynamic print (handles both string pointers and integer values) ──────

static int is_likely_string_ptr(int64_t value) {
    if (value == 0) return 0;
    uint64_t uval = (uint64_t)value;
    if (uval > 0x100000000ULL && uval < 0x800000000000ULL) {
        // Safely check if memory is readable before dereferencing
#ifdef __APPLE__
        char _probe[32];
        vm_size_t _probe_sz;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(intptr_t)value,
                32, (vm_address_t)_probe, &_probe_sz) != KERN_SUCCESS)
            return 0;
#endif
        RockitString* s = (RockitString*)(intptr_t)value;
        if (((s->refCount > 0 && s->refCount < 100000) || s->refCount == ROCKIT_IMMORTAL_REFCOUNT) &&
            s->length >= 0 && s->length < 10000000) {
            return 1;
        }
    }
    return 0;
}

void rockit_println_any(int64_t value) {
    if (value == ROCKIT_NULL) {
        printf("null\n");
    } else if (value == 0) {
        printf("0\n");
    } else if (is_likely_string_ptr(value)) {
        RockitString* s = (RockitString*)(intptr_t)value;
        fwrite(s->chars, 1, s->length, stdout);
        putchar('\n');
    } else {
        printf("%lld\n", (long long)value);
    }
}

void rockit_print_any(int64_t value) {
    if (value == ROCKIT_NULL) {
        printf("null");
    } else if (value == 0) {
        printf("0");
    } else if (is_likely_string_ptr(value)) {
        RockitString* s = (RockitString*)(intptr_t)value;
        fwrite(s->chars, 1, s->length, stdout);
    } else {
        printf("%lld", (long long)value);
    }
}

void rockit_println_double(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    printf("%s\n", buf);
}

void rockit_print_double(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    printf("%s", buf);
}

RockitString* rockit_double_to_string(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    return rockit_string_new(buf);
}

// ── Null-terminated C-string helper ──────────────────────────────────────────
// For functions that need null-terminated strings (fopen, system, etc.).
// Owned strings are already null-terminated. Slices may not be.
// Returns a pointer to a null-terminated version. If the original is already
// null-terminated, returns chars directly. Otherwise copies to buf (or heap).
// Caller must call cstr_done(result, s, buf) to free heap copies.
static inline const char* cstr(RockitString* s, char* buf, size_t bufsz) {
    // Owned and immortal strings are always null-terminated
    if (!s->base) return s->chars;
    // Slice: check if the byte after our range happens to be '\0'
    if (s->chars[s->length] == '\0') return s->chars;
    // Need a null-terminated copy
    if ((size_t)s->length < bufsz) {
        memcpy(buf, s->chars, s->length);
        buf[s->length] = '\0';
        return buf;
    }
    char* heap = (char*)malloc(s->length + 1);
    memcpy(heap, s->chars, s->length);
    heap[s->length] = '\0';
    return heap;
}
static inline void cstr_done(const char* c, RockitString* s, char* buf) {
    if (c != s->chars && c != buf) free((char*)c);
}

// ── Conversion ──────────────────────────────────────────────────────────────

RockitString* rockit_int_to_string(int64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return rockit_string_new(buf);
}

RockitString* rockit_float_to_string(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
    }
    return rockit_string_new(buf);
}

RockitString* rockit_bool_to_string(int8_t value) {
    return rockit_string_new(value ? "true" : "false");
}

// ── Exception Handling ──────────────────────────────────────────────────────

static jmp_buf rockit_exc_bufs[ROCKIT_MAX_EXC_DEPTH];
static int64_t rockit_exc_values[ROCKIT_MAX_EXC_DEPTH];
static int32_t rockit_exc_depth = 0;

void* rockit_exc_push(void) {
    if (rockit_exc_depth >= ROCKIT_MAX_EXC_DEPTH) {
        rockit_panic("try nesting too deep");
    }
    return (void*)&rockit_exc_bufs[rockit_exc_depth++];
}

void rockit_exc_pop(void) {
    if (rockit_exc_depth > 0) rockit_exc_depth--;
}

void rockit_exc_throw(int64_t value) {
    if (rockit_exc_depth <= 0) {
        // Uncaught exception — panic with a message
        fprintf(stderr, "PANIC: uncaught exception\n");
        exit(1);
    }
    int32_t idx = --rockit_exc_depth;
    rockit_exc_values[idx] = value;
    longjmp(rockit_exc_bufs[idx], 1);
}

int64_t rockit_exc_get(void) {
    // After throw, depth was decremented, so the value is at current depth
    return rockit_exc_values[rockit_exc_depth];
}

// ── Process ─────────────────────────────────────────────────────────────────

void rockit_panic(const char* message) {
    fprintf(stderr, "PANIC: %s\n", message);
    exit(1);
}

// ── String Comparison ────────────────────────────────────────────────────────

int8_t rockit_string_eq(int64_t a, int64_t b) {
    if (a == b) return 1;  // Same pointer, same integer, or both null sentinel
    // Null sentinel is only equal to itself (handled above)
    if (a == ROCKIT_NULL || b == ROCKIT_NULL) return 0;
    if (a == 0 || b == 0) return 0;
    // Both must look like valid string pointers for content comparison
    if (!is_likely_string_ptr(a) || !is_likely_string_ptr(b)) return 0;
    RockitString* sa = (RockitString*)(intptr_t)a;
    RockitString* sb = (RockitString*)(intptr_t)b;
    if (sa->length != sb->length) return 0;
    return memcmp(sa->chars, sb->chars, sa->length) == 0;
}

int8_t rockit_string_neq(int64_t a, int64_t b) {
    return !rockit_string_eq(a, b);
}

// ── Builtin Wrappers ────────────────────────────────────────────────────────
// These are called directly by LLVM IR generated from Rockit source.
// Stage 1 stores all values as i64 (pointers cast to int64_t).
// These wrappers bridge between the i64 ABI and the typed C runtime.

// -- String operations --

RockitString* charAt(RockitString* s, int64_t index) {
    if (!s || index < 0 || index >= s->length) {
        return rockit_string_new("");
    }
    char buf[2] = { s->chars[index], '\0' };
    return rockit_string_new(buf);
}

int64_t charCodeAt(RockitString* s, int64_t index) {
    if (!s || index < 0 || index >= s->length) return 0;
    return (int64_t)(unsigned char)s->chars[index];
}

RockitString* intToChar(int64_t code) {
    if (code < 0 || code > 127) {
        return rockit_string_new("");
    }
    char buf[2] = { (char)code, '\0' };
    return rockit_string_new(buf);
}

int8_t startsWith(RockitString* s, RockitString* prefix) {
    if (!s || !prefix) return 0;
    if (prefix->length > s->length) return 0;
    return memcmp(s->chars, prefix->chars, prefix->length) == 0;
}

int8_t endsWith(RockitString* s, RockitString* suffix) {
    if (!s || !suffix) return 0;
    if (suffix->length > s->length) return 0;
    return memcmp(s->chars + s->length - suffix->length, suffix->chars, suffix->length) == 0;
}

RockitString* stringConcat(RockitString* a, RockitString* b) {
    return rockit_string_concat(a, b);
}

int64_t stringIndexOf(RockitString* s, RockitString* needle) {
    if (!s || !needle) return -1;
    if (needle->length == 0) return 0;
    if (needle->length > s->length) return -1;
    for (int64_t i = 0; i <= s->length - needle->length; i++) {
        if (memcmp(s->chars + i, needle->chars, needle->length) == 0) {
            return i;
        }
    }
    return -1;
}

int64_t stringLength(RockitString* s) {
    return s ? s->length : 0;
}

RockitString* stringTrim(RockitString* s) {
    if (!s || s->length == 0) return rockit_string_new("");
    int64_t start = 0;
    while (start < s->length && (s->chars[start] == ' ' || s->chars[start] == '\t' ||
           s->chars[start] == '\n' || s->chars[start] == '\r')) start++;
    int64_t end = s->length - 1;
    while (end > start && (s->chars[end] == ' ' || s->chars[end] == '\t' ||
           s->chars[end] == '\n' || s->chars[end] == '\r')) end--;
    int64_t len = end - start + 1;
    RockitString* result = string_alloc(len);
    result->refCount = 1;
    result->length = len;
    result->base = NULL;
    result->capacity = (len <= POOL_MAX_LEN) ? POOL_MAX_LEN : len;
    memcpy(result->data, s->chars + start, len);
    result->data[len] = '\0';
    result->chars = result->data;
    return result;
}

RockitString* substring(RockitString* s, int64_t start, int64_t end) {

    if (!s) return rockit_string_new("");
    if (start < 0) start = 0;
    if (end > s->length) end = s->length;
    if (start >= end) return rockit_string_new("");
    // Zero-copy slice: point into the source string's character data.
    RockitString* result = slice_alloc();
    result->refCount = 1;
    result->length = end - start;
    result->chars = s->chars + start;
    // Chain to root: if source is itself a slice, reference its base.
    result->base = s->base ? s->base : s;
    rockit_string_retain(result->base);
    return result;
}

int64_t toInt(int64_t value) {
    // If the value looks like a string pointer, parse the string as an integer.
    // Otherwise return the value as-is (it's already an integer).
    if (value == ROCKIT_NULL) return 0;
    if (value == 0) return 0;
    if (is_likely_string_ptr(value)) {
        RockitString* s = (RockitString*)(intptr_t)value;
        // Use stack buffer for slices (may not be null-terminated)
        char buf[32];
        int64_t len = s->length < 31 ? s->length : 31;
        memcpy(buf, s->chars, len);
        buf[len] = '\0';
        return strtoll(buf, NULL, 10);
    }
    return value;
}

// -- Character checks --

int8_t isDigit(RockitString* ch) {
    if (!ch || ch->length == 0) return 0;
    char c = ch->chars[0];
    return c >= '0' && c <= '9';
}

int8_t isLetter(RockitString* ch) {
    if (!ch || ch->length == 0) return 0;
    char c = ch->chars[0];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int8_t isLetterOrDigit(RockitString* ch) {
    if (!ch || ch->length == 0) return 0;
    char c = ch->chars[0];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// -- Type checks --

int8_t isMap(int64_t val) {
    // In the VM, maps are tagged values. In native, we treat non-null as "is a map"
    // when used in Stage 1 context. This is a heuristic — Stage 1 uses isMap to check
    // if an AST node is a map (non-null pointer).
    return val != 0 && val != ROCKIT_NULL;
}

// -- List operations (i64 wrapper API) --

int64_t listCreate(void) {
    return (int64_t)(intptr_t)rockit_list_create();
}

int64_t listCreateFilled(int64_t size, int64_t value) {
    return (int64_t)(intptr_t)rockit_list_create_filled(size, value);
}

void listAppend(int64_t list, int64_t value) {
    rockit_list_append((RockitList*)(intptr_t)list, value);
}

int64_t listGet(int64_t list, int64_t index) {
    return rockit_list_get((RockitList*)(intptr_t)list, index);
}

void listSet(int64_t list, int64_t index, int64_t value) {
    rockit_list_set((RockitList*)(intptr_t)list, index, value);
}

int64_t listSize(int64_t list) {
    return rockit_list_size((RockitList*)(intptr_t)list);
}

int8_t listContains(int64_t list, int64_t value) {
    RockitList* l = (RockitList*)(intptr_t)list;
    if (!l) return 0;
    for (int64_t i = 0; i < l->size; i++) {
        if (l->data[i] == value) return 1;
    }
    return 0;
}

int64_t listRemoveAt(int64_t list, int64_t index) {
    RockitList* l = (RockitList*)(intptr_t)list;
    if (!l || index < 0 || index >= l->size) return ROCKIT_NULL;
    int64_t removed = l->data[index];
    for (int64_t i = index; i < l->size - 1; i++) {
        l->data[i] = l->data[i + 1];
    }
    l->size--;
    return removed;
}

// -- Map operations (i64 wrapper API) --

int64_t mapCreate(void);  // defined after mapCreate_string below

// String-keyed map — hash by string content, not pointer address
static uint64_t string_hash(RockitString* s) {
    // Must match Runtime/rockit/string.rok string_hash exactly for bootstrap
    if (!s) return 0;
    uint64_t h = 2166136261ULL;
    for (int64_t i = 0; i < s->length; i++) {
        h = h * 16777619ULL + (uint64_t)(unsigned char)s->chars[i];
    }
    int64_t sh = (int64_t)h;
    if (sh < 0) sh = -sh;
    return (uint64_t)sh;
}

static int8_t string_eq(RockitString* a, RockitString* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    return memcmp(a->chars, b->chars, a->length) == 0;
}

// Stage 1 maps use RockitString* keys. We need string-content-based hashing.
// The rockit_map_* functions use integer hashing on raw pointer values, which
// won't work for string keys. So mapGet/mapPut use a separate implementation.

typedef struct StringMapEntry {
    RockitString* key;
    int64_t value;
    int8_t occupied;
} StringMapEntry;

typedef struct StringMap {
    int64_t refCount;
    int64_t size;
    int64_t capacity;
    StringMapEntry* entries;
} StringMap;

static void smap_grow(StringMap* map);

int64_t mapCreate_string(void) {
    StringMap* map = (StringMap*)malloc(sizeof(StringMap));
    map->refCount = 1;
    map->size = 0;
    map->capacity = 16;
    map->entries = (StringMapEntry*)calloc(16, sizeof(StringMapEntry));
    return (int64_t)(intptr_t)map;
}

// mapCreate creates a string-keyed map (StringMap), used by Stage 1
int64_t mapCreate(void) {
    return mapCreate_string();
}

int64_t mapPut(int64_t mapVal, RockitString* key, int64_t value) {
    StringMap* map = (StringMap*)(intptr_t)mapVal;
    if (!map || !key || !map->entries) return 0;
    if (map->size * 2 >= map->capacity) {
        smap_grow(map);
    }
    uint64_t h = string_hash(key) % (uint64_t)map->capacity;
    while (map->entries[h].occupied) {
        if (string_eq(map->entries[h].key, key)) {
            rockit_retain_value(value);
            rockit_release_value(map->entries[h].value);
            map->entries[h].value = value;
            return 0;
        }
        h = (h + 1) % (uint64_t)map->capacity;
    }
    rockit_string_retain(key);
    rockit_retain_value(value);
    map->entries[h].key = key;
    map->entries[h].value = value;
    map->entries[h].occupied = 1;
    map->size++;
    return 0;
}

int64_t mapGet(int64_t mapVal, RockitString* key) {
    StringMap* map = (StringMap*)(intptr_t)mapVal;
    if (!map || !key || !map->entries) return ROCKIT_NULL;
    uint64_t h = string_hash(key) % (uint64_t)map->capacity;
    uint64_t start = h;
    while (map->entries[h].occupied) {
        if (string_eq(map->entries[h].key, key)) {
            return map->entries[h].value;
        }
        h = (h + 1) % (uint64_t)map->capacity;
        if (h == start) break;
    }
    return ROCKIT_NULL;  // Not found — return null sentinel
}

int64_t mapKeys(int64_t mapVal) {
    StringMap* map = (StringMap*)(intptr_t)mapVal;
    int64_t list = listCreate();
    if (!map) return list;
    for (int64_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            listAppend(list, (int64_t)(intptr_t)map->entries[i].key);
        }
    }
    return list;
}

static void smap_grow(StringMap* map) {
    int64_t oldCap = map->capacity;
    StringMapEntry* oldEntries = map->entries;
    map->capacity *= 2;
    map->entries = (StringMapEntry*)calloc(map->capacity, sizeof(StringMapEntry));
    map->size = 0;
    for (int64_t i = 0; i < oldCap; i++) {
        if (oldEntries[i].occupied) {
            uint64_t h = string_hash(oldEntries[i].key) % (uint64_t)map->capacity;
            while (map->entries[h].occupied) {
                h = (h + 1) % (uint64_t)map->capacity;
            }
            map->entries[h].key = oldEntries[i].key;
            map->entries[h].value = oldEntries[i].value;
            map->entries[h].occupied = 1;
            map->size++;
        }
    }
    free(oldEntries);
}

// -- I/O operations --

RockitString* readLine(void) {
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin)) {
        // Strip trailing newline
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        return rockit_string_new(buf);
    }
    return rockit_string_new("");
}

int8_t fileExists(RockitString* path) {
    if (!path) return 0;
    char buf[1024];
    const char* c = cstr(path, buf, sizeof(buf));
    FILE* f = fopen(c, "r");
    cstr_done(c, path, buf);
    if (f) { fclose(f); return 1; }
    return 0;
}

RockitString* fileRead(RockitString* path) {
    if (!path) return rockit_string_new("");
    char pbuf[1024];
    const char* c = cstr(path, pbuf, sizeof(pbuf));
    FILE* f = fopen(c, "rb");
    cstr_done(c, path, pbuf);
    if (!f) return rockit_string_new("");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(size + 1);
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);
    RockitString* result = rockit_string_new(buf);
    free(buf);
    return result;
}

int64_t fileWriteBytes(RockitString* path, int64_t bytesListVal) {
    if (!path) return 0;
    RockitList* bytes = (RockitList*)(intptr_t)bytesListVal;
    if (!bytes) return 0;
    char pbuf[1024];
    const char* c = cstr(path, pbuf, sizeof(pbuf));
    FILE* f = fopen(c, "wb");
    cstr_done(c, path, pbuf);
    if (!f) return 0;
    for (int64_t i = 0; i < bytes->size; i++) {
        uint8_t b = (uint8_t)(bytes->data[i] & 0xFF);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return bytes->size;
}

// -- Process operations --

static int s_argc = 0;
static char** s_argv = NULL;

void rockit_set_args(int argc, char** argv) {
    s_argc = argc;
    s_argv = argv;
}

int64_t processArgs(void) {
    int64_t list = listCreate();
    // Skip argv[0] (binary name) — return user arguments only
    for (int i = 1; i < s_argc; i++) {
        RockitString* s = rockit_string_new(s_argv[i]);
        listAppend(list, (int64_t)(intptr_t)s);
    }
    return list;
}

int64_t getEnv(RockitString* name) {
    const char* val = getenv(name->data);
    if (val == NULL) {
        return (int64_t)(intptr_t)rockit_string_new("");
    }
    return (int64_t)(intptr_t)rockit_string_new(val);
}

int64_t executablePath(void) {
#ifdef _WIN32
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
        return (int64_t)(intptr_t)rockit_string_new(path);
    }
#endif
    if (s_argc > 0 && s_argv != NULL && s_argv[0] != NULL) {
        return (int64_t)(intptr_t)rockit_string_new(s_argv[0]);
    }
    return (int64_t)(intptr_t)rockit_string_new("");
}

// -- Platform detection --

int64_t platformOS(void) {
#ifdef _WIN32
    return (int64_t)(intptr_t)rockit_string_new("windows");
#elif defined(__APPLE__)
    return (int64_t)(intptr_t)rockit_string_new("macos");
#else
    return (int64_t)(intptr_t)rockit_string_new("linux");
#endif
}

// -- Meta --

int64_t evalRockit(RockitString* source) {
    // evalRockit is a VM-specific feature — in native mode it's a no-op
    fprintf(stderr, "warning: evalRockit is not supported in native mode\n");
    return 0;
}

// -- Process exit --

void processExit(int64_t code) {
    exit((int)code);
}

// -- Shell execution (used by Stage 1 build-native) --

int64_t systemExec(RockitString* cmd) {
    if (!cmd) return -1;
    char buf[4096];
    const char* c = cstr(cmd, buf, sizeof(buf));
    int64_t result = (int64_t)system(c);
    cstr_done(c, cmd, buf);
    return result;
}

// -- File deletion (cross-platform, replaces shell `rm -f`) --

int64_t fileDelete(RockitString* path) {
    if (!path) return 0;
    char buf[1024];
    const char* c = cstr(path, buf, sizeof(buf));
    int64_t result = remove(c) == 0 ? 1 : 0;
    cstr_done(c, path, buf);
    return result;
}

// -- toString wrapper (used by Stage 1) --

RockitString* toString(int64_t value) {
    // In Stage 1, toString is called on various values.
    // If the value looks like a pointer to a RockitString, return it.
    // Otherwise convert the integer to string.
    if (value == ROCKIT_NULL) return rockit_string_new("null");
    if (value == 0) return rockit_int_to_string(value);  // integer 0, not null
    if (is_likely_string_ptr(value)) {
        RockitString* s = (RockitString*)(intptr_t)value;
        return s;
    }
    return rockit_int_to_string(value);
}

// -- floatToString (used by float codegen) --

RockitString* floatToString(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    return rockit_string_new(buf);
}

RockitString* formatFloat(double value, int64_t decimals) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", (int)decimals, value);
    return rockit_string_new(buf);
}

double toFloat(int64_t value) {
    return (double)value;
}

// Double-typed list access (bitcast between int64_t and double)
void listSetFloat(RockitList* list, int64_t index, double value) {
    if (index < 0 || index >= list->size) return;
    memcpy(&list->data[index], &value, sizeof(double));
}

double listGetFloat(RockitList* list, int64_t index) {
    if (index < 0 || index >= list->size) return 0.0;
    double value;
    memcpy(&value, &list->data[index], sizeof(double));
    return value;
}

// ── Actor Runtime (Stage 0 — synchronous) ──────────────────────────────

RockitActor* rockit_actor_create(const char* typeName, int32_t fieldCount) {
    RockitActor* actor = (RockitActor*)malloc(sizeof(RockitActor));
    actor->object = rockit_object_alloc(typeName, fieldCount);
    return actor;
}

RockitObject* rockit_actor_get_object(RockitActor* actor) {
    if (!actor) {
        rockit_panic("null actor dereference");
    }
    return actor->object;
}

void rockit_actor_release(RockitActor* actor) {
    if (actor) {
        rockit_release(actor->object);
        free(actor);
    }
}

// ── Async Runtime (Cooperative Task Scheduler) ──────────────────────────────

#define ROCKIT_COROUTINE_SUSPENDED ((int64_t)-9999)
#define ROCKIT_TASK_QUEUE_CAPACITY 4096

typedef struct {
    int64_t (*resume)(void* frame, int64_t result);
    void*   frame;
    int64_t result;
} RockitTask;

static struct {
    RockitTask tasks[ROCKIT_TASK_QUEUE_CAPACITY];
    int32_t head;
    int32_t tail;
    int32_t count;
} g_scheduler = {0};

void rockit_task_schedule(void* resume_fn, void* frame, int64_t result) {
    if (g_scheduler.count >= ROCKIT_TASK_QUEUE_CAPACITY) {
        fprintf(stderr, "rockit: task queue overflow\n");
        exit(1);
    }
    RockitTask* t = &g_scheduler.tasks[g_scheduler.tail];
    t->resume = (int64_t (*)(void*, int64_t))resume_fn;
    t->frame = frame;
    t->result = result;
    g_scheduler.tail = (g_scheduler.tail + 1) % ROCKIT_TASK_QUEUE_CAPACITY;
    g_scheduler.count++;
}

// Frame header: first fields of every continuation frame
// Layout as i64 array: [0]=state, [1]=parent_resume, [2]=parent_frame, [3]=join_counter
typedef struct {
    int64_t state;
    int64_t (*parent_resume)(void*, int64_t);
    void*   parent_frame;
    int64_t join_counter;
} RockitFrameHeader;

void* rockit_frame_alloc(int64_t size_bytes) {
    void* frame = calloc(1, (size_t)size_bytes);
    return frame;
}

void rockit_frame_free(void* frame) {
    free(frame);
}

int64_t rockit_await(void* child_resume_fn, void* child_frame,
                     void* parent_resume_fn, void* parent_frame) {
    // Set up parent continuation in child's frame header
    RockitFrameHeader* child_hdr = (RockitFrameHeader*)child_frame;
    child_hdr->parent_resume = (int64_t (*)(void*, int64_t))parent_resume_fn;
    child_hdr->parent_frame = parent_frame;
    // Schedule the child task
    rockit_task_schedule(child_resume_fn, child_frame, 0);
    return ROCKIT_COROUTINE_SUSPENDED;
}

void rockit_run_event_loop(void) {
    while (g_scheduler.count > 0) {
        RockitTask task = g_scheduler.tasks[g_scheduler.head];
        g_scheduler.head = (g_scheduler.head + 1) % ROCKIT_TASK_QUEUE_CAPACITY;
        g_scheduler.count--;

        int64_t ret = task.resume(task.frame, task.result);

        if (ret != ROCKIT_COROUTINE_SUSPENDED) {
            // Task completed — resume parent if one exists
            RockitFrameHeader* hdr = (RockitFrameHeader*)task.frame;
            if (hdr->parent_resume) {
                // Decrement parent's join counter
                RockitFrameHeader* parent_hdr = (RockitFrameHeader*)hdr->parent_frame;
                if (parent_hdr->join_counter > 0) {
                    parent_hdr->join_counter--;
                }
                rockit_task_schedule(
                    (void*)hdr->parent_resume,
                    hdr->parent_frame,
                    ret
                );
            }
            rockit_frame_free(task.frame);
        }
    }
}

int64_t rockit_is_suspended(int64_t value) {
    return value == ROCKIT_COROUTINE_SUSPENDED;
}

// ── Math Functions ──────────────────────────────────────────────────────────

double rockit_math_sqrt(double x)  { return sqrt(x); }
double rockit_math_sin(double x)   { return sin(x); }
double rockit_math_cos(double x)   { return cos(x); }
double rockit_math_tan(double x)   { return tan(x); }
double rockit_math_pow(double base, double exp) { return pow(base, exp); }
double rockit_math_floor(double x) { return floor(x); }
double rockit_math_ceil(double x)  { return ceil(x); }
double rockit_math_round(double x) { return round(x); }
double rockit_math_log(double x)   { return log(x); }
double rockit_math_exp(double x)   { return exp(x); }
double rockit_math_abs(double x)   { return fabs(x); }
double rockit_math_atan2(double y, double x) { return atan2(y, x); }

// ── Networking, Time & Random (Foundation builtins) ────────────────────────

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

static int random_seeded = 0;

int64_t randomInt(int64_t bound) {
    if (!random_seeded) {
        srand((unsigned int)time(NULL));
        random_seeded = 1;
    }
    if (bound <= 0) return 0;
    return (int64_t)(rand() % (int)bound);
}

int64_t currentTimeMillis(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

void sleepMillis(int64_t ms) {
    usleep((useconds_t)(ms * 1000));
}

int64_t epochToComponents(int64_t epochSec) {
    // Returns a Rockit Map with year, month, day, hour, minute, second
    // Use the runtime's mapCreate/mapPut
    int64_t map = mapCreate();
    time_t t = (time_t)epochSec;
    struct tm *tm = gmtime(&t);
    if (tm) {
        mapPut(map, rockit_string_new("year"), (int64_t)(tm->tm_year + 1900));
        mapPut(map, rockit_string_new("month"), (int64_t)(tm->tm_mon + 1));
        mapPut(map, rockit_string_new("day"), (int64_t)tm->tm_mday);
        mapPut(map, rockit_string_new("hour"), (int64_t)tm->tm_hour);
        mapPut(map, rockit_string_new("minute"), (int64_t)tm->tm_min);
        mapPut(map, rockit_string_new("second"), (int64_t)tm->tm_sec);
    }
    return map;
}

int64_t tcpConnect(const char *host_str, int64_t port) {
    // host_str is a Rockit string pointer — extract chars
    if (!host_str) return -1;
    int64_t *hdr = (int64_t *)host_str;
    int64_t len = hdr[1];
    char *chars = (char *)hdr[2];

    // Null-terminate the hostname
    char hostname[256];
    if (len >= 256) len = 255;
    memcpy(hostname, chars, len);
    hostname[len] = '\0';

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", (int)port);

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, portstr, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return (int64_t)fd;
}

int64_t tcpSend(int64_t fd, const char *msg_str) {
    if (!msg_str) return 0;
    int64_t *hdr = (int64_t *)msg_str;
    int64_t len = hdr[1];
    char *chars = (char *)hdr[2];
    ssize_t sent = send((int)fd, chars, (size_t)len, 0);
    return (int64_t)sent;
}

RockitString *tcpRecv(int64_t fd, int64_t maxBytes) {
    char *buf = malloc(maxBytes + 1);
    if (!buf) return rockit_string_new("");
    ssize_t n = recv((int)fd, buf, (size_t)maxBytes, 0);
    if (n <= 0) { free(buf); return rockit_string_new(""); }
    buf[n] = '\0';
    RockitString *result = rockit_string_new(buf);
    free(buf);
    return result;
}

void tcpClose(int64_t fd) {
    close((int)fd);
}

// ── OpenSSL: TLS, Crypto, X.509 Builtins ────────────────────────────────────

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

// Handle tables
static SSL_CTX* tls_ctx_table[256] = {0};
static SSL*     tls_ssl_table[1024] = {0};
static int      tls_fd_table[1024] = {0};
static X509*    tls_x509_table[256] = {0};
static int      tls_initialized = 0;

static void tls_ensure_init(void) {
    if (!tls_initialized) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        tls_initialized = 1;
    }
}

// Helper: extract chars/len from Rockit string pointer
static void rockit_str_extract(const char *str, char **out_chars, int64_t *out_len) {
    int64_t *hdr = (int64_t *)str;
    *out_len = hdr[1];
    *out_chars = (char *)hdr[2];
}

// Helper: build a RockitString from raw bytes (handles embedded nulls)
static RockitString* rockit_string_from_bytes(const unsigned char *data, int64_t len) {
    RockitString *s = string_alloc(len);
    s->refCount = 1;
    s->length = len;
    s->base = NULL;
    s->capacity = (len <= POOL_MAX_LEN) ? POOL_MAX_LEN : len;
    memcpy(s->data, data, len);
    s->data[len] = '\0';
    s->chars = s->data;
    return s;
}

// ── TLS Context Management ──────────────────────────────────────────────────

int64_t tlsCreateContext(void) {
    tls_ensure_init();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_default_verify_paths(ctx);
    for (int i = 0; i < 256; i++) {
        if (!tls_ctx_table[i]) {
            tls_ctx_table[i] = ctx;
            return (int64_t)i;
        }
    }
    SSL_CTX_free(ctx);
    return -1;
}

int64_t tlsCreateServerContext(void) {
    tls_ensure_init();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return -1;
    for (int i = 0; i < 256; i++) {
        if (!tls_ctx_table[i]) {
            tls_ctx_table[i] = ctx;
            return (int64_t)i;
        }
    }
    SSL_CTX_free(ctx);
    return -1;
}

int64_t tlsSetCertificate(int64_t ctx_id, const char *path_str) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    char *chars; int64_t len;
    rockit_str_extract(path_str, &chars, &len);
    char pathbuf[1024];
    if (len >= 1024) len = 1023;
    memcpy(pathbuf, chars, len);
    pathbuf[len] = '\0';
    return SSL_CTX_use_certificate_file(tls_ctx_table[ctx_id], pathbuf, SSL_FILETYPE_PEM) == 1 ? 0 : -1;
}

int64_t tlsSetPrivateKey(int64_t ctx_id, const char *path_str) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    char *chars; int64_t len;
    rockit_str_extract(path_str, &chars, &len);
    char pathbuf[1024];
    if (len >= 1024) len = 1023;
    memcpy(pathbuf, chars, len);
    pathbuf[len] = '\0';
    return SSL_CTX_use_PrivateKey_file(tls_ctx_table[ctx_id], pathbuf, SSL_FILETYPE_PEM) == 1 ? 0 : -1;
}

void tlsSetVerifyPeer(int64_t ctx_id, int64_t verify) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return;
    SSL_CTX_set_verify(tls_ctx_table[ctx_id],
                       verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
}

int64_t tlsSetAlpn(int64_t ctx_id, const char *protos_str) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    char *chars; int64_t len;
    rockit_str_extract(protos_str, &chars, &len);
    // protos_str is already in wire format: length-prefixed protocol names
    return SSL_CTX_set_alpn_protos(tls_ctx_table[ctx_id],
                                   (const unsigned char *)chars, (unsigned int)len) == 0 ? 0 : -1;
}

// ── TLS Connection ──────────────────────────────────────────────────────────

int64_t tlsConnect(int64_t ctx_id, const char *host_str, int64_t port) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    char *chars; int64_t len;
    rockit_str_extract(host_str, &chars, &len);

    char hostname[256];
    if (len >= 256) len = 255;
    memcpy(hostname, chars, len);
    hostname[len] = '\0';

    // TCP connect (reuse the same pattern as tcpConnect)
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", (int)port);
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, portstr, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    // SSL handshake
    SSL *ssl = SSL_new(tls_ctx_table[ctx_id]);
    if (!ssl) { close(fd); return -1; }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, hostname);

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); close(fd); return -1;
    }

    // Store in table
    for (int i = 0; i < 1024; i++) {
        if (!tls_ssl_table[i]) {
            tls_ssl_table[i] = ssl;
            tls_fd_table[i] = fd;
            return (int64_t)i;
        }
    }
    SSL_shutdown(ssl); SSL_free(ssl); close(fd);
    return -1;
}

int64_t tlsSend(int64_t conn_id, const char *data_str) {
    if (conn_id < 0 || conn_id >= 1024 || !tls_ssl_table[conn_id]) return -1;
    char *chars; int64_t len;
    rockit_str_extract(data_str, &chars, &len);
    int n = SSL_write(tls_ssl_table[conn_id], chars, (int)len);
    return (int64_t)n;
}

RockitString* tlsRecv(int64_t conn_id, int64_t maxBytes) {
    if (conn_id < 0 || conn_id >= 1024 || !tls_ssl_table[conn_id])
        return rockit_string_new("");
    char *buf = malloc(maxBytes + 1);
    if (!buf) return rockit_string_new("");
    int n = SSL_read(tls_ssl_table[conn_id], buf, (int)maxBytes);
    if (n <= 0) { free(buf); return rockit_string_new(""); }
    RockitString *result = rockit_string_from_bytes((unsigned char *)buf, n);
    free(buf);
    return result;
}

void tlsClose(int64_t conn_id) {
    if (conn_id < 0 || conn_id >= 1024 || !tls_ssl_table[conn_id]) return;
    SSL_shutdown(tls_ssl_table[conn_id]);
    SSL_free(tls_ssl_table[conn_id]);
    close(tls_fd_table[conn_id]);
    tls_ssl_table[conn_id] = NULL;
    tls_fd_table[conn_id] = 0;
}

RockitString* tlsGetAlpn(int64_t conn_id) {
    if (conn_id < 0 || conn_id >= 1024 || !tls_ssl_table[conn_id])
        return rockit_string_new("");
    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(tls_ssl_table[conn_id], &proto, &proto_len);
    if (!proto || proto_len == 0) return rockit_string_new("");
    return rockit_string_from_bytes(proto, (int64_t)proto_len);
}

int64_t tlsGetPeerCert(int64_t conn_id) {
    if (conn_id < 0 || conn_id >= 1024 || !tls_ssl_table[conn_id]) return -1;
    X509 *cert = SSL_get1_peer_certificate(tls_ssl_table[conn_id]);
    if (!cert) return -1;
    for (int i = 0; i < 256; i++) {
        if (!tls_x509_table[i]) {
            tls_x509_table[i] = cert;
            return (int64_t)i;
        }
    }
    X509_free(cert);
    return -1;
}

// ── TLS Server ──────────────────────────────────────────────────────────────

int64_t tlsListen(int64_t ctx_id, int64_t port) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 128) < 0) { close(fd); return -1; }
    return (int64_t)fd;
}

int64_t tlsAccept(int64_t ctx_id, int64_t server_fd) {
    if (ctx_id < 0 || ctx_id >= 256 || !tls_ctx_table[ctx_id]) return -1;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept((int)server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) return -1;

    SSL *ssl = SSL_new(tls_ctx_table[ctx_id]);
    if (!ssl) { close(client_fd); return -1; }
    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl); close(client_fd); return -1;
    }

    for (int i = 0; i < 1024; i++) {
        if (!tls_ssl_table[i]) {
            tls_ssl_table[i] = ssl;
            tls_fd_table[i] = client_fd;
            return (int64_t)i;
        }
    }
    SSL_shutdown(ssl); SSL_free(ssl); close(client_fd);
    return -1;
}

// ── Crypto: Hashing ─────────────────────────────────────────────────────────

static RockitString* crypto_digest(const char *data_str, const EVP_MD *md) {
    char *chars; int64_t len;
    rockit_str_extract(data_str, &chars, &len);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_Digest(chars, (size_t)len, hash, &hash_len, md, NULL);
    return rockit_string_from_bytes(hash, (int64_t)hash_len);
}

RockitString* cryptoSha256(const char *data_str) {
    return crypto_digest(data_str, EVP_sha256());
}

RockitString* cryptoSha1(const char *data_str) {
    return crypto_digest(data_str, EVP_sha1());
}

RockitString* cryptoSha512(const char *data_str) {
    return crypto_digest(data_str, EVP_sha512());
}

RockitString* cryptoMd5(const char *data_str) {
    return crypto_digest(data_str, EVP_md5());
}

// ── Crypto: HMAC ────────────────────────────────────────────────────────────

static RockitString* crypto_hmac(const char *key_str, const char *data_str, const EVP_MD *md) {
    char *key_chars; int64_t key_len;
    rockit_str_extract(key_str, &key_chars, &key_len);
    char *data_chars; int64_t data_len;
    rockit_str_extract(data_str, &data_chars, &data_len);
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(md, key_chars, (int)key_len, (unsigned char *)data_chars, (size_t)data_len,
         result, &result_len);
    return rockit_string_from_bytes(result, (int64_t)result_len);
}

RockitString* cryptoHmacSha256(const char *key_str, const char *data_str) {
    return crypto_hmac(key_str, data_str, EVP_sha256());
}

RockitString* cryptoHmacSha1(const char *key_str, const char *data_str) {
    return crypto_hmac(key_str, data_str, EVP_sha1());
}

// ── Crypto: Random ──────────────────────────────────────────────────────────

RockitString* cryptoRandomBytes(int64_t count) {
    if (count <= 0) return rockit_string_new("");
    unsigned char *buf = malloc(count);
    if (!buf) return rockit_string_new("");
    RAND_bytes(buf, (int)count);
    RockitString *result = rockit_string_from_bytes(buf, count);
    free(buf);
    return result;
}

// ── Crypto: AES ─────────────────────────────────────────────────────────────

RockitString* cryptoAesEncrypt(const char *key_str, const char *iv_str,
                                const char *data_str, int64_t mode) {
    char *key_chars; int64_t key_len;
    rockit_str_extract(key_str, &key_chars, &key_len);
    char *iv_chars; int64_t iv_len;
    rockit_str_extract(iv_str, &iv_chars, &iv_len);
    char *data_chars; int64_t data_len;
    rockit_str_extract(data_str, &data_chars, &data_len);

    const EVP_CIPHER *cipher;
    if (mode == 1) {
        cipher = (key_len == 32) ? EVP_aes_256_gcm() : EVP_aes_128_gcm();
    } else {
        cipher = (key_len == 32) ? EVP_aes_256_cbc() : EVP_aes_128_cbc();
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return rockit_string_new("");

    EVP_EncryptInit_ex(ctx, cipher, NULL, (unsigned char *)key_chars, (unsigned char *)iv_chars);

    int out_len = 0, final_len = 0;
    int buf_size = (int)data_len + EVP_CIPHER_block_size(cipher);
    unsigned char *out = malloc(buf_size + 16); // +16 for GCM tag
    if (!out) { EVP_CIPHER_CTX_free(ctx); return rockit_string_new(""); }

    EVP_EncryptUpdate(ctx, out, &out_len, (unsigned char *)data_chars, (int)data_len);
    EVP_EncryptFinal_ex(ctx, out + out_len, &final_len);
    int total = out_len + final_len;

    if (mode == 1) {
        // Append GCM tag
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + total);
        total += 16;
    }

    EVP_CIPHER_CTX_free(ctx);
    RockitString *result = rockit_string_from_bytes(out, total);
    free(out);
    return result;
}

RockitString* cryptoAesDecrypt(const char *key_str, const char *iv_str,
                                const char *data_str, int64_t mode) {
    char *key_chars; int64_t key_len;
    rockit_str_extract(key_str, &key_chars, &key_len);
    char *iv_chars; int64_t iv_len;
    rockit_str_extract(iv_str, &iv_chars, &iv_len);
    char *data_chars; int64_t data_len;
    rockit_str_extract(data_str, &data_chars, &data_len);

    const EVP_CIPHER *cipher;
    if (mode == 1) {
        cipher = (key_len == 32) ? EVP_aes_256_gcm() : EVP_aes_128_gcm();
    } else {
        cipher = (key_len == 32) ? EVP_aes_256_cbc() : EVP_aes_128_cbc();
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return rockit_string_new("");

    int cipher_len = (int)data_len;
    if (mode == 1) {
        // Last 16 bytes are GCM tag
        cipher_len -= 16;
        if (cipher_len < 0) { EVP_CIPHER_CTX_free(ctx); return rockit_string_new(""); }
    }

    EVP_DecryptInit_ex(ctx, cipher, NULL, (unsigned char *)key_chars, (unsigned char *)iv_chars);

    if (mode == 1) {
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                            (unsigned char *)data_chars + cipher_len);
    }

    int out_len = 0, final_len = 0;
    unsigned char *out = malloc(cipher_len + EVP_CIPHER_block_size(cipher));
    if (!out) { EVP_CIPHER_CTX_free(ctx); return rockit_string_new(""); }

    EVP_DecryptUpdate(ctx, out, &out_len, (unsigned char *)data_chars, cipher_len);
    int ok = EVP_DecryptFinal_ex(ctx, out + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ok != 1) { free(out); return rockit_string_new(""); }
    int total = out_len + final_len;
    RockitString *result = rockit_string_from_bytes(out, total);
    free(out);
    return result;
}

// ── X.509 Certificate Inspection ────────────────────────────────────────────

int64_t x509ParsePem(const char *pem_str) {
    tls_ensure_init();
    char *chars; int64_t len;
    rockit_str_extract(pem_str, &chars, &len);
    BIO *bio = BIO_new_mem_buf(chars, (int)len);
    if (!bio) return -1;
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!cert) return -1;
    for (int i = 0; i < 256; i++) {
        if (!tls_x509_table[i]) {
            tls_x509_table[i] = cert;
            return (int64_t)i;
        }
    }
    X509_free(cert);
    return -1;
}

RockitString* x509Subject(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle])
        return rockit_string_new("");
    char buf[512];
    X509_NAME_oneline(X509_get_subject_name(tls_x509_table[handle]), buf, sizeof(buf));
    return rockit_string_new(buf);
}

RockitString* x509Issuer(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle])
        return rockit_string_new("");
    char buf[512];
    X509_NAME_oneline(X509_get_issuer_name(tls_x509_table[handle]), buf, sizeof(buf));
    return rockit_string_new(buf);
}

static int64_t asn1_time_to_epoch(const ASN1_TIME *t) {
    struct tm tm_val = {0};
    if (ASN1_TIME_to_tm(t, &tm_val) != 1) return 0;
    return (int64_t)timegm(&tm_val);
}

int64_t x509NotBefore(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle]) return 0;
    return asn1_time_to_epoch(X509_get0_notBefore(tls_x509_table[handle]));
}

int64_t x509NotAfter(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle]) return 0;
    return asn1_time_to_epoch(X509_get0_notAfter(tls_x509_table[handle]));
}

RockitString* x509SerialNumber(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle])
        return rockit_string_new("");
    ASN1_INTEGER *serial = X509_get_serialNumber(tls_x509_table[handle]);
    BIGNUM *bn = ASN1_INTEGER_to_BN(serial, NULL);
    if (!bn) return rockit_string_new("");
    char *hex = BN_bn2hex(bn);
    BN_free(bn);
    if (!hex) return rockit_string_new("");
    RockitString *result = rockit_string_new(hex);
    OPENSSL_free(hex);
    return result;
}

void x509Free(int64_t handle) {
    if (handle < 0 || handle >= 256 || !tls_x509_table[handle]) return;
    X509_free(tls_x509_table[handle]);
    tls_x509_table[handle] = NULL;
}

// ── TLS Error Reporting ─────────────────────────────────────────────────────

RockitString* tlsLastError(void) {
    unsigned long err = ERR_get_error();
    if (err == 0) return rockit_string_new("");
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return rockit_string_new(buf);
}
