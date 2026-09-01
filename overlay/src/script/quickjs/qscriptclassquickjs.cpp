/****************************************************************************
**
** QtScript custom-class API support for the QuickJS-NG backend.
**
****************************************************************************/

#include "qscriptquickjs_p.h"

#include <QtScript/qscriptclass.h>
#include <QtScript/qscriptclasspropertyiterator.h>
#include <QtScript/qscriptstring.h>

#include <QtCore/qset.h>

#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE

class QScriptClassPrivate
{
    Q_DECLARE_PUBLIC(QScriptClass)

public:
    QScriptClassPrivate() = default;
    virtual ~QScriptClassPrivate() = default;

    QScriptEngine *engine = nullptr;
    QScriptClass *q_ptr = nullptr;
};

Q_DECLARE_METATYPE(QScriptContext *)
Q_DECLARE_METATYPE(QScriptValueList)

namespace {

constexpr char classBindingProperty[] = "__qtscript_class_binding__";

static void freeQuickJSDescriptor(JSContext *context, JSPropertyDescriptor *descriptor)
{
    JS_FreeValue(context, descriptor->value);
    JS_FreeValue(context, descriptor->getter);
    JS_FreeValue(context, descriptor->setter);
}

static bool isCppCreatedObject(QScriptEngineState *state, JSValueConst value)
{
    if (!state || !state->context || !JS_IsObject(value))
        return false;
    const JSAtom marker = state->cppObjectMarkerAtom;
    if (marker == JS_ATOM_NULL)
        return false;
    JSPropertyDescriptor descriptor{};
    const int result = JS_GetOwnProperty(state->context, &descriptor, value, marker);
    if (result <= 0) {
        if (result < 0)
            freeQuickJSDescriptor(state->context, &descriptor);
        return false;
    }
    const bool isCppObject = JS_IsBool(descriptor.value)
        && JS_ToBool(state->context, descriptor.value) > 0;
    freeQuickJSDescriptor(state->context, &descriptor);
    return isCppObject;
}

static QScriptClassObjectData *classData(QScriptEngineState *state,
                                         JSValueConst value)
{
    if (!state || !state->scriptClassClassId || !JS_IsObject(value))
        return nullptr;
    return static_cast<QScriptClassObjectData *>(
        JS_GetOpaque(value, state->scriptClassClassId));
}

static QScriptValue classObjectValue(QScriptEngineState *state, JSValueConst value,
                                     QScriptClassObjectData *data = nullptr)
{
    if (data && data->prototypeBinding && !JS_IsUndefined(data->boundObject))
        value = data->boundObject;
    return state && state->engine
        ? QScriptEnginePrivate::get(state->engine)->fromBorrowed(value)
        : QScriptValue();
}

static bool atomName(JSContext *context, JSAtom atom, QByteArray *name)
{
    JSValue string = JS_AtomToString(context, atom);
    if (JS_IsException(string) || JS_IsSymbol(string)) {
        JS_FreeValue(context, string);
        return false;
    }
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, string);
    if (!text) {
        JS_FreeValue(context, string);
        return false;
    }
    *name = QByteArray(text, int(length));
    JS_FreeCString(context, text);
    JS_FreeValue(context, string);
    return true;
}

static QScriptClassObjectData *classDataForValue(QScriptEngineState *state,
                                                 JSValueConst value);

static QScriptClass::QueryFlags queryClass(QScriptEngineState *state,
                                           JSValueConst object, JSAtom atom,
                                           QScriptClass::QueryFlags flags,
                                           QScriptString *name, uint *id)
{
    QScriptClassObjectData *data = classDataForValue(state, object);
    if (!data || !data->scriptClass || !state->engine)
        return {};
    QByteArray text;
    if (!atomName(state->context, atom, &text))
        return {};
    if (text.startsWith("__qtscript_") || text.startsWith("__qtscript"))
        return {};
    *name = state->engine->toStringHandle(QString::fromUtf8(text));
    *id = 0;
    return data->scriptClass->queryProperty(
        classObjectValue(state, object, data), *name, flags, id);
}

