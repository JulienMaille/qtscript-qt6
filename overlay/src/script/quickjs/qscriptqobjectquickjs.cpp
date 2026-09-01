/****************************************************************************
**
** QObject bridge for the QuickJS-NG QtScript backend.
**
****************************************************************************/

#include "qscriptquickjs_p.h"
#include "../api/qscriptable.h"
#include "../api/qscriptable_p.h"
#include "../api/qregexp.h"

#include <QtCore/qdebug.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qset.h>
#include <QtCore/qthread.h>

#include <limits>

QT_BEGIN_NAMESPACE

namespace {

struct QObjectPayload
{
    QPointer<QObject> object;
    QScriptEngine::ValueOwnership ownership = QScriptEngine::QtOwnership;
    QScriptEngineState *state = nullptr;
    // For wrappers based on an ordinary JS object, this is the outer object;
    // the payload itself lives on a hidden QObject-class object.
    JSValue wrapperValue = JS_UNDEFINED;
    QScriptEngine::QObjectWrapOptions options;
    // Dynamic properties present when the wrapper was populated are ordinary
    // accessor properties.  Later-added QObject dynamic properties are
    // reported by the exotic-property callback instead.
    QSet<QByteArray> directDynamicProperties;
    QSet<QByteArray> methodProperties;
};

struct PropertyClosure
{
    enum Kind {
        MetaProperty,
        DynamicProperty,
        ChildObject
    };

    QPointer<QObject> object;
    int propertyIndex = -1;
    QScriptEngineState *state = nullptr;
    Kind kind = MetaProperty;
    QByteArray name;
};

struct MethodClosure
{
    QPointer<QObject> object;
    QByteArray name;
    QByteArray signature;
    QScriptEngineState *state = nullptr;
};

struct SignalClosure
{
    QPointer<QObject> object;
    QMetaMethod signal;
    QScriptEngineState *state = nullptr;
    bool connect = true;
    bool signatureSpecific = false;
};

void deletePropertyClosure(void *opaque) { delete static_cast<PropertyClosure *>(opaque); }
void deleteMethodClosure(void *opaque) { delete static_cast<MethodClosure *>(opaque); }
void deleteSignalClosure(void *opaque) { delete static_cast<SignalClosure *>(opaque); }

static QScriptable *scriptableFromQObject(QObject *object)
{
    return object ? dynamic_cast<QScriptable *>(object) : nullptr;
}

class ScriptableInvocationScope
{
public:
    ScriptableInvocationScope(QObject *object, QScriptEngine *engine)
    {
        scriptable = scriptableFromQObject(object);
        if (scriptable) {
            previous = QScriptablePrivate::get(scriptable)->engine;
            QScriptablePrivate::get(scriptable)->engine = engine;
        }
    }

