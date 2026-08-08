#include "qscriptvalue.h"
#include "qscriptvalue_p.h"
#include "qscriptengine.h"
#include <QtCore/qdebug.h>

QScriptValue::QScriptValue() : d(new QScriptValuePrivate(nullptr, JS_UNDEFINED, false)) {}

QScriptValue::QScriptValue(bool val) : d(new QScriptValuePrivate(nullptr, JS_MKVAL(JS_TAG_BOOL, val ? 1 : 0), false)) {}

QScriptValue::QScriptValue(int val) : d(new QScriptValuePrivate(nullptr, JS_NewInt32(nullptr, val), false)) {}

QScriptValue::QScriptValue(double val) : d(new QScriptValuePrivate(nullptr, JS_NewFloat64(nullptr, val), false)) {}

QScriptValue::QScriptValue(const QString &val) {
    JSContext *ctx = reinterpret_cast<JSContext*>(QScriptEngine::activeContext());
    if (ctx) {
        JSValue jsStr = JS_NewString(ctx, val.toUtf8().constData());
        d = QSharedPointer<QScriptValuePrivate>::create(ctx, jsStr, true);
        JS_FreeValue(ctx, jsStr);
    } else {
        d = QSharedPointer<QScriptValuePrivate>::create(nullptr, JS_UNDEFINED, false);
    }
}

QScriptValue::QScriptValue(const char *val) {
    JSContext *ctx = reinterpret_cast<JSContext*>(QScriptEngine::activeContext());
    if (ctx) {
        JSValue jsStr = JS_NewString(ctx, val);
        d = QSharedPointer<QScriptValuePrivate>::create(ctx, jsStr, true);
        JS_FreeValue(ctx, jsStr);
    } else {
        d = QSharedPointer<QScriptValuePrivate>::create(nullptr, JS_UNDEFINED, false);
    }
}

QScriptValue::QScriptValue(QScriptValuePrivate *pPrivate) : d(pPrivate) {}

QScriptValue::QScriptValue(const QScriptValue &other) : d(other.d) {}

QScriptValue &QScriptValue::operator=(const QScriptValue &other) {
    if (this != &other) {
        d = other.d;
    }
    return *this;
}

QScriptValue::~QScriptValue() {}

bool QScriptValue::isValid() const {
    return d && d->ctx != nullptr;
}

bool QScriptValue::isBool() const {
    return d && JS_IsBool(d->val);
}

bool QScriptValue::isNumber() const {
    return d && JS_IsNumber(d->val);
}

bool QScriptValue::isString() const {
    return d && JS_IsString(d->val);
}

bool QScriptValue::isObject() const {
    return d && JS_IsObject(d->val);
}

bool QScriptValue::isUndefined() const {
    return d && JS_IsUndefined(d->val);
}

bool QScriptValue::isNull() const {
    return d && JS_IsNull(d->val);
}

bool QScriptValue::isError() const {
    return d && d->ctx && JS_IsError(d->ctx, d->val);
}

bool QScriptValue::toBool() const {
    if (!d) return false;
    return JS_ToBool(d->ctx, d->val);
}

double QScriptValue::toNumber() const {
    if (!d || !d->ctx) return 0.0;
    double val = 0.0;
    JS_ToFloat64(d->ctx, &val, d->val);
    return val;
}

int QScriptValue::toInt32() const {
    if (!d || !d->ctx) return 0;
    int32_t val = 0;
    JS_ToInt32(d->ctx, &val, d->val);
    return val;
}

QString QScriptValue::toString() const {
    if (!d || !d->ctx) return QString();
    if (JS_IsUndefined(d->val)) return QStringLiteral("undefined");
    if (JS_IsNull(d->val)) return QStringLiteral("null");

    size_t len = 0;
    const char *str = JS_ToCStringLen(d->ctx, &len, d->val);
    if (!str) return QString();
    QString result = QString::fromUtf8(str, len);
    JS_FreeCString(d->ctx, str);
    return result;
}

QVariant QScriptValue::toVariant() const {
    if (!d || !d->ctx) return QVariant();
    if (isBool()) return toBool();
    if (isNumber()) return toNumber();
    if (isString()) return toString();

    // QVariant opaque object check
    if (isObject()) {
        void *opaque = JS_GetOpaque(d->val, QScriptEngine::variantClassId());
        if (opaque) {
            return *reinterpret_cast<QVariant*>(opaque);
        }
    }
    return QVariant();
}

QScriptValue QScriptValue::call(const QScriptValue &thisObject, const QScriptValueList &args) {
    if (!d || !d->ctx || !JS_IsFunction(d->ctx, d->val)) {
        return QScriptValue();
    }

    JSValue thisVal = JS_UNDEFINED;
    if (thisObject.isValid() && thisObject.d_ptr()) {
        thisVal = thisObject.d_ptr()->val;
    }

    int argc = args.size();
    QVector<JSValue> argv(argc);
    for (int i = 0; i < argc; ++i) {
        argv[i] = args[i].d_ptr() ? args[i].d_ptr()->val : JS_UNDEFINED;
    }

    JSValue result = JS_Call(d->ctx, d->val, thisVal, argc, argv.data());
    QScriptValue ret(new QScriptValuePrivate(d->ctx, result, true));
    JS_FreeValue(d->ctx, result);
    return ret;
}

void QScriptValue::setProperty(const QString &name, const QScriptValue &value) {
    if (!d || !d->ctx || !JS_IsObject(d->val)) return;
    JSValue val = JS_UNDEFINED;
    if (value.d_ptr()) {
        val = JS_DupValue(d->ctx, value.d_ptr()->val);
    }
    JS_SetPropertyStr(d->ctx, d->val, name.toUtf8().constData(), val);
}

QScriptValue QScriptValue::property(const QString &name) const {
    if (!d || !d->ctx || !JS_IsObject(d->val)) return QScriptValue();
    JSValue prop = JS_GetPropertyStr(d->ctx, d->val, name.toUtf8().constData());
    QScriptValue ret(new QScriptValuePrivate(d->ctx, prop, true));
    JS_FreeValue(d->ctx, prop);
    return ret;
}
