import sys
import os

def patch_file(filepath):
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    filename = os.path.basename(filepath)

    if filename == "quickjs.h":
        # 1. Patch designated initializers in quickjs.h
        target = """#define JS_MKVAL(tag, val) (JSValue){ (JSValueUnion){ .int32 = val }, tag }
#define JS_MKPTR(tag, p) (JSValue){ (JSValueUnion){ .ptr = p }, tag }

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN (JSValue){ .u.float64 = JS_FLOAT64_NAN, JS_TAG_FLOAT64 }"""

        replacement = """#ifdef __cplusplus
static inline JSValue JS_MKVAL(int64_t tag, int32_t val) {
    JSValue v;
    v.u.int32 = val;
    v.tag = tag;
    return v;
}
static inline JSValue JS_MKPTR(int64_t tag, void *p) {
    JSValue v;
    v.u.ptr = p;
    v.tag = tag;
    return v;
}
static inline JSValue __js_nan_init() {
    JSValue v;
    v.u.float64 = JS_FLOAT64_NAN;
    v.tag = JS_TAG_FLOAT64;
    return v;
}
#define JS_NAN __js_nan_init()
#else
#define JS_MKVAL(tag, val) (JSValue){ (JSValueUnion){ .int32 = val }, tag }
#define JS_MKPTR(tag, p) (JSValue){ (JSValueUnion){ .ptr = p }, tag }
#define JS_NAN (JSValue){ .u.float64 = JS_FLOAT64_NAN, JS_TAG_FLOAT64 }
#endif

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)"""

        # Also handle Windows style CRLF line endings
        target_crlf = target.replace('\n', '\r\n')
        replacement_crlf = replacement.replace('\n', '\r\n')

        if target in content:
            print("Patching designated initializers inside quickjs.h (LF)...")
            content = content.replace(target, replacement)
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            print("quickjs.h patching complete!")
        elif target_crlf in content:
            print("Patching designated initializers inside quickjs.h (CRLF)...")
            content = content.replace(target_crlf, replacement_crlf)
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            print("quickjs.h patching complete!")
        else:
            print("quickjs.h is already patched or does not contain target.")

    elif filename == "cutils.h":
        print("Patching cutils.h for MSVC compatibility...")

        # 1. Patch __builtin_expect, force_inline, etc.
        target_macros = """#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define force_inline inline __attribute__((always_inline))
#define no_inline __attribute__((noinline))
#define __maybe_unused __attribute__((unused))"""

        replacement_macros = """#ifdef _MSC_VER
#define likely(x)       (x)
#define unlikely(x)     (x)
#define force_inline __forceinline
#define no_inline __declspec(noinline)
#define __maybe_unused
#ifndef __attribute__
#define __attribute__(x)
#endif
#ifndef __attribute
#define __attribute(x)
#endif
#else
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define force_inline inline __attribute__((always_inline))
#define no_inline __attribute__((noinline))
#define __maybe_unused __attribute__((unused))
#endif"""

        content = content.replace(target_macros, replacement_macros)
        content = content.replace(target_macros.replace('\n', '\r\n'), replacement_macros.replace('\n', '\r\n'))

        # 2. Patch __builtin_clz / __builtin_ctz
        target_builtins = """static inline int clz32(unsigned int a)
{
    return __builtin_clz(a);
}

/* WARNING: undefined if a = 0 */
static inline int clz64(uint64_t a)
{
    return __builtin_clzll(a);
}

/* WARNING: undefined if a = 0 */
static inline int ctz32(unsigned int a)
{
    return __builtin_ctz(a);
}

/* WARNING: undefined if a = 0 */
static inline int ctz64(uint64_t a)
{
    return __builtin_ctzll(a);
}"""

        replacement_builtins = """#ifdef _MSC_VER
#include <intrin.h>
static inline int clz32(unsigned int x) {
    unsigned long index;
    if (_BitScanReverse(&index, x)) return 31 - index;
    return 32;
}
static inline int clz64(uint64_t x) {
    unsigned long index;
    if (_BitScanReverse64(&index, x)) return 63 - index;
    return 64;
}
static inline int ctz32(unsigned int x) {
    unsigned long index;
    if (_BitScanForward(&index, x)) return index;
    return 32;
}
static inline int ctz64(uint64_t x) {
    unsigned long index;
    if (_BitScanForward64(&index, x)) return index;
    return 64;
}
#else
static inline int clz32(unsigned int a) { return __builtin_clz(a); }
static inline int clz64(uint64_t a) { return __builtin_clzll(a); }
static inline int ctz32(unsigned int a) { return __builtin_ctz(a); }
static inline int ctz64(uint64_t a) { return __builtin_ctzll(a); }
#endif"""

        content = content.replace(target_builtins, replacement_builtins)
        content = content.replace(target_builtins.replace('\n', '\r\n'), replacement_builtins.replace('\n', '\r\n'))

        # 3. Patch packed struct definitions
        target_packed = """struct __attribute__((packed)) packed_u64 {
    uint64_t v;
};

struct __attribute__((packed)) packed_u32 {
    uint32_t v;
};

struct __attribute__((packed)) packed_u16 {
    uint16_t v;
};"""

        replacement_packed = """#ifdef _MSC_VER
#pragma pack(push, 1)
struct packed_u64 { uint64_t v; };
struct packed_u32 { uint32_t v; };
struct packed_u16 { uint16_t v; };
#pragma pack(pop)
#else
struct __attribute__((packed)) packed_u64 { uint64_t v; };
struct __attribute__((packed)) packed_u32 { uint32_t v; };
struct __attribute__((packed)) packed_u16 { uint16_t v; };
#endif"""

        content = content.replace(target_packed, replacement_packed)
        content = content.replace(target_packed.replace('\n', '\r\n'), replacement_packed.replace('\n', '\r\n'))

        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print("cutils.h patching complete!")

if __name__ == "__main__":
    for path in sys.argv[1:]:
        patch_file(path)