    ~ScriptableInvocationScope()
    {
        if (scriptable)
            QScriptablePrivate::get(scriptable)->engine = previous;
    }

private:
    QScriptable *scriptable = nullptr;
    QScriptEngine *previous = nullptr;
};

JSValue makeClosure(JSContext *context, JSCClosure *function, const char *name,
                    JSCClosureFinalizerFunc *finalizer, void *opaque);
JSValue methodCall(JSContext *context, JSValueConst thisValue, int argc,
                   JSValueConst *argv, int magic, void *opaque);
JSValue signalCall(JSContext *context, JSValueConst thisValue, int argc,
                   JSValueConst *argv, int magic, void *opaque);
JSValue signalOperation(JSContext *context, JSValueConst thisValue, int argc,
                        JSValueConst *argv, int magic, void *opaque);

static JSValue makeSignalObject(JSContext *context, QObject *object,
                                const QMetaMethod &signal,
                                QScriptEngineState *state, bool signatureSpecific,
                                const char *name);

QObjectPayload *payloadFor(QScriptEngineState *state, JSValueConst value)
{
    if (!state)
        return nullptr;
    QObjectPayload *payload = static_cast<QObjectPayload *>(JS_GetOpaque(value, state->qobjectClassId));
    if (payload)
        return payload;
    if (!JS_IsObject(value))
        return nullptr;
    JSValue hidden = JS_GetPropertyStr(state->context, value, "__qtscript_qobject__");
    payload = static_cast<QObjectPayload *>(JS_GetOpaque(hidden, state->qobjectClassId));
    JS_FreeValue(state->context, hidden);
    return payload;
}

static JSValue throwDeletedQObjectMember(JSContext *context, const QByteArray &name)
{
    JSValue error = JS_NewError(context);
    const QByteArray message = QByteArrayLiteral("cannot access member `")
        + name + QByteArrayLiteral("' of deleted QObject");
    JS_SetPropertyStr(context, error, "message",
                      JS_NewStringLen(context, message.constData(), size_t(message.size())));
    return JS_Throw(context, error);
}

static JSValue throwDeletedQObjectFunction(JSContext *context)
{
    JSValue error = JS_NewError(context);
    const QByteArray message = QByteArrayLiteral("cannot call function of deleted QObject");
    JS_SetPropertyStr(context, error, "message",
                      JS_NewStringLen(context, message.constData(), size_t(message.size())));
    return JS_Throw(context, error);
}

static QVariant unwrappedVariant(const QVariant &value);

static QObject *receiverObject(QScriptEngineState *state, JSValueConst thisValue,
                               QObject *fallback)
{
    if (!fallback)
        return nullptr;
    QObjectPayload *thisPayload = payloadFor(state, thisValue);
    if (!thisPayload)
        return fallback;
    if (!thisPayload->object)
        return nullptr;
    return fallback->metaObject()->cast(thisPayload->object) ? thisPayload->object.data()
                                                              : fallback;
}

void qobjectFinalizer(JSRuntime *, JSValueConst value)
{
    auto *payload = static_cast<QObjectPayload *>(JS_GetOpaque(value, JS_GetClassID(value)));
    if (!payload)
        return;
    QObject *object = payload->object;
    const bool scriptOwns = payload->ownership == QScriptEngine::ScriptOwnership
        || (payload->ownership == QScriptEngine::AutoOwnership && object && !object->parent());
    QScriptEngineState *state = payload->state;

    if (state && !state->destroying && object && !JS_IsUndefined(payload->wrapperValue)) {
        auto it = state->qobjectWrappers.find(object);
        if (it != state->qobjectWrappers.end()) {
            for (auto wrapper = it->begin(); wrapper != it->end(); ++wrapper) {
                if (JS_VALUE_GET_TAG(wrapper->value) == JS_VALUE_GET_TAG(payload->wrapperValue)
                    && JS_VALUE_GET_PTR(wrapper->value) == JS_VALUE_GET_PTR(payload->wrapperValue)) {
                    it->erase(wrapper);
                    break;
                }
            }
            if (it->isEmpty())
                state->qobjectWrappers.erase(it);
        }
    }

    // More than one script-owned wrapper can refer to the same QObject.  Do
    // not destroy the object until the last such wrapper is finalized.
    if (scriptOwns && state && !state->destroying && object) {
        const auto it = state->qobjectWrappers.constFind(object);
        if (it != state->qobjectWrappers.cend()) {
            bool hasQtOwnedWrapper = false;
            for (const QScriptQObjectWrapper &wrapper : it.value()) {
                if (wrapper.ownership == QScriptEngine::QtOwnership) {
                    hasQtOwnedWrapper = true;
                    break;
                }
                const bool otherScriptOwns =
                    wrapper.ownership == QScriptEngine::ScriptOwnership
                    || (wrapper.ownership == QScriptEngine::AutoOwnership && !object->parent());
                if (otherScriptOwns) {
                    delete payload;
                    return;
                }
            }
            if (hasQtOwnedWrapper) {
                delete payload;
                return;
            }
        }
    }
    delete payload;
    if (scriptOwns && state) {
        if (!state->deferredQObjectDeletes.contains(object))
            state->deferredQObjectDeletes.append(object);
        return;
    }
    if (scriptOwns)
        delete object;
}

JSValue propertyGetter(JSContext *context, JSValueConst thisValue, int, JSValueConst *, int,
                       void *opaque)
{
    auto *closure = static_cast<PropertyClosure *>(opaque);
    if (!closure || !closure->state || !closure->state->engine)
        return JS_UNDEFINED;
    if (!closure->object)
        return throwDeletedQObjectMember(context, closure->name);
    QObject *object = receiverObject(closure->state, thisValue, closure->object.data());
    if (!object)
        return throwDeletedQObjectMember(context, closure->name);
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(closure->state->engine);
    QScriptContext *scriptContext = QScriptContextPrivate::create();
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(scriptContext);
    contextPrivate->engine = closure->state->engine;
    contextPrivate->parent = closure->state->currentContext;
    contextPrivate->thisObject = engine->fromBorrowed(thisValue);
    contextPrivate->activationObject = closure->state->engine->newObject();
    contextPrivate->scopes.append(contextPrivate->activationObject);
    contextPrivate->scopes.append(closure->state->engine->globalObject());
    contextPrivate->functionType = QScriptContextInfo::QtPropertyFunction;
    contextPrivate->functionMetaIndex = closure->propertyIndex;
    contextPrivate->backtraceName = QString::fromUtf8(closure->name);
    QScriptContext *previousContext = closure->state->currentContext;
    closure->state->currentContext = scriptContext;
    ScriptableInvocationScope scriptableScope(object,
                                              closure->state->engine);
    JSValue result = JS_UNDEFINED;
    if (closure->kind == PropertyClosure::DynamicProperty) {
        const QVariant value = object->property(closure->name.constData());
        bool ok = false;
        result = engine->toQuickJS(engine->fromVariant(value), &ok);
        if (!ok)
            result = JS_UNDEFINED;
    } else if (closure->kind == PropertyClosure::ChildObject) {
        QObject *child = nullptr;
        for (QObject *candidate : object->children()) {
            if (candidate->objectName().toUtf8() == closure->name) {
                child = candidate;
                break;
            }
        }
        bool ok = false;
        result = engine->toQuickJS(
            closure->state->engine->newQObject(
                child, QScriptEngine::QtOwnership,
                QScriptEngine::PreferExistingWrapperObject), &ok);
        if (!ok)
            result = JS_UNDEFINED;
    } else {
        const QMetaObject *metaObject = object->metaObject();
        if (closure->propertyIndex >= 0
            && closure->propertyIndex < metaObject->propertyCount()) {
            const QMetaProperty property = metaObject->property(closure->propertyIndex);
            if (property.isReadable()) {
                const QVariant value = property.read(object);
                bool ok = false;
                result = engine->toQuickJS(engine->fromVariant(value), &ok);
                if (!ok)
                    result = JS_UNDEFINED;
            }
        }
    }
    closure->state->currentContext = previousContext;
    delete scriptContext;
    return result;
}

JSValue propertySetter(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv,
                       int, void *opaque)
{
    auto *closure = static_cast<PropertyClosure *>(opaque);
    if (!closure || !closure->state || !closure->state->engine || argc < 1)
        return JS_UNDEFINED;
    if (!closure->object)
        return throwDeletedQObjectMember(context, closure->name);
    QObject *object = receiverObject(closure->state, thisValue, closure->object.data());
    if (!object)
        return throwDeletedQObjectMember(context, closure->name);
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(closure->state->engine);
    if (closure->kind == PropertyClosure::ChildObject)
        return JS_ThrowTypeError(context, "QObject child property %s is read-only",
                                 closure->name.constData());
    if (closure->kind == PropertyClosure::DynamicProperty) {
        const QVariant value = engine->fromBorrowed(argv[0]).toVariant();
        object->setProperty(closure->name.constData(), value);
        return JS_UNDEFINED;
    }
    const QMetaObject *metaObject = object->metaObject();
    if (closure->propertyIndex < 0 || closure->propertyIndex >= metaObject->propertyCount())
        return JS_UNDEFINED;
    const QMetaProperty property = metaObject->property(closure->propertyIndex);
    QScriptContext *scriptContext = QScriptContextPrivate::create();
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(scriptContext);
    contextPrivate->engine = closure->state->engine;
    contextPrivate->parent = closure->state->currentContext;
    contextPrivate->thisObject = engine->fromBorrowed(thisValue);
    contextPrivate->activationObject = closure->state->engine->newObject();
    contextPrivate->scopes.append(contextPrivate->activationObject);
    contextPrivate->scopes.append(closure->state->engine->globalObject());
    contextPrivate->functionType = QScriptContextInfo::QtPropertyFunction;
    contextPrivate->functionMetaIndex = closure->propertyIndex;
    contextPrivate->backtraceName = QString::fromUtf8(closure->name);
    QScriptContext *previousContext = closure->state->currentContext;
    closure->state->currentContext = scriptContext;
    ScriptableInvocationScope scriptableScope(object,
                                              closure->state->engine);
    QVariant converted(property.metaType());
        const QScriptValue scriptValue = engine->fromBorrowed(argv[0]);
        const bool customEnum = property.isEnumType()
            && scriptValue.isString()
            && !engine->state->typeInfos.contains(property.userType());
        if (customEnum)
            converted = QVariant(scriptValue.toString());
        else if (property.metaType().id() == QMetaType::QVariant)
            converted = unwrappedVariant(scriptValue.toVariant());
        else if (!engine->toVariantType(scriptValue, property.metaType(), converted.data(),
                                        property.enclosingMetaObject())) {
        closure->state->currentContext = previousContext;
        delete scriptContext;
        return JS_ThrowTypeError(context, "Cannot convert value for QObject property %s", property.name());
    }
    if (!property.write(object, converted)) {
        closure->state->currentContext = previousContext;
        delete scriptContext;
        return JS_ThrowTypeError(context, "QObject property %s is not writable", property.name());
    }
    if (closure->state->engine->hasUncaughtException()) {
        bool ok = false;
        JSValue exception = engine->toQuickJS(closure->state->engine->uncaughtException(), &ok);
        closure->state->currentContext = previousContext;
        delete scriptContext;
        return ok ? JS_Throw(context, exception)
                  : JS_ThrowTypeError(context, "QObject signal handler failed");
    }
    closure->state->currentContext = previousContext;
    delete scriptContext;
    return JS_UNDEFINED;
}

static bool isQObjectMetaMember(const QObject *object, const QByteArray &name)
{
    if (!object)
        return false;
    const QMetaObject *metaObject = object->metaObject();
    if (metaObject->indexOfProperty(name.constData()) >= 0)
        return true;
    for (int index = 0; index < metaObject->methodCount(); ++index) {
        if (metaObject->method(index).name() == name)
            return true;
    }
    return false;
}

static QObject *namedChild(const QObject *object, const QByteArray &name)
{
    if (!object)
        return nullptr;
    for (QObject *child : object->children()) {
        if (child->objectName().toUtf8() == name)
            return child;
    }
    return nullptr;
}

static bool dynamicPropertyExists(const QObject *object, const QByteArray &name)
{
    if (!object || isQObjectMetaMember(object, name))
        return false;
    return object->dynamicPropertyNames().contains(name)
        && object->property(name.constData()).isValid();
}

static QVariant unwrappedVariant(const QVariant &value)
{
    QVariant result = value;
    for (int depth = 0; depth < 8 && result.isValid()
         && result.metaType().id() == QMetaType::QVariant; ++depth) {
        result = result.value<QVariant>();
    }
    return result;
}

static bool objectDeclaresEnum(const QObject *object, const QMetaType &type)
{
    if (!object || !type.isValid() || !type.name())
        return false;
    QByteArray enumName(type.name());
    if (enumName.startsWith("QFlags<") && enumName.endsWith('>'))
        enumName = enumName.mid(7, enumName.size() - 8);
    const int separator = enumName.lastIndexOf("::");
    if (separator >= 0)
        enumName.remove(0, separator + 2);
    if (object->metaObject()->indexOfEnumerator(enumName.constData()) >= 0)
        return true;
    if (enumName.endsWith("Flag")) {
        enumName.chop(4);
        return object->metaObject()->indexOfEnumerator(enumName.constData()) >= 0;
    }
    return false;
}

static bool hasMethodAccess(const QMetaMethod &method, int index,
                            const QScriptEngine::QObjectWrapOptions &options)
{
    static const int deleteLaterIndex = QObject::staticMetaObject.indexOfMethod("deleteLater()");
    return method.access() != QMetaMethod::Private
        && ((index != deleteLaterIndex)
            || !options.testFlag(QScriptEngine::ExcludeDeleteLater))
        && (!options.testFlag(QScriptEngine::ExcludeSlots)
            || method.methodType() != QMetaMethod::Slot);
}

static int qobjectGetOwnProperty(JSContext *context, JSPropertyDescriptor *descriptor,
                                 JSValueConst value, JSAtom atom)
{
    QScriptEngineState *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QObjectPayload *payload = payloadFor(state, value);
    if (!payload)
        return 0;
    JSValue atomString = JS_AtomToString(context, atom);
    if (JS_IsException(atomString) || JS_IsSymbol(atomString)) {
        JS_FreeValue(context, atomString);
        return 0;
    }
    const char *name = JS_ToCString(context, atomString);
    if (!name) {
        JS_FreeValue(context, atomString);
        return 0;
    }
    const QByteArray propertyName(name);
    JS_FreeCString(context, name);
    JS_FreeValue(context, atomString);
    if (!payload->object) {
        if (JS_IsUndefined(payload->wrapperValue))
            return 0;
        if (!descriptor)
            return 1;
        auto *getterClosure = new PropertyClosure{nullptr, -1, state,
                                                  PropertyClosure::MetaProperty, propertyName};
        auto *setterClosure = new PropertyClosure{nullptr, -1, state,
                                                  PropertyClosure::MetaProperty, propertyName};
        descriptor->value = JS_UNDEFINED;
        descriptor->getter = makeClosure(context, propertyGetter, propertyName.constData(),
                                         deletePropertyClosure, getterClosure);
        descriptor->setter = makeClosure(context, propertySetter, propertyName.constData(),
                                         deletePropertyClosure, setterClosure);
        descriptor->flags = JS_PROP_GETSET | JS_PROP_HAS_GET | JS_PROP_HAS_SET
            | JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        return 1;
    }
    PropertyClosure::Kind kind;
    int metaPropertyIndex = -1;
    bool metaWritable = false;
    if (dynamicPropertyExists(payload->object, propertyName)) {
        kind = PropertyClosure::DynamicProperty;
    } else if (!payload->options.testFlag(QScriptEngine::ExcludeChildObjects)
               && namedChild(payload->object, propertyName)) {
        kind = PropertyClosure::ChildObject;
    } else {
        const QMetaObject *metaObject = payload->object->metaObject();
        const int propertyIndex = metaObject->indexOfProperty(propertyName.constData());
        if (propertyIndex < 0)
            return 0;
        const QMetaProperty property = metaObject->property(propertyIndex);
        const bool excludeSuper =
            payload->options.testFlag(QScriptEngine::ExcludeSuperClassProperties)
            || payload->options.testFlag(QScriptEngine::ExcludeSuperClassContents);
        if (!property.isScriptable()
            || (excludeSuper && propertyIndex < metaObject->propertyOffset()))
            return 0;
        kind = PropertyClosure::MetaProperty;
        metaPropertyIndex = propertyIndex;
        metaWritable = property.isWritable();
    }
    if (!descriptor)
        return 1;

    auto *getterClosure = new PropertyClosure{payload->object, metaPropertyIndex, state,
                                              kind, propertyName};
    JSValue getter = makeClosure(context, propertyGetter, propertyName.constData(),
                                 deletePropertyClosure, getterClosure);
    JSValue setter = JS_UNDEFINED;
    // Dynamic properties and writable meta-properties expose a real setter so
    // that assignments made through a prototype-inheriting receiver (e.g.
    // `o.oof = x` with `o.__proto__ = scriptable`) land on the QObject
    // property instead of silently creating an own shadowing property.  A
    // read-only meta-property is exposed as a getter-only accessor so writes
    // through inheritance are rejected rather than shadowed.
    if (kind == PropertyClosure::DynamicProperty
        || (kind == PropertyClosure::MetaProperty && metaWritable)) {
        auto *setterClosure = new PropertyClosure{payload->object, metaPropertyIndex, state,
                                                  kind, propertyName};
        setter = makeClosure(context, propertySetter, propertyName.constData(),
                             deletePropertyClosure, setterClosure);
    }
    descriptor->value = JS_UNDEFINED;
    descriptor->getter = getter;
    descriptor->setter = setter;
    descriptor->flags = JS_PROP_GETSET | JS_PROP_HAS_GET | JS_PROP_HAS_SET
        | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;
    if (kind == PropertyClosure::DynamicProperty)
        descriptor->flags |= JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    else if (kind == PropertyClosure::MetaProperty)
        // populateQObject installs meta-properties as enumerable getset
        // shape entries (JS_PROP_ENUMERABLE); mirror that flag so the exotic
        // hook never disagrees with the shape when it fires for a property
        // the shape also carries.
        descriptor->flags |= JS_PROP_ENUMERABLE;
    return 1;
}

static int qobjectGetOwnPropertyNames(JSContext *context, JSPropertyEnum **properties,
                                      uint32_t *count, JSValueConst value)
{
    QScriptEngineState *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QObjectPayload *payload = payloadFor(state, value);
    if (!payload || !payload->object) {
        *properties = nullptr;
        *count = 0;
        return 0;
    }

    QList<QByteArray> names;
    QSet<QByteArray> seen;
    for (const QByteArray &name : payload->object->dynamicPropertyNames()) {
        if (dynamicPropertyExists(payload->object, name)
            && !payload->directDynamicProperties.contains(name)
            && !seen.contains(name)) {
            seen.insert(name);
            names.append(name);
        }
    }
    if (!payload->options.testFlag(QScriptEngine::ExcludeChildObjects)) {
    for (QObject *child : payload->object->children()) {
        const QByteArray name = child->objectName().toUtf8();
        if (!name.isEmpty() && !isQObjectMetaMember(payload->object, name)
            && !seen.contains(name)) {
            seen.insert(name);
            names.append(name);
        }
    }
    }
    if (names.isEmpty()) {
        *properties = nullptr;
        *count = 0;
        return 0;
    }
    auto *result = static_cast<JSPropertyEnum *>(
        js_malloc(context, sizeof(JSPropertyEnum) * size_t(names.size())));
    if (!result)
        return -1;
    for (int index = 0; index < names.size(); ++index) {
        result[index].atom = JS_NewAtomLen(context, names.at(index).constData(),
                                           size_t(names.at(index).size()));
        if (result[index].atom == JS_ATOM_NULL) {
            JS_FreePropertyEnum(context, result, uint32_t(index));
            return -1;
        }
        result[index].is_enumerable = true;
    }
    *properties = result;
    *count = uint32_t(names.size());
    return 0;
}

static int qobjectDeleteProperty(JSContext *context, JSValueConst value, JSAtom atom)
{
    QScriptEngineState *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QObjectPayload *payload = payloadFor(state, value);
    if (payload && payload->object) {
        const char *name = JS_AtomToCString(context, atom);
        if (!name)
            return -1;
        const QByteArray propertyName(name);
        JS_FreeCString(context, name);
        if (dynamicPropertyExists(payload->object, propertyName)) {
            payload->object->setProperty(propertyName.constData(), QVariant());
            return true;
        }
        if (!payload->options.testFlag(QScriptEngine::ExcludeChildObjects)
            && namedChild(payload->object, propertyName))
            return false;
    }
    return JS_DeleteProperty(context, value, atom, JS_PROP_NO_EXOTIC);
}

static JSValue qobjectGetProperty(JSContext *context, JSValueConst value, JSAtom atom,
                                  JSValueConst receiver)
{
    QScriptEngineState *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QObjectPayload *payload = payloadFor(state, value);
    if (!payload)
        return JS_UNDEFINED;

    JSValue atomString = JS_AtomToString(context, atom);
    if (JS_IsException(atomString) || JS_IsSymbol(atomString)) {
        JS_FreeValue(context, atomString);
        return JS_UNDEFINED;
    }
    const char *name = JS_ToCString(context, atomString);
    if (!name) {
        JS_FreeValue(context, atomString);
        return JS_UNDEFINED;
    }
    const QByteArray propertyName(name);
    JS_FreeCString(context, name);
    JS_FreeValue(context, atomString);
    if (!payload->object) {
        if (!JS_IsUndefined(payload->wrapperValue))
            return throwDeletedQObjectMember(context, propertyName);
        JSValue prototype = JS_GetPrototype(context, value);
        if (JS_IsObject(prototype)) {
            JSValue result = JS_GetProperty(context, prototype, atom);
            JS_FreeValue(context, prototype);
            return result;
        }
        JS_FreeValue(context, prototype);
        return JS_UNDEFINED;
    }

    PropertyClosure::Kind kind;
    if (dynamicPropertyExists(payload->object, propertyName)) {
        kind = PropertyClosure::DynamicProperty;
    } else if (!payload->options.testFlag(QScriptEngine::ExcludeChildObjects)
               && namedChild(payload->object, propertyName)) {
        kind = PropertyClosure::ChildObject;
    } else {
        const QMetaObject *metaObject = payload->object->metaObject();
        int methodIndex = -1;
        if (propertyName.contains('(')) {
            const QByteArray normalized = QMetaObject::normalizedSignature(
                propertyName.constData());
            methodIndex = metaObject->indexOfMethod(normalized.constData());
            if (methodIndex >= 0) {
                const QMetaMethod method = metaObject->method(methodIndex);
                if (!hasMethodAccess(method, methodIndex, payload->options)
                    || ((payload->options.testFlag(QScriptEngine::ExcludeSuperClassMethods)
                         || payload->options.testFlag(QScriptEngine::ExcludeSuperClassContents))
                        && methodIndex < metaObject->methodOffset()))
                    methodIndex = -1;
            }
        } else {
            const int methodStart =
                (payload->options.testFlag(QScriptEngine::ExcludeSuperClassMethods)
                 || payload->options.testFlag(QScriptEngine::ExcludeSuperClassContents))
                    ? metaObject->methodOffset() : 0;
            for (int index = metaObject->methodCount() - 1; index >= methodStart; --index) {
                const QMetaMethod method = metaObject->method(index);
                if (method.name() == propertyName
                    && hasMethodAccess(method, index, payload->options)
                    && method.methodType() != QMetaMethod::Signal) {
                    methodIndex = index;
                    break;
                }
            }
        }

        if (methodIndex >= 0) {
            const QMetaMethod method = metaObject->method(methodIndex);
            if (method.methodType() == QMetaMethod::Signal) {
                // QMetaObject::normalizedSignature() accepts legacy spellings
                // such as valueChanged(const QString&), while populateQObject
                // installs the canonical valueChanged(QString) property.  A
                // lookup through the legacy spelling must still return a real
                // signal object; treating it as a method leaves the inherited
                // Function.prototype.connect helper pointing at a non-signal
                // closure and can hang the QtScript evaluation path.
                return makeSignalObject(context, payload->object, method, state,
                                        true, propertyName.constData());
            }
            auto *closure = new MethodClosure{payload->object, method.name(),
                                              method.methodSignature(), state};
            return makeClosure(context, methodCall, propertyName.constData(),
                               deleteMethodClosure, closure);
        }

        const int propertyIndex = metaObject->indexOfProperty(propertyName.constData());
        if (propertyIndex >= 0) {
            const QMetaProperty property = metaObject->property(propertyIndex);
            if (property.isScriptable()
                && (!(payload->options.testFlag(QScriptEngine::ExcludeSuperClassProperties)
                      || payload->options.testFlag(QScriptEngine::ExcludeSuperClassContents))
                    || propertyIndex >= metaObject->propertyOffset())) {
                PropertyClosure closure{payload->object, propertyIndex, state,
                                        PropertyClosure::MetaProperty, propertyName};
                return propertyGetter(context, receiver, 0, nullptr, 0, &closure);
            }
        }

        JSValue prototype = JS_GetPrototype(context, value);
        if (JS_IsObject(prototype)) {
            JSValue result = JS_GetProperty(context, prototype, atom);
            JS_FreeValue(context, prototype);
            return result;
        }
        JS_FreeValue(context, prototype);
        return JS_UNDEFINED;
    }

    PropertyClosure closure{payload->object, -1, state, kind, propertyName};
    return propertyGetter(context, receiver, 0, nullptr, 0, &closure);
}

static int qobjectDefineOwnProperty(JSContext *context, JSValueConst value, JSAtom atom,
                                    JSValueConst propertyValue, JSValueConst getter,
                                    JSValueConst setter, int flags)
{
    QScriptEngineState *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QObjectPayload *payload = payloadFor(state, value);
    if (!payload)
        return 0;
    const char *name = JS_AtomToCString(context, atom);
    if (!name)
        return -1;
    const QByteArray propertyName(name);
    JS_FreeCString(context, name);
    if (!payload->object) {
        throwDeletedQObjectMember(context, propertyName);
        return -1;
    }

    const bool dynamic = dynamicPropertyExists(payload->object, propertyName)
        || (payload->options.testFlag(QScriptEngine::AutoCreateDynamicProperties)
            && !isQObjectMetaMember(payload->object, propertyName));
    if (dynamic && !(flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))) {
        QScriptEnginePrivate *engine = QScriptEnginePrivate::get(state->engine);
        payload->object->setProperty(propertyName.constData(),
                                     engine->fromBorrowed(propertyValue).toVariant());
        return true;
    }