static QScriptClassObjectData *bindingData(QScriptEngineState *state,
                                           JSValueConst object)
{
    if (!state || !state->context || !JS_IsObject(object))
        return nullptr;
    JSAtom marker = JS_NewAtom(state->context, classBindingProperty);
    if (marker == JS_ATOM_NULL)
        return nullptr;
    JSPropertyDescriptor descriptor{};
    const int result = JS_GetOwnProperty(state->context, &descriptor, object, marker);
    JS_FreeAtom(state->context, marker);
    if (result <= 0) {
        if (result < 0) {
            JS_FreeValue(state->context, descriptor.value);
            JS_FreeValue(state->context, descriptor.getter);
            JS_FreeValue(state->context, descriptor.setter);
        }
        return nullptr;
    }
    QScriptClassObjectData *data = classData(state, descriptor.value);
    JS_FreeValue(state->context, descriptor.value);
    JS_FreeValue(state->context, descriptor.getter);
    JS_FreeValue(state->context, descriptor.setter);
    return data;
}

static QScriptClassObjectData *classDataForValue(QScriptEngineState *state,
                                                 JSValueConst value)
{
    if (QScriptClassObjectData *data = classData(state, value))
        return data;
    return bindingData(state, value);
}

static JSValue classPropertyValue(JSContext *context, JSValueConst object,
                                  QScriptClassObjectData *data, JSAtom atom,
                                  uint id, bool *handled)
{
    *handled = false;
    if (!data || !data->scriptClass || !data->state || !data->state->engine)
        return JS_UNDEFINED;
    QScriptString name;
    QByteArray text;
    if (!atomName(context, atom, &text))
        return JS_UNDEFINED;
    name = data->state->engine->toStringHandle(QString::fromUtf8(text));
    QScriptClass::QueryFlags query = data->scriptClass->queryProperty(
        classObjectValue(data->state, object, data), name,
        QScriptClass::HandlesReadAccess, &id);
    if (!(query & QScriptClass::HandlesReadAccess))
        return JS_UNDEFINED;
    *handled = true;
    const QScriptValue value = data->scriptClass->property(
        classObjectValue(data->state, object, data), name, id);
    bool ok = false;
    JSValue result = QScriptEnginePrivate::get(data->state->engine)->toQuickJS(value, &ok);
    if (!ok)
        return JS_UNDEFINED;
    return result;
}

static int scriptClassGetOwnProperty(JSContext *context, JSPropertyDescriptor *descriptor,
                                     JSValueConst object, JSAtom atom)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classDataForValue(state, object);
    if (!data || !data->scriptClass || !state->engine)
        return 0;

    QScriptString name;
    uint id = 0;
    const QScriptClass::QueryFlags query = queryClass(
        state, object, atom, QScriptClass::HandlesReadAccess, &name, &id);
    if (!(query & QScriptClass::HandlesReadAccess))
        return 0;
    if (!descriptor)
        return 1;

    const QScriptValue value = data->scriptClass->property(
        classObjectValue(state, object, data), name, id);
    bool ok = false;
    descriptor->value = QScriptEnginePrivate::get(state->engine)->toQuickJS(value, &ok);
    if (!ok)
        descriptor->value = JS_UNDEFINED;
    descriptor->getter = JS_UNDEFINED;
    descriptor->setter = JS_UNDEFINED;
    const QScriptValue::PropertyFlags flags = data->scriptClass->propertyFlags(
        classObjectValue(state, object, data), name, id);
    descriptor->flags = qScriptQuickJSPropertyFlags(flags)
        | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE
        | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;
    return 1;
}

