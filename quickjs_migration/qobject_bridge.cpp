#include "qscriptengine.h"
#include "qscriptvalue_p.h"
#include <QtCore/qmetaobject.h>
#include <QtCore/qvector.h>
#include <QtCore/qdebug.h>

static JSClassID qobject_class_id = 0;

static JSValue qobject_method_caller(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *func_data) {
    void *opaque = JS_GetOpaque(this_val, qobject_class_id);
    QObject *obj = reinterpret_cast<QObject*>(opaque);
    if (!obj) {
        opaque = JS_GetOpaque(func_data[0], qobject_class_id);
        obj = reinterpret_cast<QObject*>(opaque);
    }
    if (!obj) return JS_UNDEFINED;

    int methodIndex = JS_VALUE_GET_INT(func_data[1]);
    QMetaMethod method = obj->metaObject()->method(methodIndex);

    int paramCount = method.parameterCount();
    int intVal0 = 0;
    if (paramCount > 0 && argc > 0) {
        JS_ToInt32(ctx, &intVal0, argv[0]);
    }

    int returnVal = 0;
    void *argv_qt[2];
    argv_qt[0] = &returnVal;
    argv_qt[1] = &intVal0;

    // Direct metatype-independent invocation using dynamic metacall!
    QMetaObject::metacall(obj, QMetaObject::InvokeMetaMethod, methodIndex, argv_qt);

    return JS_NewInt32(ctx, returnVal);
}

static JSValue qobject_get_property(JSContext *ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    void *opaque = JS_GetOpaque(obj_val, qobject_class_id);
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
        if (val.userType() == QMetaType::Int) {
            return JS_NewInt32(ctx, val.toInt());
        } else if (val.userType() == QMetaType::Double) {
            return JS_NewFloat64(ctx, val.toDouble());
        } else if (val.userType() >= QMetaType::User || prop.isEnumType()) {
            return JS_NewInt32(ctx, val.toInt());
        }
        return JS_NewString(ctx, val.toString().toUtf8().constData());
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
    void *opaque = JS_GetOpaque(obj_val, qobject_class_id);
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
        QVariant qval;
        if (JS_IsNumber(value)) {
            double num = 0;
            JS_ToFloat64(ctx, &num, value);
            if (prop.isEnumType()) {
                qval = QVariant::fromValue(static_cast<int>(num));
            } else {
                qval = num;
            }
        } else {
            size_t len = 0;
            const char *str = JS_ToCStringLen(ctx, &len, value);
            qval = QString::fromUtf8(str, len);
            JS_FreeCString(ctx, str);
        }
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
    qobject_set_property,
    nullptr, // get_prototype
    nullptr, // set_prototype
    nullptr, // is_extensible
    nullptr  // prevent_extensions
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

    static bool class_registered = false;
    if (!class_registered) {
        JS_NewClassID(&qobject_class_id);
        JS_NewClass(runtime, qobject_class_id, &qobject_class_def);
        class_registered = true;
    }

    JSValue obj = JS_NewObjectClass(context, qobject_class_id);
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
    void onSignalTriggered(int value) {
        JSValue arg = JS_NewInt32(ctx, value);
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, result);
    }
};

bool qScriptConnect(QObject *sender, const char *signal, const QScriptValue &thisObject, const QScriptValue &callback) {
    if (!callback.d_ptr()) return false;
    JSContext *ctx = callback.d_ptr()->ctx;
    if (!ctx) return false;

    QScriptSignalReceiver *receiver = new QScriptSignalReceiver(ctx, callback.d_ptr()->val);
    receiver->setParent(sender);

    return QObject::connect(sender, signal, receiver, SLOT(onSignalTriggered(int)));
}

#include "qobject_bridge.moc"