    if (!(flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))) {
        const QMetaObject *metaObject = payload->object->metaObject();
        const int propertyIndex = metaObject->indexOfProperty(propertyName.constData());
        if (propertyIndex >= 0) {
            const QMetaProperty property = metaObject->property(propertyIndex);
            if (property.isScriptable() && property.isWritable()) {
                QScriptEnginePrivate *engine = QScriptEnginePrivate::get(state->engine);
                QVariant converted(property.metaType());
                if (property.metaType().id() == QMetaType::QVariant) {
                    converted = unwrappedVariant(engine->fromBorrowed(propertyValue).toVariant());
                } else if (!engine->toVariantType(engine->fromBorrowed(propertyValue),
                                                  property.metaType(), converted.data(),
                                                  property.enclosingMetaObject())) {
                    JS_ThrowTypeError(context,
                                      "Cannot convert value for QObject property %s",
                                      property.name());
                    return -1;
                }
                if (!property.write(payload->object, converted)) {
                    JS_ThrowTypeError(context,
                                      "QObject property %s is not writable",
                                      property.name());
                    return -1;
                }
                return true;
            }
        }
    }

    return JS_DefineProperty(context, value, atom, propertyValue, getter, setter,
                             flags | JS_PROP_NO_EXOTIC);
}