static int scriptClassGetOwnPropertyNames(JSContext *context,
                                          JSPropertyEnum **properties,
                                          uint32_t *count, JSValueConst object)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classDataForValue(state, object);
    *properties = nullptr;
    *count = 0;
    if (!data || !data->scriptClass || !state->engine)
        return 0;

    std::vector<QScriptString> names;
    std::unique_ptr<QScriptClassPropertyIterator> iterator(
        data->scriptClass->newIterator(classObjectValue(state, object, data)));
    if (!iterator)
        return 0;
    iterator->toFront();
    while (iterator->hasNext()) {
        iterator->next();
        const QScriptString name = iterator->name();
        if (!name.isValid())
            continue;
        const QScriptValue::PropertyFlags propertyFlags = iterator->flags();
        if (propertyFlags & QScriptValue::SkipInEnumeration)
            continue;
        names.push_back(name);
    }
    if (names.empty())
        return 0;
    auto *result = static_cast<JSPropertyEnum *>(
        js_malloc(context, sizeof(JSPropertyEnum) * names.size()));
    if (!result)
        return -1;
    for (size_t index = 0; index < names.size(); ++index) {
        const QByteArray utf8 = names[index].toString().toUtf8();
        result[index].atom = JS_NewAtomLen(context, utf8.constData(),
                                           size_t(utf8.size()));
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

static int scriptClassDeleteProperty(JSContext *context, JSValueConst object, JSAtom atom)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classDataForValue(state, object);
    if (data && data->scriptClass && state->engine) {
        QScriptString name;
        uint id = 0;
        const QScriptClass::QueryFlags query = queryClass(
            state, object, atom, QScriptClass::HandlesWriteAccess, &name, &id);
        if (query & QScriptClass::HandlesWriteAccess) {
            const QScriptValue::PropertyFlags flags = data->scriptClass->propertyFlags(
                classObjectValue(state, object, data), name, id);
            if (flags & QScriptValue::Undeletable)
                return false;
            QScriptValue objectValue = classObjectValue(state, object, data);
            data->scriptClass->setProperty(objectValue, name, id, QScriptValue());
            return true;
        }
    }
    return JS_DeleteProperty(context, object, atom, JS_PROP_NO_EXOTIC);
}

static int scriptClassSetProperty(JSContext *context, JSValueConst object, JSAtom atom,
                                  JSValueConst value, JSValueConst receiver, int flags)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classDataForValue(state, object);
    if (data && data->scriptClass && state->engine) {
        QScriptString name;
        uint id = 0;
        const QScriptClass::QueryFlags query = queryClass(
            state, object, atom, QScriptClass::HandlesWriteAccess, &name, &id);
        if (query & QScriptClass::HandlesWriteAccess) {
            if (JS_IsObject(receiver)
                && !JS_IsStrictEqual(context, receiver, object)) {
                JSPropertyDescriptor existing{};
                const bool hasOwn = JS_GetOwnProperty(context, &existing,
                                                      receiver, atom) > 0;
                if (hasOwn) {
                    freeQuickJSDescriptor(context, &existing);
                    QScriptString readName;
                    uint readId = id;
                    const QScriptClass::QueryFlags readQuery = queryClass(
                        state, object, atom, QScriptClass::HandlesReadAccess,
                        &readName, &readId);
                    if (readQuery & QScriptClass::HandlesReadAccess)
                        return true;
                }
            }
            QScriptValue objectValue = classObjectValue(state, object, data);
            data->scriptClass->setProperty(objectValue, name, id,
                                           QScriptEnginePrivate::get(state->engine)
                                               ->fromBorrowed(value));
            return true;
        }
    }

    const JSValueConst target = JS_IsObject(receiver) ? receiver : object;
    const int result = JS_DefineProperty(context, target, atom, value,
                                         JS_UNDEFINED, JS_UNDEFINED,
                                         JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE
                                         | JS_PROP_HAS_ENUMERABLE
                                         | JS_PROP_HAS_CONFIGURABLE
                                         | JS_PROP_C_W_E);
    if (result <= 0 && (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT)))
        JS_ThrowTypeError(context, "Unable to set QScriptClass property");
    return result;
}

static JSClassExoticMethods scriptClassExoticMethods{
    scriptClassGetOwnProperty,
    scriptClassGetOwnPropertyNames,
    scriptClassDeleteProperty,
    nullptr,
    nullptr,
    nullptr,
    scriptClassSetProperty
};

static void scriptClassFinalizer(JSRuntime *, JSValueConst value)
{
    QScriptClassObjectData *data = static_cast<QScriptClassObjectData *>(
        JS_GetOpaque(value, JS_GetClassID(value)));
    if (!data)
        return;
    if (data->state) {
        data->state->scriptClassObjects.removeOne(data);
        if (data->state->context && !JS_IsUndefined(data->originalPrototype))
            JS_FreeValue(data->state->context, data->originalPrototype);
        if (data->state->context && !JS_IsUndefined(data->boundObject))
            JS_FreeValue(data->state->context, data->boundObject);
    }
    delete data;
}

