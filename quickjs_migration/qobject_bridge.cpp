#include "qscriptengine.h"
#include "qscriptvalue_p.h"
#include <QtCore/qmetaobject.h>
#include <QtCore/qvector.h>
#include <QtCore/qdebug.h>

static JSValue qobject_method_caller(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *func_data) {
    unsigned int qobjId = QScriptEngine::qobjectClassId();
    void *opaque = JS_GetOpaque(this_val, qobjId);
    QObject *obj = reinterpret_cast<QObject*>(opaque);
    if (!obj) {
        opaque = JS_GetOpaque(func_data[0], qobjId);
        obj = reinterpret_cast<QObject*>(opaque);
    }
    if (!obj) return JS_UNDEFINED;

    int methodIndex = JS_VALUE_GET_INT(func_data[1]);
    QMetaMethod method = obj->metaObject()->method(methodIndex);

    int paramCount = method.parameterCount();
    int returnType = method.returnType();

    // Allocate vector of QVariants and pointers dynamically to avoid any buffer overflow or stack corruption!
    QVector<QVariant> args(paramCount + 1);
    QVector<void*> argv_qt(paramCount + 1);

    // Initialize return value space
    if (returnType != static_cast<int>(QMetaType::Void)) {
        args[0] = QVariant(QMetaType(returnType));
        argv_qt[0] = const_cast<void*>(args[0].constData());
    } else {
        argv_qt[0] = nullptr;
    }

    // Convert and bind parameters dynamically using our generic marshaller
    for (int i = 0; i < paramCount; ++i) {
        int paramType = method.parameterType(i);
        JSValueConst js_val = (i < argc) ? argv[i] : JS_UNDEFINED;
        args[i + 1] = js_value_to_qvariant(ctx, js_val, paramType);
        argv_qt[i + 1] = const_cast<void*>(args[i + 1].constData());
    }

    // Direct invocation via dynamic metacall
    QMetaObject::metacall(obj, QMetaObject::InvokeMetaMethod, methodIndex, argv_qt.data());

    // Wrap and return back to JavaScript
    if (returnType != static_cast<int>(QMetaType::Void)) {
        return qvariant_to_js_value(ctx, args[0]);
    }
    return JS_UNDEFINED;
}

static JSValue qobject_get_property(JSContext *ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    unsigned int qobjId = QScriptEngine::qobjectClassId();
    void *opaque = JS_GetOpaque(obj_val, qobjId);
    QObject *obj = reinterpret_cast<QObject*>(opaque);
    if (!obj) return JS_UNDEFINED;

    const char *name = JS_AtomToCString(ctx, atom);
    if (!name) return JS_UNDEFINED;
    QString propName = QString::fromUtf8(name);
    JS_FreeCString(ctx, name);

    const QMetaObject *meta = obj->metaObject();

    // 1. Check properties
    int propIndex = meta->indexOfProperty(propName.toUtf8().constData());
    if (propIndex >= 0) {
        QMetaProperty prop = meta->property(propIndex);
        QVariant val = prop.read(obj);
        return qvariant_to_js_value(ctx, val);
    }

    // 2. Check methods (slots/invokables)
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (QString::fromUtf8(method.name()) == propName) {
            JSValue data[2];
            data[0] = JS_DupValue(ctx, obj_val);
            data[1] = JS_NewInt32(ctx, i);
            return JS_NewCFunctionData(ctx, qobject_method_caller, 0, 0, 2, data);
        }
    }

    return JS_UNDEFINED;
}

static int qobject_set_property(JSContext *ctx, JSValueConst obj_val, JSAtom atom, JSValueConst value, JSValueConst receiver, int flags) {
    unsigned int qobjId = QScriptEngine::qobjectClassId();
    void *opaque = JS_GetOpaque(obj_val, qobjId);
    QObject *obj = reinterpret_cast<QObject*>(opaque);
    if (!obj) return -1;

    const char *name = JS_AtomToCString(ctx, atom);
    if (!name) return -1;
    QString propName = QString::fromUtf8(name);
    JS_FreeCString(ctx, name);

    const QMetaObject *meta = obj->metaObject();
    int propIndex = meta->indexOfProperty(propName.toUtf8().constData());
    if (propIndex >= 0) {
        QMetaProperty prop = meta->property(propIndex);
        int propType = prop.userType();
        QVariant qval = js_value_to_qvariant(ctx, value, propType);
        prop.write(obj, qval);
        return 1;
    }
    return -1;
}