static JSClassExoticMethods qobjectExoticMethods{
    qobjectGetOwnProperty,
    qobjectGetOwnPropertyNames,
    qobjectDeleteProperty,
    qobjectDefineOwnProperty,
    nullptr,
    qobjectGetProperty,
    nullptr
};

bool invokeMethod(const QMetaMethod &method, QObject *object, QScriptEnginePrivate *engine,
                  int argc, JSValueConst *argv, QScriptValue *result)
{
    if (argc < method.parameterCount() || method.parameterCount() > 10)
        return false;

    QVector<QVariant> converted;
    converted.reserve(method.parameterCount());
    const QList<QByteArray> parameterTypeNames = method.parameterTypes();
    QGenericArgument arguments[10];
    for (int index = 0; index < method.parameterCount(); ++index) {
        const QMetaType type = method.parameterMetaType(index);
        if (!type.isValid())
            return false;
        converted.append(type.id() == QMetaType::QVariant ? QVariant() : QVariant(type));
        if (type.id() == QMetaType::QVariant)
            converted.last() = unwrappedVariant(engine->fromBorrowed(argv[index]).toVariant());
        else if (!engine->toVariantType(engine->fromBorrowed(argv[index]), type,
                                        converted.last().data(), object->metaObject()))
            return false;
        arguments[index] = QGenericArgument(parameterTypeNames.at(index).constData(),
                                            converted.last().constData());
    }

    const QMetaType returnType = method.returnMetaType();
    QVariant returnStorage;
    QGenericReturnArgument returnArgument;
    if (returnType.isValid() && returnType.id() != QMetaType::Void) {
        returnStorage = QVariant(returnType);
        returnArgument = QGenericReturnArgument(method.typeName(), returnStorage.data());
    }

    QScriptable *scriptable = dynamic_cast<QScriptable *>(object);
    if (scriptable)
        QScriptablePrivate::get(scriptable)->engine = engine->state->engine;
    const bool invoked = method.invoke(object, Qt::DirectConnection, returnArgument,
        arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
        arguments[5], arguments[6], arguments[7], arguments[8], arguments[9]);
    if (scriptable)
        QScriptablePrivate::get(scriptable)->engine = nullptr;
    if (!invoked)
        return false;
    *result = returnStorage.isValid() ? engine->fromVariant(returnStorage)
                                      : engine->state->engine->undefinedValue();
    return true;
}

static int argumentConversionScore(const QScriptValue &value, const QMetaType &type,
                                   QScriptEnginePrivate *engine, QObject *object)
{
    if (!type.isValid() || !engine)
        return std::numeric_limits<int>::max();
    const int typeId = type.id();
    const QByteArray typeName = type.name() ? QByteArray(type.name()) : QByteArray();
    if (type.flags().testFlag(QMetaType::IsEnumeration)
        && !engine->state->typeInfos.contains(typeId)
        && !objectDeclaresEnum(object, type))
        return std::numeric_limits<int>::max();
    if (typeName == "QScriptValue" || typeId == qMetaTypeId<QScriptValue>())
        return value.engine() == engine->state->engine ? 0 : 1000;
    if (typeId == QMetaType::Bool)
        return value.isBoolean() ? 0 : (value.isNumber() ? 4 : 20);
    if (typeId == QMetaType::Double)
        return value.isNumber() ? 0 : 20;
    if (typeId == QMetaType::Float)
        return value.isNumber() ? 1 : 20;
    if (typeId == QMetaType::Int || typeId == QMetaType::UInt
        || typeId == QMetaType::Char || typeId == QMetaType::SChar
        || typeId == QMetaType::UChar || typeId == QMetaType::Short
        || typeId == QMetaType::UShort || typeId == QMetaType::Long
        || typeId == QMetaType::ULong || typeId == QMetaType::LongLong
        || typeId == QMetaType::ULongLong)
        return value.isNumber() ? 2 : 20;
    if (typeId == QMetaType::QString)
        return value.isString() ? 0 : 15;
    if (typeId == QMetaType::QVariant)
        return value.isVariant() ? 0 : 30;
    if (typeId == QMetaType::VoidStar)
        return value.isNull() ? 0 : std::numeric_limits<int>::max();
    if (type.flags().testFlag(QMetaType::PointerToQObject)) {
        if (value.isNull() || value.isUndefined())
            return 0;
        QObject *object = value.toQObject();
        if (!object)
            return std::numeric_limits<int>::max();
        return type.metaObject() && type.metaObject()->cast(object) ? 0 : 1000;
    }
    if (typeId == QMetaType::QVariantList || typeName == "QStringList")
        return value.isArray() ? 0 : 20;
    if (typeName == "QObjectList" || typeName == "QList<QObject*>"
        || typeName == "QList<QObject *>")
        return value.isArray() ? 0 : std::numeric_limits<int>::max();
    if (typeName == "QList<int>")
        return value.isArray() ? 0 : std::numeric_limits<int>::max();
    if (typeId == QMetaType::QVariantMap)
        return value.isObject() && !value.isArray() ? 0 : 20;
    if (typeId == QMetaType::QDateTime)
        return value.isDate() ? 0 : 20;
    if (typeId == QMetaType::QDate || typeId == QMetaType::QTime)
        return value.isDate() ? 1 : 20;
    if (typeId == qMetaTypeId<QRegExp>())
        return value.isRegExp() ? 0 : 20;

    if (!type.flags().testFlag(QMetaType::IsPointer) && value.isVariant()) {
        const QVariant variant = value.toVariant();
        const QMetaType variantType = variant.metaType();
        if (variant.isValid() && variantType.flags().testFlag(QMetaType::IsPointer)
            && variantType.name() && type.name()) {
            QByteArray pointeeName(variantType.name());
            if (pointeeName.endsWith('*')) {
                pointeeName.chop(1);
                pointeeName = pointeeName.trimmed();
                if (pointeeName.startsWith("const "))
                    pointeeName.remove(0, 6);
                if (pointeeName == type.name())
                    return 1;
            }
        }
    }

    if (value.isVariant()) {
        const QVariant variant = value.toVariant();
        if (variant.isValid() && variant.metaType() == type)
            return 0;
        if (variant.isValid() && variant.canConvert(type))
            return 5;
    }
    if (engine->state->typeInfos.contains(typeId))
        return 5;
    const QVariant variant = value.toVariant();
    if (variant.isValid() && variant.canConvert(type))
        return 50;
    return std::numeric_limits<int>::max();
}

