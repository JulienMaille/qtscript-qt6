#ifndef QSCRIPTVALUE_P_H
#define QSCRIPTVALUE_P_H

#include "qscriptvalue.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "quickjs.h"
#ifdef __cplusplus
}
#endif

class QScriptValuePrivate {
public:
    JSContext *ctx;
    JSValue val;
    bool owned;

    QScriptValuePrivate() : ctx(nullptr), val(JS_UNDEFINED), owned(false) {}
    QScriptValuePrivate(JSContext *c, JSValue v, bool o = true) : ctx(c), val(v), owned(o) {
        if (owned && ctx) {
            JS_DupValue(ctx, val);
        }
    }
    ~QScriptValuePrivate() {
        if (owned && ctx) {
            JS_FreeValue(ctx, val);
        }
    }
};

#endif