static JSClassExoticMethods qobject_exotic_methods = {
    nullptr, // get_own_property
    nullptr, // get_own_property_names
    nullptr, // delete_property
    nullptr, // define_own_property
    nullptr, // has_property
    qobject_get_property,
    qobject_set_property
};

static JSClassDef qobject_class_def = {
    "QObjectWrapper",
    nullptr, // finalizer
    nullptr, // gc_mark
    nullptr, // call
    &qobject_exotic_methods
};

QScriptValue QScriptEngine::newQObject(QObject *object) {
    if (!object) return QScriptValue();

    JSContext *context = reinterpret_cast<JSContext*>(ctx);
    JSRuntime *runtime = reinterpret_cast<JSRuntime*>(rt);

    unsigned int qobjId = qobjectClassId();
    if (!m_qobjectClassRegistered) {
        JS_NewClass(runtime, qobjId, &qobject_class_def);
        m_qobjectClassRegistered = true;
    }

    JSValue obj = JS_NewObjectClass(context, qobjId);
    JS_SetOpaque(obj, object);
    QScriptValue ret(new QScriptValuePrivate(context, obj, true));
    JS_FreeValue(context, obj);
    return ret;
}

class QScriptSignalReceiver : public QObject {
    Q_OBJECT
public:
    JSContext *ctx;
    JSValue callback;

    QScriptSignalReceiver(JSContext *c, JSValue cb) : ctx(c), callback(JS_DupValue(c, cb)) {}
    ~QScriptSignalReceiver() {
        JS_FreeValue(ctx, callback);
    }

public slots:
    void onSignalTriggered0() {
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredInt(int val) {
        JSValue arg = JS_NewInt32(ctx, val);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredDouble(double val) {
        JSValue arg = JS_NewFloat64(ctx, val);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredBool(bool val) {
        JSValue arg = JS_NewBool(ctx, val);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredString(const QString &val) {
        JSValue arg = JS_NewString(ctx, val.toUtf8().constData());
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredVariant(const QVariant &val) {
        JSValue arg = qvariant_to_js_value(ctx, val);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
    void onSignalTriggeredVariant2(const QVariant &val1, const QVariant &val2) {
        JSValue args[2];
        args[0] = qvariant_to_js_value(ctx, val1);
        args[1] = qvariant_to_js_value(ctx, val2);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, result);
    }
};

bool qScriptConnect(QObject *sender, const char *signal, const QScriptValue &thisObject, const QScriptValue &callback) {
    if (!callback.d_ptr()) return false;
    JSContext *ctx = callback.d_ptr()->ctx;
    if (!ctx) return false;

    // Normalize signature (remove signal code if present)
    const char *signalName = signal;
    if (signalName[0] >= '0' && signalName[0] <= '3') {
        signalName++;
    }

    const QMetaObject *meta = sender->metaObject();
    int signalIndex = meta->indexOfSignal(signalName);
    if (signalIndex < 0) return false;
    QMetaMethod signalMethod = meta->method(signalIndex);

    int paramCount = signalMethod.parameterCount();
    const char *slotSignature = nullptr;

    if (paramCount == 0) {
        slotSignature = SLOT(onSignalTriggered0());
    } else if (paramCount == 1) {
        int paramType = signalMethod.parameterType(0);
        if (paramType == static_cast<int>(QMetaType::Int)) {
            slotSignature = SLOT(onSignalTriggeredInt(int));
        } else if (paramType == static_cast<int>(QMetaType::Double)) {
            slotSignature = SLOT(onSignalTriggeredDouble(double));
        } else if (paramType == static_cast<int>(QMetaType::Bool)) {
            slotSignature = SLOT(onSignalTriggeredBool(bool));
        } else if (paramType == static_cast<int>(QMetaType::QString)) {
            slotSignature = SLOT(onSignalTriggeredString(QString));
        } else {
            slotSignature = SLOT(onSignalTriggeredVariant(QVariant));
        }
    } else if (paramCount == 2) {
        slotSignature = SLOT(onSignalTriggeredVariant2(QVariant, QVariant));
    } else {
        qWarning() << "qScriptConnect: signals with more than 2 parameters are not supported";
        return false;
    }

    QScriptSignalReceiver *receiver = new QScriptSignalReceiver(ctx, callback.d_ptr()->val);
    receiver->setParent(sender);

    return QObject::connect(sender, signal, receiver, slotSignature);
}

#include "qobject_bridge.moc"