JSValue methodCall(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv,
                   int, void *opaque)
{
    auto *closure = static_cast<MethodClosure *>(opaque);
    if (!closure || !closure->state || !closure->state->engine)
        return JS_ThrowTypeError(context, "Cannot call a method on a deleted QObject");
    if (!closure->object)
        return throwDeletedQObjectFunction(context);
    QObject *object = receiverObject(closure->state, thisValue, closure->object.data());
    if (!object)
        return throwDeletedQObjectFunction(context);

    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(closure->state->engine);
    QScriptContext *scriptContext = QScriptContextPrivate::create();
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(scriptContext);
    contextPrivate->engine = closure->state->engine;
    contextPrivate->parent = closure->state->currentContext;
    contextPrivate->thisObject = engine->fromBorrowed(thisValue);
    contextPrivate->activationObject = closure->state->engine->newObject();
    contextPrivate->scopes.append(contextPrivate->activationObject);
    contextPrivate->scopes.append(closure->state->engine->globalObject());
    for (int index = 0; index < argc; ++index)
        contextPrivate->arguments.append(engine->fromBorrowed(argv[index]));
    QScriptContext *previousContext = closure->state->currentContext;
    closure->state->currentContext = scriptContext;
    if (closure->state->agent)
        closure->state->agent->contextPush();
    ScriptableInvocationScope scriptableScope(object,
                                              closure->state->engine);

    const QMetaObject *metaObject = object->metaObject();
    if (closure->signature.isEmpty()
        && (closure->name == QByteArrayLiteral("findChild")
            || closure->name == QByteArrayLiteral("findChildren"))) {
        QScriptValue result;
        if (closure->name == QByteArrayLiteral("findChild")) {
            const QString name = argc > 0 ? engine->fromBorrowed(argv[0]).toString()
                                          : QString();
            QObject *child = object->findChild<QObject *>(name);
            result = closure->state->engine->newQObject(
                child, QScriptEngine::QtOwnership,
                QScriptEngine::PreferExistingWrapperObject);
        } else {
            QObjectList children;
            if (argc > 0 && engine->fromBorrowed(argv[0]).isRegExp()) {
                const QRegExp regexp = engine->fromBorrowed(argv[0]).toRegExp();
                for (QObject *child : object->children()) {
                    if (regexp.indexIn(child->objectName()) >= 0)
                        children.append(child);
                }
            } else if (argc > 0) {
                children = object->findChildren<QObject *>(
                    engine->fromBorrowed(argv[0]).toString());
            } else {
                children = object->findChildren<QObject *>(QString());
            }
            result = closure->state->engine->newArray(uint(children.size()));
            for (int index = 0; index < children.size(); ++index)
                result.setProperty(uint(index), closure->state->engine->newQObject(
                    children.at(index), QScriptEngine::QtOwnership,
                    QScriptEngine::PreferExistingWrapperObject));
        }
        if (closure->state->agent)
            closure->state->agent->contextPop();
        closure->state->currentContext = previousContext;
        delete scriptContext;
        bool ok = false;
        JSValue value = engine->toQuickJS(result, &ok);
        return ok ? value : JS_UNDEFINED;
    }

    QStringList candidates;
    QMetaMethod selected;
    int selectedScore = std::numeric_limits<int>::max();
    bool ambiguous = false;
    for (int index = metaObject->methodCount() - 1; index >= 0; --index) {
        const QMetaMethod method = metaObject->method(index);
        if (method.name() != closure->name)
            continue;
        if (!closure->signature.isEmpty()
            && method.methodSignature() != closure->signature)
            continue;
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        candidates.append(QString::fromLatin1(method.methodSignature()));
        if (argc < method.parameterCount() || method.parameterCount() > 10)
            continue;
        int score = 0;
        for (int argument = 0; argument < method.parameterCount(); ++argument) {
            const int argumentScore = argumentConversionScore(
                engine->fromBorrowed(argv[argument]), method.parameterMetaType(argument), engine,
                object);
            if (argumentScore == std::numeric_limits<int>::max()) {
                score = std::numeric_limits<int>::max();
                break;
            }
            score += argumentScore;
        }
        if (score != std::numeric_limits<int>::max())
            score += (argc - method.parameterCount()) * 10;
        if (score < selectedScore) {
            selected = method;
            selectedScore = score;
            ambiguous = false;
        } else if (score != std::numeric_limits<int>::max() && score == selectedScore) {
            if (selected.methodSignature() != method.methodSignature())
                ambiguous = true;
        }
    }

    if (selected.isValid() && !ambiguous) {
        contextPrivate->functionType = QScriptContextInfo::QtFunction;
        contextPrivate->functionMetaIndex = selected.methodIndex();
        contextPrivate->parameterNames.clear();
        for (const QByteArray &name : selected.parameterNames())
            contextPrivate->parameterNames.append(QString::fromLatin1(name));
        contextPrivate->backtraceName = QString::fromLatin1(selected.name());
        QScriptValue result;
        if (!invokeMethod(selected, object, engine, argc, argv, &result)) {
            if (contextPrivate->state == QScriptContext::ExceptionState) {
                const QScriptValue thrownValue = contextPrivate->thrownValue;
                if (closure->state->agent)
                    closure->state->agent->contextPop();
                closure->state->currentContext = previousContext;
                delete scriptContext;
                bool ok = false;
                JSValue value = engine->toQuickJS(thrownValue, &ok);
                return ok ? JS_Throw(context, value)
                          : JS_ThrowTypeError(context, "QObject argument conversion failed");
            }
        } else {
            const bool threw = contextPrivate->state == QScriptContext::ExceptionState;
            const QScriptValue thrownValue = contextPrivate->thrownValue;
            if (closure->state->agent)
                closure->state->agent->contextPop();
            closure->state->currentContext = previousContext;
            delete scriptContext;
            bool ok = false;
            JSValue value = engine->toQuickJS(threw ? thrownValue : result, &ok);
            if (!ok)
                value = JS_UNDEFINED;
            return threw ? JS_Throw(context, value) : value;
        }
    }

    if (closure->signature.isEmpty() && ambiguous) {
        std::sort(candidates.begin(), candidates.end());
        if (closure->state->agent)
            closure->state->agent->contextPop();
        closure->state->currentContext = previousContext;
        delete scriptContext;
        QString message = QStringLiteral("ambiguous call of overloaded function %1(); candidates were")
            .arg(QString::fromLatin1(closure->name));
        for (const QString &candidate : candidates)
            message += QLatin1Char('\n') + QStringLiteral("    ") + candidate;
        return JS_ThrowTypeError(context, "%s", message.toUtf8().constData());
    }

    /* A conversion failure may have raised a QScript exception. */
    if (contextPrivate->state == QScriptContext::ExceptionState) {
        const QScriptValue thrownValue = contextPrivate->thrownValue;
        if (closure->state->agent)
            closure->state->agent->contextPop();
        closure->state->currentContext = previousContext;
        delete scriptContext;
        bool ok = false;
        JSValue value = engine->toQuickJS(thrownValue, &ok);
        return ok ? JS_Throw(context, value)
                  : JS_ThrowTypeError(context, "QObject argument conversion failed");
    }

    if (closure->state->agent)
        closure->state->agent->contextPop();
    closure->state->currentContext = previousContext;
    delete scriptContext;

    if (closure->signature.isEmpty()) {
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.name() != closure->name || argc < method.parameterCount())
                continue;
            for (int argument = 0; argument < method.parameterCount(); ++argument) {
                const QMetaType type = method.parameterMetaType(argument);
                if (type.flags().testFlag(QMetaType::IsEnumeration)
                    && !engine->state->typeInfos.contains(type.id())
                    && !objectDeclaresEnum(object, type)) {
                    const char *name = type.name() ? type.name() : "unknown";
                    return JS_ThrowTypeError(context,
                                             "cannot call %s(): argument %d has unknown type `%s' (register the type with qScriptRegisterMetaType())",
                                             closure->name.constData(), argument + 1, name);
                }
            }
        }
    }

    if (closure->signature.isEmpty() && !candidates.isEmpty()) {
        bool tooFewArguments = true;
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.name() == closure->name && method.parameterCount() <= argc) {
                tooFewArguments = false;
                break;
            }
        }
        if (tooFewArguments) {
            QString message = QStringLiteral("too few arguments in call to %1(); candidates are")
                .arg(QString::fromLatin1(closure->name));
            for (const QString &candidate : candidates)
                message += QLatin1Char('\n') + QStringLiteral("    ") + candidate;
            return JS_ThrowSyntaxError(context, "%s", message.toUtf8().constData());
        }
    }

    QString incompatible = QStringLiteral(
        "incompatible type of argument(s) in call to %1(); candidates were")
        .arg(QString::fromLatin1(closure->name));
    for (const QString &candidate : candidates)
        incompatible += QLatin1Char('\n') + QStringLiteral("    ") + candidate;
    const QByteArray message = incompatible.toUtf8();
    return JS_ThrowTypeError(context, "%s", message.constData());
}

class ScriptSignalSlotObject final : public QtPrivate::QSlotObjectBase
{
public:
    ScriptSignalSlotObject(const QScriptValue &callback, const QMetaMethod &signal,
                           QScriptEngineState *state,
                           const QScriptValue &receiver = QScriptValue())
        : QSlotObjectBase(&impl), callback(callback), receiver(receiver), signal(signal), state(state)
    {
    }

private:
    static void callOnEngine(QPointer<QScriptEngine> engineObject,
                             QScriptEngineState *state,
                             QPointer<QObject> sender,
                             QScriptValue callback,
                             const QScriptValue &receiver,
                             const QMetaMethod &signal,
                             const QList<QVariant> &values)
    {
        if (!engineObject || !state || state->destroying || !state->context
            || !callback.isFunction()) {
            return;
        }
        if (receiver.isObject() && receiver.isQObject() && !receiver.toQObject())
            return;
        const qint64 callbackId = callback.objectId();
        const qint64 receiverId = receiver.isObject() ? receiver.objectId() : -1;
        bool connected = false;
        for (const QScriptSignalConnection &connection : state->signalConnections) {
            if (connection.sender == sender
                && connection.signalIndex == signal.methodIndex()
                && connection.callbackId == callbackId
                && connection.receiverId == receiverId) {
                connected = true;
                break;
            }
        }
        if (!connected)
            return;
        QScriptEnginePrivate *engine = QScriptEnginePrivate::get(engineObject.data());
        QScriptValueList arguments;
        arguments.reserve(signal.parameterCount());
        for (int index = 0; index < signal.parameterCount(); ++index) {
            const QMetaType type = signal.parameterMetaType(index);
            const QVariant &value = values.at(index);
            if (type.id() == QMetaType::QVariant) {
                arguments.append(value.metaType().id() == QMetaType::QVariant
                                     ? engine->fromVariantAsVariant(value)
                                     : engine->fromVariant(value));
                continue;
            }
            arguments.append(engine->fromVariant(value));
        }
        JS_UpdateStackTop(state->runtime);
        const QScriptValue result = callback.call(receiver, arguments);
        if (result.isError() || engineObject->hasUncaughtException()) {
            QScriptValue exception = result.isError()
                ? result : engineObject->uncaughtException();
            bool ok = false;
            JSValue quickException = engine->toQuickJS(exception, &ok);
            if (ok)
                JS_Throw(state->context, quickException);
            emit engineObject->signalHandlerException(exception);
        }
    }

    static void impl(int operation, QtPrivate::QSlotObjectBase *base, QObject *sender,
                     void **arguments, bool *result)
    {
        auto *self = static_cast<ScriptSignalSlotObject *>(base);
        switch (operation) {
        case Destroy:
            delete self;
            break;
        case Call:
            self->call(sender, arguments);
            break;
        case Compare:
            if (result)
                *result = false;
            break;
        default:
            break;
        }
    }

