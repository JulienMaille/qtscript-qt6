#ifndef QSCRIPTVALUE_P_H
#define QSCRIPTVALUE_P_H

#include "qscriptvalue.h"
#include <QtCore/qvariant.h>
#include <QtCore/qstring.h>
#include <QtCore/qmetatype.h>

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

// Expose Variant Class ID on the engine class to allow wrapper identification
#include "qscriptengine.h"

static inline QVariant js_value_to_qvariant(JSContext *ctx, JSValueConst val, int targetType) {
    QMetaType metaType(targetType);
    if (metaType.flags().testFlag(QMetaType::IsEnumeration)) {
        int v = 0;
        JS_ToInt32(ctx, &v, val);
        return QVariant(metaType, &v);
    }
    if (targetType == static_cast<int>(QMetaType::Int)) {
        int v = 0;
        JS_ToInt32(ctx, &v, val);
        return QVariant::fromValue(v);
    } else if (targetType == static_cast<int>(QMetaType::Double)) {
        double v = 0;
        JS_ToFloat64(ctx, &v, val);
        return QVariant::fromValue(v);
    } else if (targetType == static_cast<int>(QMetaType::Bool)) {
        bool v = JS_ToBool(ctx, val);
        return QVariant::fromValue(v);
    } else if (targetType == static_cast<int>(QMetaType::QString)) {
        size_t len = 0;
        const char *str = JS_ToCStringLen(ctx, &len, val);
        QString s = QString::fromUtf8(str, len);
        JS_FreeCString(ctx, str);
        return QVariant::fromValue(s);
    } else if (targetType == static_cast<int>(QScriptEngine::variantClassId())) {
        void *opaque = JS_GetOpaque(val, QScriptEngine::variantClassId());
        if (opaque) {
            return *reinterpret_cast<QVariant*>(opaque);
        }
    }
    // Default fallback
    return QVariant();
}

static inline JSValue qvariant_to_js_value(JSContext *ctx, const QVariant &val) {
    int type = val.userType();
    QMetaType metaType(type);
    if (metaType.flags().testFlag(QMetaType::IsEnumeration)) {
        return JS_NewInt32(ctx, val.toInt());
    }
    if (type == static_cast<int>(QMetaType::Int)) {
        return JS_NewInt32(ctx, val.toInt());
    } else if (type == static_cast<int>(QMetaType::Double)) {
        return JS_NewFloat64(ctx, val.toDouble());
    } else if (type == static_cast<int>(QMetaType::Bool)) {
        return JS_NewBool(ctx, val.toBool());
    } else if (type == static_cast<int>(QMetaType::QString)) {
        return JS_NewString(ctx, val.toString().toUtf8().constData());
    } else if (type == static_cast<int>(QScriptEngine::variantClassId()) || type >= static_cast<int>(QMetaType::User)) {
        // Fallback: wrap as a QVariant object
        JSValue obj = JS_NewObjectClass(ctx, QScriptEngine::variantClassId());
        QVariant *copied = new QVariant(val);
        JS_SetOpaque(obj, copied);
        return obj;
    } else {
        // Fallback for other standard conversions
        return JS_NewString(ctx, val.toString().toUtf8().constData());
    }
}

#endif