static QScriptContext *createClassContext(QScriptEngine *engine, JSValueConst callee,
                                          JSValueConst thisValue, int argc,
                                          JSValueConst *argv, bool constructor)
{
    QScriptEngineState *state = QScriptEnginePrivate::get(engine)->state.data();
    QScriptContext *context = QScriptContextPrivate::create();
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(context);
    contextPrivate->engine = engine;
    contextPrivate->parent = state->currentContext;
    contextPrivate->callee = QScriptEnginePrivate::get(engine)->fromBorrowed(callee);
    contextPrivate->thisObject = constructor
        ? engine->newObject()
        : ((JS_IsUndefined(thisValue) || JS_IsNull(thisValue))
           ? engine->globalObject()
           : QScriptEnginePrivate::get(engine)->fromBorrowed(thisValue));
    contextPrivate->activationObject = engine->newObject();
    contextPrivate->scopes.append(contextPrivate->activationObject);
    contextPrivate->scopes.append(engine->globalObject());
    contextPrivate->calledAsConstructor = constructor;
    for (int index = 0; index < argc; ++index)
        contextPrivate->arguments.append(QScriptEnginePrivate::get(engine)->fromBorrowed(argv[index]));
    return context;
}

static JSValue scriptClassCall(JSContext *context, JSValueConst function,
                               JSValueConst thisValue, int argc, JSValueConst *argv,
                               int flags)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classData(state, function);
    if (!data || !data->scriptClass || !data->scriptClass->supportsExtension(QScriptClass::Callable))
        return JS_ThrowTypeError(context, "QScriptClass object is not callable");
    QScriptEngine *engine = state->engine;
    QScriptContext *scriptContext = createClassContext(
        engine, function, thisValue, argc, argv, flags & JS_CALL_FLAG_CONSTRUCTOR);
    QScriptContext *previous = state->currentContext;
    state->currentContext = scriptContext;
    if (state->agent)
        state->agent->contextPush();
    const QVariant returned = data->scriptClass->extension(
        QScriptClass::Callable, QVariant::fromValue(scriptContext));
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(scriptContext);
    const bool threw = contextPrivate->state == QScriptContext::ExceptionState;
    const QScriptValue thrownValue = contextPrivate->thrownValue;
    QScriptValue value;
    if (contextPrivate->returnValue.isValid())
        value = contextPrivate->returnValue;
    else if ((flags & JS_CALL_FLAG_CONSTRUCTOR) && !returned.isValid())
        value = contextPrivate->thisObject;
    else
        value = QScriptEnginePrivate::get(engine)->fromVariant(returned);
    if (state->agent)
        state->agent->contextPop();
    state->currentContext = previous;
    delete scriptContext;
    bool ok = false;
    JSValue result = QScriptEnginePrivate::get(engine)->toQuickJS(
        threw ? thrownValue : value, &ok);
    if (!ok)
        result = JS_UNDEFINED;
    if (threw)
        return JS_Throw(context, result);
    return result;
}

static JSValue scriptClassHasInstance(JSContext *context, JSValueConst thisValue,
                                      int argc, JSValueConst *argv)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    QScriptClassObjectData *data = classData(state, thisValue);
    if (!data || !data->scriptClass || !data->scriptClass->supportsExtension(QScriptClass::HasInstance))
        return JS_FALSE;
    QScriptEngine *engine = state->engine;
    QScriptValueList arguments;
    arguments.append(QScriptEnginePrivate::get(engine)->fromBorrowed(thisValue));
    if (argc > 0)
        arguments.append(QScriptEnginePrivate::get(engine)->fromBorrowed(argv[0]));
    const QVariant returned = data->scriptClass->extension(
        QScriptClass::HasInstance, QVariant::fromValue(arguments));
    return returned.toBool() ? JS_TRUE : JS_FALSE;
}