    void call(QObject *sender, void **rawArguments)
    {
        if (!state || !state->engine || !callback.isFunction())
            return;
        QList<QVariant> values;
        values.reserve(signal.parameterCount());
        for (int index = 0; index < signal.parameterCount(); ++index) {
            const QMetaType type = signal.parameterMetaType(index);
            if (type.id() == QMetaType::QVariant) {
                values.append(*static_cast<const QVariant *>(rawArguments[index + 1]));
                continue;
            }
            values.append(QVariant(type, rawArguments[index + 1]));
        }

        QPointer<QScriptEngine> engineObject = state->engine;
        if (!engineObject)
            return;
        if (QThread::currentThread() != engineObject->thread()) {
            const QScriptValue queuedCallback = callback;
            const QScriptValue queuedReceiver = receiver;
            const QMetaMethod queuedSignal = signal;
             QScriptEngineState *queuedState = state;
            QMetaObject::invokeMethod(engineObject.data(),
                                      [engineObject, queuedState, sender, queuedCallback,
                                       queuedReceiver, queuedSignal, values] {
                                           callOnEngine(engineObject, queuedState,
                                                        sender,
                                                        queuedCallback, queuedReceiver,
                                                        queuedSignal, values);
                                      },
                                      Qt::QueuedConnection);
            return;
        }
        callOnEngine(engineObject, state, sender, callback, receiver, signal, values);
    }

    QScriptValue callback;
    QScriptValue receiver;
    QMetaMethod signal;
    QScriptEngineState *state = nullptr;
};

JSValue signalCall(JSContext *context, JSValueConst, int argc, JSValueConst *argv,
                   int, void *opaque)
{
    auto *closure = static_cast<SignalClosure *>(opaque);
    if (!closure || !closure->object || !closure->state || !closure->state->engine)
        return JS_ThrowTypeError(context, "Cannot emit a signal on a deleted QObject");
    if (!closure->signatureSpecific) {
        int overloadCount = 0;
        const QMetaObject *metaObject = closure->object->metaObject();
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.methodType() == QMetaMethod::Signal
                && method.name() == closure->signal.name())
                ++overloadCount;
        }
        if (overloadCount != 1)
            return JS_ThrowTypeError(context, "Cannot emit an overloaded signal without a signature");
    }
    if (argc > closure->signal.parameterCount())
        return JS_ThrowTypeError(context, "Too many arguments for signal %s",
                                 closure->signal.name().constData());

    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(closure->state->engine);
    QVector<QVariant> converted;
    converted.reserve(closure->signal.parameterCount());
    const QList<QByteArray> parameterTypeNames = closure->signal.parameterTypes();
    QGenericArgument arguments[10];
    for (int index = 0; index < closure->signal.parameterCount(); ++index) {
        const QMetaType type = closure->signal.parameterMetaType(index);
        converted.append(type.id() == QMetaType::QVariant ? QVariant() : QVariant(type));
        if (index < argc) {
            if (type.id() == QMetaType::QVariant) {
                converted.last() = unwrappedVariant(
                    engine->fromBorrowed(argv[index]).toVariant());
            } else if (!engine->toVariantType(engine->fromBorrowed(argv[index]), type,
                                              converted.last().data(),
                                              closure->object->metaObject())) {
                return JS_ThrowTypeError(context, "Cannot convert signal argument %d",
                                         index + 1);
            }
        }
        arguments[index] = QGenericArgument(parameterTypeNames.at(index).constData(),
                                             converted.last().constData());
    }
    if (!closure->signal.invoke(closure->object, Qt::DirectConnection,
                                arguments[0], arguments[1], arguments[2], arguments[3],
                                arguments[4], arguments[5], arguments[6], arguments[7],
                                arguments[8], arguments[9]))
        return JS_ThrowInternalError(context, "Unable to emit QObject signal");
    return JS_UNDEFINED;
}

JSValue signalOperation(JSContext *context, JSValueConst, int argc, JSValueConst *argv,
                        int, void *opaque)
{
    auto *closure = static_cast<SignalClosure *>(opaque);
    if (!closure || !closure->object || !closure->state || !closure->state->engine) {
        const bool connect = closure && closure->connect;
        return JS_ThrowTypeError(context, connect
            ? "Function.prototype.connect: cannot connect to deleted QObject"
            : "Function.prototype.discconnect: cannot disconnect from deleted QObject");
    }
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(closure->state->engine);
    QScriptValue receiver;
    int callbackIndex = 0;
    if (argc >= 2) {
        receiver = engine->fromBorrowed(argv[0]);
        callbackIndex = 1;
    }
    if (argc < callbackIndex + 1)
        return JS_ThrowTypeError(context, closure->connect
            ? "Function.prototype.connect: target is not a function"
            : "Function.prototype.disconnect: target is not a function");
    QScriptValue callback;
    if (JS_IsFunction(context, argv[callbackIndex])) {
        callback = engine->fromBorrowed(argv[callbackIndex]);
    } else if (receiver.isObject() && JS_IsString(argv[callbackIndex])) {
        callback = receiver.property(engine->fromBorrowed(argv[callbackIndex]).toString());
    } else {
        return JS_ThrowTypeError(context, closure->connect
            ? "Function.prototype.connect: target is not a function"
            : "Function.prototype.disconnect: target is not a function");
    }
    if (!callback.isFunction())
        return JS_ThrowTypeError(context, closure->connect
            ? "Function.prototype.connect: target is not a function"
            : "Function.prototype.disconnect: target is not a function");
    if (receiver.isObject() && receiver.engine() != callback.engine())
        return JS_ThrowTypeError(context, "Signal receiver belongs to another engine");

    QMetaMethod signal = closure->signal;
    // A generic signal name is connectable when the meta-object has a single
    // signal entry (including signals with default arguments).  Truly
    // overloaded signal names are rejected below; the signature properties
    // created by populateQObject bypass this check.
    if (!closure->signatureSpecific) {
        const QMetaObject *metaObject = closure->object->metaObject();
        int bestParameterCount = -1;
        bool ambiguous = false;
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.methodType() != QMetaMethod::Signal
                || method.name() != closure->signal.name())
                continue;
            if (method.parameterCount() > bestParameterCount) {
                signal = method;
                bestParameterCount = method.parameterCount();
                ambiguous = false;
            } else if (method.parameterCount() == bestParameterCount
                       && method.methodSignature() != signal.methodSignature()) {
                ambiguous = true;
            }
        }
        if (ambiguous) {
            QStringList candidates;
            for (int index = 0; index < metaObject->methodCount(); ++index) {
                const QMetaMethod method = metaObject->method(index);
                if (method.methodType() == QMetaMethod::Signal
                    && method.name() == closure->signal.name())
                    candidates.append(QString::fromLatin1(method.methodSignature()));
            }
            const QString message = QStringLiteral(
                "Function.prototype.connect: ambiguous connect to %1::%2(); candidates are\n%3\n"
                "Use e.g. object['%4'].connect() to connect to a particular overload")
                .arg(QString::fromLatin1(closure->object->metaObject()->className()))
                .arg(QString::fromLatin1(closure->signal.name()))
                .arg([&candidates] {
                    QString result;
                    for (const QString &candidate : candidates)
                        result += QStringLiteral("    ") + candidate + QLatin1Char('\n');
                    if (!result.isEmpty())
                        result.chop(1);
                    return result;
                }())
                .arg(candidates.isEmpty() ? QString() : candidates.constLast());
            JSValue error = JS_NewError(context);
            const QByteArray errorMessage = message.toUtf8();
            JS_SetPropertyStr(context, error, "message",
                              JS_NewStringLen(context, errorMessage.constData(),
                                              errorMessage.size()));
            return JS_Throw(context, error);
        }
    }
    const qint64 callbackId = callback.objectId();
    if (closure->connect) {
        auto *slot = new ScriptSignalSlotObject(callback, signal, closure->state, receiver);
        const QMetaObject::Connection connection = QObjectPrivate::connect(
            closure->object, signal.methodIndex(), slot, Qt::AutoConnection);
        if (!connection)
            return JS_ThrowInternalError(context, "Unable to connect QObject signal");
        closure->state->signalConnections.append(QScriptSignalConnection{
            closure->object, signal.methodIndex(), callbackId,
            receiver.isObject() ? receiver.objectId() : -1, connection});
        return JS_UNDEFINED;
    }

    bool disconnected = false;
    for (auto it = closure->state->signalConnections.begin();
         it != closure->state->signalConnections.end();) {
        if (it->sender == closure->object && it->signalIndex == signal.methodIndex()
            && it->callbackId == callbackId
            && it->receiverId == (receiver.isObject() ? receiver.objectId() : -1)) {
            disconnected = QObject::disconnect(it->connection) || disconnected;
            it = closure->state->signalConnections.erase(it);
        } else {
            ++it;
        }
    }
    if (!disconnected) {
        const QByteArray message = QByteArrayLiteral("Function.prototype.disconnect: "
                                                      "failed to disconnect from ")
            + closure->object->metaObject()->className() + QByteArrayLiteral("::")
            + closure->signal.methodSignature();
        JSValue error = JS_NewError(context);
        JS_SetPropertyStr(context, error, "message",
                          JS_NewStringLen(context, message.constData(),
                                          size_t(message.size())));
        return JS_Throw(context, error);
    }
    return JS_UNDEFINED;
}

