#include "qscriptengine.h"
#include "qscriptvalue_p.h"
#include <QtCore/qdebug.h>

static thread_local void *s_activeContext = nullptr;
static unsigned int s_qvariant_class_id = 0;

static void qvariant_finalizer(JSRuntime *rt, JSValue val) {
    void *opaque = JS_GetOpaque(val, QScriptEngine::variantClassId());
    if (opaque) {
        delete reinterpret_cast<QVariant*>(opaque);
    }
}

static JSClassDef qvariant_class_def = {
    "QVariantWrapper",
    qvariant_finalizer,
    nullptr, // gc_mark
    nullptr, // call
    nullptr  // exotic
};

QScriptEngine::QScriptEngine() : m_hasException(false) {
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *context = JS_NewContext(runtime);

    rt = runtime;
    ctx = context;
    s_activeContext = context;

    // Register QVariant Class
    JS_NewClassID(&s_qvariant_class_id);
    JS_NewClass(runtime, s_qvariant_class_id, &qvariant_class_def);
}

QScriptEngine::~QScriptEngine() {
    JSContext *context = reinterpret_cast<JSContext*>(ctx);
    JSRuntime *runtime = reinterpret_cast<JSRuntime*>(rt);
    if (s_activeContext == ctx) {
        s_activeContext = nullptr;
    }
    exceptionVal = QScriptValue(); // Clears reference
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
}

void *QScriptEngine::activeContext() {
    return s_activeContext;
}

unsigned int QScriptEngine::variantClassId() {
    return s_qvariant_class_id;
}

QScriptValue QScriptEngine::evaluate(const QString &program) {
    m_hasException = false;
    s_activeContext = ctx;
    JSContext *context = reinterpret_cast<JSContext*>(ctx);

    std::string code = program.toStdString();
    JSValue result = JS_Eval(context, code.c_str(), code.length(), "<eval>", JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        m_hasException = true;
        JSValue exception = JS_GetException(context);
        exceptionVal = QScriptValue(new QScriptValuePrivate(context, exception, true));
        JS_FreeValue(context, exception);
        return exceptionVal;
    }

    QScriptValue ret(new QScriptValuePrivate(context, result, true));
    JS_FreeValue(context, result);
    return ret;
}

bool QScriptEngine::hasUncaughtException() const {
    return m_hasException;
}

void QScriptEngine::clearExceptions() {
    if (m_hasException) {
        m_hasException = false;
        exceptionVal = QScriptValue();
    }
}

QScriptValue QScriptEngine::globalObject() const {
    JSContext *context = reinterpret_cast<JSContext*>(ctx);
    JSValue global = JS_GetGlobalObject(context);
    QScriptValue ret(new QScriptValuePrivate(context, global, true));
    JS_FreeValue(context, global);
    return ret;
}

QScriptValue QScriptEngine::newVariant(const QVariant &payload) {
    JSContext *context = reinterpret_cast<JSContext*>(ctx);
    JSValue obj = JS_NewObjectClass(context, s_qvariant_class_id);
    QVariant *copied = new QVariant(payload);
    JS_SetOpaque(obj, copied);
    QScriptValue ret(new QScriptValuePrivate(context, obj, true));
    JS_FreeValue(context, obj);
    return ret;
}