static void installClassExtensions(QScriptEngineState *state, JSValue object,
                                   QScriptClass *scriptClass)
{
    if (!state || !state->context || !scriptClass)
        return;
    // QuickJS only consults @@hasInstance for constructor-capable right-hand
    // sides.  A QScriptClass may expose HasInstance without being callable;
    // its call hook still rejects actual calls in that case.
    if (scriptClass->supportsExtension(QScriptClass::Callable)
        || scriptClass->supportsExtension(QScriptClass::HasInstance))
        JS_SetConstructorBit(state->context, object, true);
    if (scriptClass->supportsExtension(QScriptClass::HasInstance)) {
        JSValue function = JS_NewCFunction2(state->context,
                                            reinterpret_cast<JSCFunction *>(scriptClassHasInstance),
                                            "[Symbol.hasInstance]", 1,
                                            JS_CFUNC_generic, 0);
        if (state->symbolHasInstanceAtom != JS_ATOM_NULL)
            JS_DefinePropertyValue(state->context, object, state->symbolHasInstanceAtom, function,
                                   JS_PROP_C_W_E);
        else
            JS_FreeValue(state->context, function);
    }
}

} // unnamed namespace

QScriptClass::QScriptClass(QScriptEngine *engine)
    : d_ptr(new QScriptClassPrivate)
{
    d_ptr->q_ptr = this;
    d_ptr->engine = engine;
}

QScriptClass::QScriptClass(QScriptEngine *engine, QScriptClassPrivate &dd)
    : d_ptr(&dd)
{
    d_ptr->q_ptr = this;
    d_ptr->engine = engine;
}

QScriptClass::~QScriptClass()
{
    Q_D(QScriptClass);
    if (!d->engine)
        return;
    QScriptEngineState *state = QScriptEnginePrivate::get(d->engine)->state.data();
    for (QScriptClassObjectData *data : std::as_const(state->scriptClassObjects)) {
        if (data->scriptClass == this)
            data->scriptClass = nullptr;
    }
}

QScriptEngine *QScriptClass::engine() const
{
    Q_D(const QScriptClass);
    return d->engine;
}

QScriptClass::QueryFlags QScriptClass::queryProperty(const QScriptValue &object,
                                                     const QScriptString &name,
                                                     QueryFlags flags, uint *id)
{
    Q_UNUSED(object);
    Q_UNUSED(name);
    Q_UNUSED(flags);
    Q_UNUSED(id);
    return {};
}

QScriptValue QScriptClass::property(const QScriptValue &object,
                                    const QScriptString &name, uint id)
{
    Q_UNUSED(object);
    Q_UNUSED(name);
    Q_UNUSED(id);
    return QScriptValue();
}

void QScriptClass::setProperty(QScriptValue &object, const QScriptString &name,
                               uint id, const QScriptValue &value)
{
    Q_UNUSED(object);
    Q_UNUSED(name);
    Q_UNUSED(id);
    Q_UNUSED(value);
}

QScriptValue::PropertyFlags QScriptClass::propertyFlags(const QScriptValue &object,
                                                         const QScriptString &name,
                                                         uint id)
{
    Q_UNUSED(object);
    Q_UNUSED(name);
    Q_UNUSED(id);
    return {};
}

QScriptClassPropertyIterator *QScriptClass::newIterator(const QScriptValue &object)
{
    Q_UNUSED(object);
    return nullptr;
}

QScriptValue QScriptClass::prototype() const { return QScriptValue(); }
QString QScriptClass::name() const { return QString(); }

bool QScriptClass::supportsExtension(Extension extension) const
{
    Q_UNUSED(extension);
    return false;
}

QVariant QScriptClass::extension(Extension extension, const QVariant &argument)
{
    Q_UNUSED(extension);
    Q_UNUSED(argument);
    return QVariant();
}

class QScriptClassPropertyIteratorPrivate
{
    Q_DECLARE_PUBLIC(QScriptClassPropertyIterator)

public:
    QScriptClassPropertyIteratorPrivate() = default;
    virtual ~QScriptClassPropertyIteratorPrivate() = default;

    QScriptValue object;
    QScriptClassPropertyIterator *q_ptr = nullptr;
};

QScriptClassPropertyIterator::QScriptClassPropertyIterator(const QScriptValue &object)
    : d_ptr(new QScriptClassPropertyIteratorPrivate)
{
    d_ptr->q_ptr = this;
    d_ptr->object = object;
}