static JSValue makeSignalObject(JSContext *context, QObject *object,
                                const QMetaMethod &signal,
                                QScriptEngineState *state, bool signatureSpecific,
                                const char *name)
{
    auto *callClosure = new SignalClosure{object, signal, state, true, signatureSpecific};
    JSValue signalObject = makeClosure(context, signalCall, name,
                                       deleteSignalClosure, callClosure);
    auto *connectClosure = new SignalClosure{object, signal, state, true, signatureSpecific};
    auto *disconnectClosure = new SignalClosure{object, signal, state, false,
                                                signatureSpecific};
    JS_SetPropertyStr(context, signalObject, "connect",
                      makeClosure(context, signalOperation, "connect",
                                  deleteSignalClosure, connectClosure));
    JS_SetPropertyStr(context, signalObject, "disconnect",
                      makeClosure(context, signalOperation, "disconnect",
                                  deleteSignalClosure, disconnectClosure));
    return signalObject;
}

JSValue makeClosure(JSContext *context, JSCClosure *function, const char *name,
                    JSCClosureFinalizerFunc *finalizer, void *opaque)
{
    return JS_NewCClosure(context, function, name, finalizer, 1, 0, opaque);
}

} // unnamed namespace

static QMetaMethod qScriptQuickJSSignal(QObject *sender, const char *signal)
{
    if (!sender || !signal)
        return QMetaMethod();
    QByteArray signature(signal);
    if (!signature.isEmpty() && signature.front() >= '0' && signature.front() <= '2')
        signature.remove(0, 1);
    signature = QMetaObject::normalizedSignature(signature.constData());
    const int index = sender->metaObject()->indexOfSignal(signature.constData());
    return index >= 0 ? sender->metaObject()->method(index) : QMetaMethod();
}

static int qScriptQuickJSObjectEquality(JSContext *context, JSValueConst left,
                                        JSValueConst right, void *opaque)
{
    auto *state = static_cast<QScriptEngineState *>(opaque);
    if (!state || state->context != context)
        return -1;

    // Look up the direct QObject wrapper identity for both operands.  Only the
    // direct class payload is inspected here: looking up the hidden property
    // used by object-backed wrappers can invoke the object's exotic property
    // hooks while QuickJS is already performing equality, which is not a
    // re-entrant-safe operation.
    const QObjectPayload *leftPayload = nullptr;
    const QObjectPayload *rightPayload = nullptr;
    if (JS_IsObject(left) && JS_GetClassID(left) == state->qobjectClassId) {
        leftPayload = static_cast<const QObjectPayload *>(
            JS_GetOpaque(left, state->qobjectClassId));
    }
    if (JS_IsObject(right) && JS_GetClassID(right) == state->qobjectClassId) {
        rightPayload = static_cast<const QObjectPayload *>(
            JS_GetOpaque(right, state->qobjectClassId));
    }

    if (leftPayload || rightPayload) {
        if (!leftPayload || !rightPayload)
            return 0;
        return leftPayload->object.data() == rightPayload->object.data();
    }

    // QVariant wrappers compare by their underlying variant value, matching
    // both QScriptValue::equals() and the JSC backend contract where
    // `newVariant(false) == newVariant(false)` is true.  Reading the direct
    // class payload is re-entrant-safe (no property lookups).
    const QScriptVariantPayload *leftVariant = nullptr;
    const QScriptVariantPayload *rightVariant = nullptr;
    if (JS_IsObject(left) && JS_GetClassID(left) == state->variantClassId) {
        leftVariant = static_cast<const QScriptVariantPayload *>(
            JS_GetOpaque(left, state->variantClassId));
    }
    if (JS_IsObject(right) && JS_GetClassID(right) == state->variantClassId) {
        rightVariant = static_cast<const QScriptVariantPayload *>(
            JS_GetOpaque(right, state->variantClassId));
    }
    if (leftVariant && rightVariant)
        return leftVariant->value == rightVariant->value;
    if (leftVariant || rightVariant)
        return 0;

    return -1;
}
void qScriptQuickJSRegisterQObjectClass(QScriptEngineState *state)
{
    JS_NewClassID(state->runtime, &state->qobjectClassId);
    JSClassDef definition{};
    definition.class_name = "QObject";
    definition.finalizer = qobjectFinalizer;
    definition.exotic = &qobjectExoticMethods;
    JS_NewClass(state->runtime, state->qobjectClassId, &definition);
    auto *prototypePayload = new QObjectPayload;
    prototypePayload->state = state;
    JSValue prototype = JS_NewObjectClass(state->context, state->qobjectClassId);
    JS_SetOpaque(prototype, prototypePayload);
    JSValue object = JS_NewObject(state->context);
    JSValue objectPrototype = JS_GetPrototype(state->context, object);
    JS_SetPrototype(state->context, prototype, objectPrototype);
    JS_FreeValue(state->context, objectPrototype);
    JS_FreeValue(state->context, object);
    JS_SetClassProto(state->context, state->qobjectClassId, prototype);
    JS_SetObjectEqualityHandler(state->runtime, qScriptQuickJSObjectEquality, state);
}

void qScriptQuickJSInvalidateQObjectMethods(QScriptEngineState *state, QObject *object)
{
    if (!state || !state->context || !object)
        return;
    const auto wrappers = state->qobjectWrappers.constFind(object);
    if (wrappers == state->qobjectWrappers.cend())
        return;

    for (const QScriptQObjectWrapper &wrapper : wrappers.value()) {
        if (!JS_IsObject(wrapper.value))
            continue;
        const QObjectPayload *payload = payloadFor(state, wrapper.value);
        if (!payload)
            continue;
        for (const QByteArray &name : payload->methodProperties) {
            const JSAtom atom = JS_NewAtomLen(state->context, name.constData(),
                                              size_t(name.size()));
            if (atom == JS_ATOM_NULL)
                continue;
            JS_DeleteProperty(state->context, wrapper.value, atom, JS_PROP_NO_EXOTIC);
            JS_FreeAtom(state->context, atom);
            if (JS_HasException(state->context))
                JS_FreeValue(state->context, JS_GetException(state->context));
        }
    }
}

QScriptValue QScriptEnginePrivate::wrapQObject(const QScriptValue &base, QObject *object,
                                                QScriptEngine::ValueOwnership ownership,
                                                QScriptEngine::QObjectWrapOptions options)
{
    if (!object)
        return q_func()->nullValue();

    const QScriptEngine::QObjectWrapOptions wrapperOptions =
        options & ~QScriptEngine::QObjectWrapOptions(
            QScriptEngine::PreferExistingWrapperObject);
    const auto existing = state->qobjectWrappers.constFind(object);
    if (!base.isObject() && options.testFlag(QScriptEngine::PreferExistingWrapperObject)
        && existing != state->qobjectWrappers.cend()) {
        for (const QScriptQObjectWrapper &wrapper : existing.value()) {
            if (wrapper.ownership == ownership && wrapper.options == wrapperOptions)
                return fromBorrowed(wrapper.value);
        }
    }

    JSValue wrapper;
    bool baseOk = false;
    if (base.isObject())
        wrapper = toQuickJS(base, &baseOk);
    if (!baseOk)
        wrapper = JS_NewObjectClass(state->context, state->qobjectClassId);

    JSValue payloadObject = wrapper;
    if (JS_GetClassID(wrapper) != state->qobjectClassId) {
        payloadObject = JS_NewObjectClass(state->context, state->qobjectClassId);
        JS_DefinePropertyValueStr(state->context, wrapper, "__qtscript_qobject__",
                                  JS_DupValue(state->context, payloadObject), 0);
    }
    JS_SetOpaque(payloadObject, new QObjectPayload{object, ownership, state.data(), wrapper,
                                                   options});
    if (JS_VALUE_GET_PTR(payloadObject) != JS_VALUE_GET_PTR(wrapper))
        JS_FreeValue(state->context, payloadObject);

    populateQObject(wrapper, object, options);
    QScriptValue result = fromOwned(wrapper);
    if (!base.isObject()) {
        const QByteArray pointerTypeName = QByteArray(object->metaObject()->className()) + '*';
        const QMetaType pointerType = QMetaType::fromName(pointerTypeName);
        QScriptValue prototype = pointerType.isValid()
            ? state->defaultPrototypes.value(pointerType.id())
            : QScriptValue();
        if (!prototype.isValid())
            prototype = state->defaultPrototypes.value(qMetaTypeId<QObject *>());
        if (prototype.isValid())
            result.setPrototype(prototype);
    }
    QScriptQObjectWrapper wrapperRecord;
    wrapperRecord.value = wrapper;
    wrapperRecord.ownership = ownership;
    wrapperRecord.options = wrapperOptions;
    state->qobjectWrappers[object].append(wrapperRecord);
    QObject::connect(object, &QObject::destroyed, q_func(),
                     [this](QObject *destroyed) { _q_objectDestroyed(destroyed); });
    return result;
}

