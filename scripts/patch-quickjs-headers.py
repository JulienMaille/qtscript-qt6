import sys
import os

filepath = sys.argv[1]
if not os.path.exists(filepath):
    print(f"File not found: {filepath}")
    sys.exit(0)

with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

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
    print("Patching complete!")
elif target_crlf in content:
    print("Patching designated initializers inside quickjs.h (CRLF)...")
    content = content.replace(target_crlf, replacement_crlf)
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)
    print("Patching complete!")
else:
    print("quickjs.h is already patched or does not contain target.")