QScriptClassPropertyIterator::QScriptClassPropertyIterator(
    const QScriptValue &object, QScriptClassPropertyIteratorPrivate &dd)
    : d_ptr(&dd)
{
    d_ptr->q_ptr = this;
    d_ptr->object = object;
}

QScriptClassPropertyIterator::~QScriptClassPropertyIterator() = default;

QScriptValue QScriptClassPropertyIterator::object() const
{
    Q_D(const QScriptClassPropertyIterator);
    return d->object;
}

uint QScriptClassPropertyIterator::id() const { return 0; }
QScriptValue::PropertyFlags QScriptClassPropertyIterator::flags() const
{
    return object().propertyFlags(name());
}

void qScriptQuickJSRegisterScriptClass(QScriptEngineState *state)
{
    JS_NewClassID(state->runtime, &state->scriptClassClassId);
    JSClassDef definition{};
    definition.class_name = "QScriptClassObject";
    definition.finalizer = scriptClassFinalizer;
    definition.call = scriptClassCall;
    definition.exotic = &scriptClassExoticMethods;
    JS_NewClass(state->runtime, state->scriptClassClassId, &definition);
    JSValue prototype = JS_NewObject(state->context);
    JS_SetClassProto(state->context, state->scriptClassClassId, prototype);
}

QScriptValue qScriptQuickJSNewScriptClassObject(QScriptEnginePrivate *engine,
                                                QScriptClass *scriptClass,
                                                const QScriptValue &data)
{
    QScriptEngineState *state = engine->state.data();
    JSValue object = JS_NewObjectClass(state->context, state->scriptClassClassId);
    // Keep the object constructor-capable even when supportsExtension() is
    // changed after the object was created; the call hook still rejects
    // construction while the class does not support Callable.
    JS_SetConstructorBit(state->context, object, true);
    auto *objectData = new QScriptClassObjectData{state, scriptClass, JS_UNDEFINED,
                                                  JS_UNDEFINED, false};
    JS_SetOpaque(object, objectData);
    state->scriptClassObjects.append(objectData);
    if (scriptClass) {
        const QScriptValue prototype = scriptClass->prototype();
        bool ok = false;
        JSValue prototypeValue = JS_UNDEFINED;
        if (prototype.engine() && prototype.engine() != state->engine) {
            QScriptValue objectValue = engine->fromBorrowed(object);
            objectValue.setPrototype(prototype);
        } else {
            prototypeValue = engine->toQuickJS(prototype, &ok);
        }
        if (ok && JS_IsObject(prototypeValue)) {
            JS_SetPrototype(state->context, object, prototypeValue);
        }
        JS_FreeValue(state->context, prototypeValue);
        installClassExtensions(state, object, scriptClass);
    }
    QScriptValue result = engine->fromOwned(object);
    if (data.isValid())
        result.setData(data);
    return result;
}

QScriptClass *qScriptQuickJSScriptClass(const QScriptValue &value)
{
    QScriptValuePrivate *privateValue = QScriptValuePrivate::get(value);
    if (!privateValue || privateValue->kind != QScriptValuePrivate::QuickJSValue
        || !privateValue->state || !privateValue->state->context
        || !JS_IsObject(privateValue->value))
        return nullptr;
    if (QScriptClassObjectData *data = classDataForValue(privateValue->state.data(), privateValue->value))
        return data->scriptClass;
    return nullptr;
}