void QScriptEnginePrivate::populateQObject(JSValueConst wrapper, QObject *object,
                                            QScriptEngine::QObjectWrapOptions options)
{
    const QMetaObject *metaObject = object->metaObject();
    QObjectPayload *payload = payloadFor(state.data(), wrapper);
    const int propertyStart = (options.testFlag(QScriptEngine::ExcludeSuperClassProperties)
                               || options.testFlag(QScriptEngine::ExcludeSuperClassContents))
        ? metaObject->propertyOffset() : 0;
    for (int index = propertyStart; index < metaObject->propertyCount(); ++index) {
        const QMetaProperty property = metaObject->property(index);
        if (!property.isScriptable() || !property.isValid()
            || metaObject->indexOfProperty(property.name()) != index)
            continue;
        auto *getterClosure = new PropertyClosure{object, index, state.data(),
                                                  PropertyClosure::MetaProperty,
                                                  property.name()};
        JSValue getter = property.isReadable()
            ? makeClosure(state->context, propertyGetter, property.name(),
                          deletePropertyClosure, getterClosure)
            : JS_UNDEFINED;
        JSValue setter = JS_UNDEFINED;
        if (property.isWritable()) {
            auto *setterClosure = new PropertyClosure{object, index, state.data(),
                                                      PropertyClosure::MetaProperty,
                                                      property.name()};
            setter = makeClosure(state->context, propertySetter, property.name(),
                                 deletePropertyClosure, setterClosure);
        }
        const JSAtom atom = JS_NewAtom(state->context, property.name());
        JS_DefinePropertyGetSet(state->context, wrapper, atom, getter, setter,
                                JS_PROP_ENUMERABLE);
        JS_FreeAtom(state->context, atom);
    }

    // Dynamic QObject properties follow the meta-properties in QtScript's
    // enumeration order, while still using accessors so that reads and writes
    // stay connected to QObject::property().
    if (JS_GetClassID(wrapper) == state->qobjectClassId) {
        for (const QByteArray &name : object->dynamicPropertyNames()) {
            if (!dynamicPropertyExists(object, name))
                continue;
            auto *getterClosure = new PropertyClosure{
                object, -1, state.data(), PropertyClosure::DynamicProperty, name};
            auto *setterClosure = new PropertyClosure{
                object, -1, state.data(), PropertyClosure::DynamicProperty, name};
            JSValue getter = makeClosure(state->context, propertyGetter, name.constData(),
                                         deletePropertyClosure, getterClosure);
            JSValue setter = makeClosure(state->context, propertySetter, name.constData(),
                                         deletePropertyClosure, setterClosure);
            const JSAtom atom = JS_NewAtom(state->context, name.constData());
            JS_DefinePropertyGetSet(state->context, wrapper, atom, getter, setter,
                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
            JS_FreeAtom(state->context, atom);
            if (payload)
                payload->directDynamicProperties.insert(name);
        }
    }

    QSet<QByteArray> installed;
    const int methodStart = (options.testFlag(QScriptEngine::ExcludeSuperClassMethods)
                             || options.testFlag(QScriptEngine::ExcludeSuperClassContents))
        ? metaObject->methodOffset() : 0;
    for (int index = methodStart; index < metaObject->methodCount(); ++index) {
        const QMetaMethod method = metaObject->method(index);
        const QByteArray name = method.name();
        if (name.isEmpty())
            continue;
        if (!hasMethodAccess(method, index, options))
            continue;
        if (method.methodType() == QMetaMethod::Signal) {
            if (!installed.contains(name)) {
                auto *callClosure = new SignalClosure{object, method, state.data(), true, false};
                JSValue signalObject = makeClosure(state->context, signalCall, name.constData(),
                                                   deleteSignalClosure, callClosure);
                auto *connectClosure = new SignalClosure{object, method, state.data(), true, false};
                auto *disconnectClosure = new SignalClosure{object, method, state.data(), false, false};
                JS_SetPropertyStr(state->context, signalObject, "connect",
                    makeClosure(state->context, signalOperation, "connect", deleteSignalClosure,
                                connectClosure));
                JS_SetPropertyStr(state->context, signalObject, "disconnect",
                    makeClosure(state->context, signalOperation, "disconnect", deleteSignalClosure,
                                disconnectClosure));
                JS_DefinePropertyValueStr(state->context, wrapper, name.constData(), signalObject,
                                          JS_PROP_CONFIGURABLE);
                installed.insert(name);
                if (payload)
                    payload->methodProperties.insert(name);
            }
            const QByteArray signature = method.methodSignature();
            if (!signature.isEmpty() && !installed.contains(signature)) {
                auto *signatureCallClosure = new SignalClosure{
                    object, method, state.data(), true, true};
                JSValue signatureObject = makeClosure(state->context, signalCall,
                                                       signature.constData(),
                                                       deleteSignalClosure,
                                                       signatureCallClosure);
                auto *signatureConnectClosure = new SignalClosure{
                    object, method, state.data(), true, true};
                auto *signatureDisconnectClosure = new SignalClosure{
                    object, method, state.data(), false, true};
                JS_SetPropertyStr(state->context, signatureObject, "connect",
                    makeClosure(state->context, signalOperation, "connect",
                                deleteSignalClosure, signatureConnectClosure));
                JS_SetPropertyStr(state->context, signatureObject, "disconnect",
                    makeClosure(state->context, signalOperation, "disconnect",
                                deleteSignalClosure, signatureDisconnectClosure));
                const int signatureFlags = JS_PROP_CONFIGURABLE
                    | (options.testFlag(QScriptEngine::SkipMethodsInEnumeration)
                           ? 0 : JS_PROP_ENUMERABLE);
                JS_DefinePropertyValueStr(state->context, wrapper, signature.constData(),
                                          signatureObject, signatureFlags);
                installed.insert(signature);
                if (payload)
                    payload->methodProperties.insert(signature);
            }
            continue;
        }
        if (options.testFlag(QScriptEngine::ExcludeSlots)
            && method.methodType() == QMetaMethod::Slot)
            continue;
        const int flags = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE
            | (options.testFlag(QScriptEngine::SkipMethodsInEnumeration)
                   ? 0 : JS_PROP_ENUMERABLE);
        const int nameFlags = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
        if (!installed.contains(name)) {
            auto *closure = new MethodClosure{object, name, {}, state.data()};
            JSValue function = makeClosure(state->context, methodCall, name.constData(),
                                           deleteMethodClosure, closure);
            const QByteArray marker = QByteArray(metaObject->className()) + "::"
                + method.methodSignature();
            JS_DefinePropertyValueStr(state->context, function,
                                      "__qtscript_qobject_method__",
                                      JS_NewStringLen(state->context, marker.constData(),
                                                      size_t(marker.size())), 0);
            JS_DefinePropertyValueStr(state->context, wrapper, name.constData(), function,
                                      nameFlags);
            installed.insert(name);
            if (payload)
                payload->methodProperties.insert(name);
        }
        const QByteArray signature = method.methodSignature();
        if (!signature.isEmpty() && !installed.contains(signature)) {
            auto *signatureClosure = new MethodClosure{object, name, signature, state.data()};
            JSValue signatureFunction = makeClosure(state->context, methodCall,
                                                    signature.constData(),
                                                    deleteMethodClosure, signatureClosure);
            const QByteArray marker = QByteArray(metaObject->className()) + "::"
                + method.methodSignature();
            JS_DefinePropertyValueStr(state->context, signatureFunction,
                                      "__qtscript_qobject_method__",
                                      JS_NewStringLen(state->context, marker.constData(),
                                                      size_t(marker.size())), 0);
            JS_DefinePropertyValueStr(state->context, wrapper, signature.constData(),
                                      signatureFunction, flags);
            installed.insert(signature);
            if (payload)
                payload->methodProperties.insert(signature);
        }
    }

    for (const QByteArray &name : {QByteArrayLiteral("findChild"),
                                   QByteArrayLiteral("findChildren")}) {
        if (installed.contains(name))
            continue;
        auto *closure = new MethodClosure{object, name, {}, state.data()};
        JSValue function = makeClosure(state->context, methodCall, name.constData(),
                                       deleteMethodClosure, closure);
        JS_DefinePropertyValueStr(state->context, wrapper, name.constData(), function,
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        installed.insert(name);
        if (payload)
            payload->methodProperties.insert(name);
    }

    if (JS_GetClassID(wrapper) != state->qobjectClassId) {
        for (const QByteArray &name : object->dynamicPropertyNames()) {
            if (installed.contains(name))
                continue;
            bool ok = false;
            JSValue value = toQuickJS(fromVariant(object->property(name.constData())), &ok);
            if (ok)
                JS_DefinePropertyValueStr(state->context, wrapper, name.constData(), value,
                                          JS_PROP_C_W_E);
        }
    }
}

bool qScriptConnect(QObject *sender, const char *signal,
                    const QScriptValue &receiver, const QScriptValue &function)
{
    if (!sender || !function.isFunction() || !function.engine())
        return false;
    if (receiver.isObject() && receiver.engine() != function.engine())
        return false;
    const QMetaMethod method = qScriptQuickJSSignal(sender, signal);
    if (!method.isValid())
        return false;

    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(function.engine());
    auto *slot = new ScriptSignalSlotObject(function, method, engine->state.data(), receiver);
    const QMetaObject::Connection connection = QObjectPrivate::connect(
        sender, method.methodIndex(), slot, Qt::AutoConnection);
    if (!connection)
        return false;
    if (!engine->state->signalCleanupSenders.contains(sender)) {
        engine->state->signalCleanupSenders.insert(sender);
        QObject::connect(sender, &QObject::destroyed, function.engine(),
                         [state = engine->state](QObject *destroyed) {
                             state->signalCleanupSenders.remove(destroyed);
                             for (auto it = state->signalConnections.begin();
                                  it != state->signalConnections.end();) {
                                 if (it->sender == destroyed)
                                     it = state->signalConnections.erase(it);
                                 else
                                     ++it;
                             }
                         });
    }
    engine->state->signalConnections.append(QScriptSignalConnection{
        sender, method.methodIndex(), function.objectId(),
        receiver.isObject() ? receiver.objectId() : -1, connection});
    return true;
}

bool qScriptDisconnect(QObject *sender, const char *signal,
                       const QScriptValue &receiver, const QScriptValue &function)
{
    if (!sender || !function.isFunction() || !function.engine())
        return false;
    if (receiver.isObject() && receiver.engine() != function.engine())
        return false;
    const QMetaMethod method = qScriptQuickJSSignal(sender, signal);
    if (!method.isValid())
        return false;

    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(function.engine());
    const qint64 callbackId = function.objectId();
    const qint64 receiverId = receiver.isObject() ? receiver.objectId() : -1;
    bool disconnected = false;
    for (auto it = engine->state->signalConnections.begin();
         it != engine->state->signalConnections.end();) {
        if (it->sender == sender && it->signalIndex == method.methodIndex()
            && it->callbackId == callbackId && it->receiverId == receiverId) {
            disconnected |= QObject::disconnect(it->connection);
            it = engine->state->signalConnections.erase(it);
        } else {
            ++it;
        }
    }
    return disconnected;
}

QT_END_NAMESPACE