void qScriptQuickJSSetScriptClass(QScriptValue &value, QScriptClass *scriptClass)
{
    QScriptValuePrivate *privateValue = QScriptValuePrivate::get(value);
    if (!privateValue || privateValue->kind != QScriptValuePrivate::QuickJSValue
        || !privateValue->state || !privateValue->state->context
        || !JS_IsObject(privateValue->value))
        return;
    QScriptEngineState *state = privateValue->state.data();
    if (QScriptClassObjectData *data = classData(state, privateValue->value)) {
        data->scriptClass = scriptClass;
        if (scriptClass)
            installClassExtensions(state, privateValue->value, scriptClass);
        return;
    }
    if (value.isArray() || value.isDate() || value.isRegExp() || value.isFunction()
        || (!value.isQObject() && !value.isVariant()
            && !isCppCreatedObject(state, privateValue->value))) {
        qWarning("QScriptValue::setScriptClass() failed: cannot change class of non-QScriptObject");
        return;
    }
    QScriptClassObjectData *binding = bindingData(state, privateValue->value);
    if (!scriptClass && binding) {
        JS_SetPrototype(state->context, privateValue->value, binding->originalPrototype);
        JSAtom marker = JS_NewAtom(state->context, classBindingProperty);
        if (marker != JS_ATOM_NULL) {
            JS_DeleteProperty(state->context, privateValue->value, marker, 0);
            JS_FreeAtom(state->context, marker);
        }
        return;
    }
    if (!binding) {
        JSValue original = JS_GetPrototype(state->context, privateValue->value);
        JSValue bridge = JS_NewObjectClass(state->context, state->scriptClassClassId);
        binding = new QScriptClassObjectData{state, scriptClass, original,
                                             JS_DupValue(state->context,
                                                         privateValue->value), true};
        JS_SetOpaque(bridge, binding);
        state->scriptClassObjects.append(binding);
        const QScriptValue prototype = scriptClass ? scriptClass->prototype() : QScriptValue();
        bool ok = false;
        JSValue prototypeValue = QScriptEnginePrivate::get(state->engine)->toQuickJS(prototype, &ok);
        const bool hasClassPrototype = ok && JS_IsObject(prototypeValue);
        if (hasClassPrototype)
            JS_SetPrototype(state->context, bridge, prototypeValue);
        JS_FreeValue(state->context, prototypeValue);
        if (!hasClassPrototype)
            JS_SetPrototype(state->context, bridge, original);
        JS_SetPrototype(state->context, privateValue->value, bridge);
        JSAtom marker = JS_NewAtom(state->context, classBindingProperty);
        if (marker != JS_ATOM_NULL) {
            JS_DefinePropertyValue(state->context, privateValue->value, marker,
                                   JS_DupValue(state->context, bridge),
                                    JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            JS_FreeAtom(state->context, marker);
        }
        installClassExtensions(state, bridge, scriptClass);
        JS_FreeValue(state->context, bridge);
    } else {
        binding->scriptClass = scriptClass;
        JSValue bridge = JS_GetPropertyStr(state->context, privateValue->value,
                                           classBindingProperty);
        if (JS_IsObject(bridge) && scriptClass)
            installClassExtensions(state, bridge, scriptClass);
        JS_FreeValue(state->context, bridge);
    }
}

bool qScriptQuickJSOwnPropertyShadowsClass(const QScriptValue &value,
                                           const QString &propertyName)
{
    QScriptValuePrivate *privateValue = QScriptValuePrivate::get(value);
    if (!privateValue || privateValue->kind != QScriptValuePrivate::QuickJSValue
        || !privateValue->state || !privateValue->state->context
        || !JS_IsObject(privateValue->value))
        return false;
    QScriptEngineState *state = privateValue->state.data();
    QScriptClassObjectData *data = bindingData(state, privateValue->value);
    if (!data || !data->scriptClass || !state->engine)
        return false;
    const QByteArray utf8 = propertyName.toUtf8();
    JSAtom atom = JS_NewAtomLen(state->context, utf8.constData(), size_t(utf8.size()));
    if (atom == JS_ATOM_NULL)
        return false;
    JSPropertyDescriptor descriptor{};
    const bool hasOwn = JS_GetOwnProperty(state->context, &descriptor,
                                          privateValue->value, atom) > 0;
    if (hasOwn)
        freeQuickJSDescriptor(state->context, &descriptor);
    JS_FreeAtom(state->context, atom);
    if (!hasOwn)
        return false;
    const QScriptString name = state->engine->toStringHandle(propertyName);
    uint id = 0;
    const QScriptClass::QueryFlags query = data->scriptClass->queryProperty(
        QScriptEnginePrivate::get(state->engine)->fromBorrowed(privateValue->value),
        name, QScriptClass::HandlesReadAccess | QScriptClass::HandlesWriteAccess, &id);
    return (query & QScriptClass::HandlesReadAccess)
        && (query & QScriptClass::HandlesWriteAccess);
}

QT_END_NAMESPACE
