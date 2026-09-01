/****************************************************************************
**
** QuickJS-NG implementation of QScriptValue and QScriptContext.
**
****************************************************************************/

#include "qscriptquickjs_p.h"

#include "../api/qregexp.h"

#include <QtScript/qscriptclass.h>

#include <QtCore/qdatetime.h>
#include <QtCore/qdebug.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qset.h>
#include <QtCore/qvariant.h>

#include <cmath>
#include <limits>
#include <utility>

QT_BEGIN_NAMESPACE

namespace {

constexpr char dataProperty[] = "__qt_data__";
constexpr char scopeProperty[] = "__qt_scope__";
constexpr char metaObjectProperty[] = "__qtscript_metaobject__";
constexpr char staticScopeProperty[] = "__qtscript_static_scope__";

// Must mirror the opaque payload used by qscriptqobjectquickjs.cpp.  The
// bridge keeps this private to its translation unit, so the value backend
// accesses it through this layout-compatible view.
struct QObjectPayloadView
{
    QPointer<QObject> object;
    QScriptEngine::ValueOwnership ownership = QScriptEngine::QtOwnership;
    QScriptEngineState *state = nullptr;
};

static void discardException(JSContext *context)
{
    if (context && JS_HasException(context))
        JS_FreeValue(context, JS_GetException(context));
}

static QString errorObjectString(JSContext *context, JSValueConst error)
{
    if (!context || !JS_IsObject(error))
        return QString();
    JSValue name = JS_GetPropertyStr(context, error, "name");
    QString nameText;
    if (!JS_IsException(name))
        nameText = qScriptQuickJSString(context, name);
    else
        discardException(context);
    JS_FreeValue(context, name);
    JSValue message = JS_GetPropertyStr(context, error, "message");
    QString messageText;
    if (!JS_IsException(message))
        messageText = qScriptQuickJSString(context, message);
    else
        discardException(context);
    JS_FreeValue(context, message);
    if (nameText.isEmpty())
        nameText = QStringLiteral("Error");
    return messageText.isEmpty() ? nameText : nameText + QStringLiteral(": ") + messageText;
}

static void *qobjectOpaque(QScriptEngineState *state, JSValueConst value)
{
    if (!state || !state->qobjectClassId)
        return nullptr;
    void *opaque = JS_GetOpaque(value, state->qobjectClassId);
    if (opaque || !JS_IsObject(value))
        return opaque;
    JSAtom marker = JS_NewAtom(state->context, "__qtscript_qobject__");
    if (marker == JS_ATOM_NULL)
        return nullptr;
    JSPropertyDescriptor descriptor{};
    const int result = JS_GetOwnProperty(state->context, &descriptor, value, marker);
    JS_FreeAtom(state->context, marker);
    if (result <= 0) {
        if (result < 0)
            discardException(state->context);
        return nullptr;
    }
    if (!(descriptor.flags & JS_PROP_GETSET))
        opaque = JS_GetOpaque(descriptor.value, state->qobjectClassId);
    JS_FreeValue(state->context, descriptor.value);
    JS_FreeValue(state->context, descriptor.getter);
    JS_FreeValue(state->context, descriptor.setter);
    return opaque;
}

static QScriptEngineState *engineState(const QScriptValuePrivate *value)
{
    return value && value->state ? value->state.data() : nullptr;
}

static JSContext *contextFor(const QScriptValuePrivate *value)
{
    QScriptEngineState *state = engineState(value);
    return state ? state->context : nullptr;
}

static bool isQuickValue(const QScriptValuePrivate *value)
{
    return value && value->kind == QScriptValuePrivate::QuickJSValue
        && value->state && value->state->context && !value->state->destroying
        && value->state->engine;
}

static bool isStaticScopeObject(JSContext *context, JSValueConst object)
{
    JSValue marker = JS_GetPropertyStr(context, object, staticScopeProperty);
    if (JS_IsException(marker)) {
        discardException(context);
        return false;
    }
    const bool result = JS_ToBool(context, marker);
    JS_FreeValue(context, marker);
    return result;
}

static QScriptValue invalidValue()
{
    return QScriptValue();
}

static QScriptValue undefinedValue()
{
    return QScriptValue(QScriptValue::UndefinedValue);
}

static bool sameEngine(const QScriptValuePrivate *left, const QScriptValuePrivate *right)
{
    if (!left || !right || !left->state || !right->state)
        return true;
    return left->state == right->state;
}

static bool invalidPrivateValue(const QScriptValuePrivate *value)
{
    return !value || value->kind == QScriptValuePrivate::Invalid;
}

static bool ownProperty(JSContext *context, JSValueConst object, JSAtom atom,
                        JSPropertyDescriptor *descriptor = nullptr)
{
    if (!context || !JS_IsObject(object))
        return false;

    JSPropertyDescriptor local{};
    JSPropertyDescriptor *result = descriptor ? descriptor : &local;
    const int status = JS_GetOwnProperty(context, result, object, atom);
    if (status <= 0) {
        if (status < 0 && descriptor) {
            JS_FreeValue(context, result->value);
            JS_FreeValue(context, result->getter);
            JS_FreeValue(context, result->setter);
        }
        return false;
    }

    if (!descriptor) {
        JS_FreeValue(context, result->value);
        JS_FreeValue(context, result->getter);
        JS_FreeValue(context, result->setter);
    }
    return true;
}

static bool resolvedProperty(JSContext *context, JSValueConst object, JSAtom atom,
                             QScriptValue::ResolveFlags mode,
                             JSPropertyDescriptor *descriptor = nullptr)
{
    if (!context || !JS_IsObject(object))
        return false;

    JSValue current = JS_DupValue(context, object);
    while (JS_IsObject(current)) {
        if (ownProperty(context, current, atom, descriptor)) {
            JS_FreeValue(context, current);
            return true;
        }

        if (!(mode & QScriptValue::ResolvePrototype)) {
            JS_FreeValue(context, current);
            return false;
        }

        JSValue prototype = JS_GetPrototype(context, current);
        JS_FreeValue(context, current);
        current = prototype;
        if (JS_IsNull(current) || JS_IsException(current))
            break;
    }
    JS_FreeValue(context, current);
    return false;
}

static QScriptValue::PropertyFlags propertyFlagsFromDescriptor(
    const JSPropertyDescriptor &descriptor)
{
    QScriptValue::PropertyFlags result;
    const bool isAccessor = descriptor.flags & JS_PROP_GETSET;
    if (!isAccessor && !(descriptor.flags & JS_PROP_WRITABLE))
        result |= QScriptValue::ReadOnly;
    if (!(descriptor.flags & JS_PROP_CONFIGURABLE))
        result |= QScriptValue::Undeletable;
    if (!(descriptor.flags & JS_PROP_ENUMERABLE))
        result |= QScriptValue::SkipInEnumeration;
    if (descriptor.flags & JS_PROP_GETSET) {
        if (!JS_IsUndefined(descriptor.getter))
            result |= QScriptValue::PropertyGetter;
        if (!JS_IsUndefined(descriptor.setter))
            result |= QScriptValue::PropertySetter;
    }
    return result;
}

constexpr char propertyFlagsProperty[] = "__qtscript_property_flags__";

static QScriptValue::PropertyFlags storedPropertyFlags(JSContext *context,
                                                       JSValueConst object,
                                                       const QString &name)
{
    if (!context || !JS_IsObject(object))
        return {};
    JSValue map = JS_GetPropertyStr(context, object, propertyFlagsProperty);
    if (JS_IsException(map) || !JS_IsObject(map)) {
        if (JS_IsException(map))
            discardException(context);
        JS_FreeValue(context, map);
        return {};
    }
    const QByteArray utf8 = name.toUtf8();
    JSValue value = JS_GetPropertyStr(context, map, utf8.constData());
    int32_t flags = 0;
    if (!JS_IsException(value))
        JS_ToInt32(context, &flags, value);
    else
        discardException(context);
    JS_FreeValue(context, value);
    JS_FreeValue(context, map);
    return QScriptValue::PropertyFlags(flags) & QScriptValue::UserRange;
}

static void storePropertyFlags(JSContext *context, JSValueConst object,
                               const QString &name,
                               QScriptValue::PropertyFlags flags)
{
    if (!context || !JS_IsObject(object))
        return;
    const int userFlags = int(flags & QScriptValue::UserRange);
    JSAtom marker = JS_NewAtom(context, propertyFlagsProperty);
    if (marker == JS_ATOM_NULL)
        return;
    JSPropertyDescriptor markerDescriptor{};
    const bool hasOwnMap = JS_GetOwnProperty(context, &markerDescriptor, object, marker) > 0;
    JS_FreeValue(context, markerDescriptor.value);
    JS_FreeValue(context, markerDescriptor.getter);
    JS_FreeValue(context, markerDescriptor.setter);
    if (!hasOwnMap && !userFlags) {
        JS_FreeAtom(context, marker);
        return;
    }

    JSValue map = hasOwnMap ? JS_GetProperty(context, object, marker) : JS_NewObject(context);
    if (JS_IsException(map) || !JS_IsObject(map)) {
        if (JS_IsException(map))
            discardException(context);
        JS_FreeValue(context, map);
        JS_FreeAtom(context, marker);
        return;
    }
    if (!hasOwnMap) {
        JS_DefinePropertyValue(context, object, marker, JS_DupValue(context, map),
                               JS_PROP_CONFIGURABLE);
    }
    const QByteArray utf8 = name.toUtf8();
    if (userFlags) {
        if (JS_SetPropertyStr(context, map, utf8.constData(),
                              JS_NewInt32(context, userFlags)) < 0)
            discardException(context);
    } else {
        JSAtom atom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
        if (atom != JS_ATOM_NULL) {
            if (JS_DeleteProperty(context, map, atom, 0) < 0)
                discardException(context);
            JS_FreeAtom(context, atom);
        }
    }
    JS_FreeValue(context, map);
    JS_FreeAtom(context, marker);
}

static bool materialize(const QScriptValue &value, QScriptEnginePrivate *engine,
                        JSValue *result)
{
    if (!engine || !result)
        return false;
    bool ok = false;
    *result = engine->toQuickJS(value, &ok);
    return ok;
}

static bool materializeArguments(const QScriptValueList &arguments,
                                 QScriptEnginePrivate *engine,
                                 QVector<JSValue> *result)
{
    if (!engine || !result)
        return false;
    result->clear();
    result->reserve(arguments.size());
    for (const QScriptValue &argument : arguments) {
        JSValue value = JS_UNDEFINED;
        if (!materialize(argument, engine, &value)) {
            for (const JSValue &created : std::as_const(*result))
                JS_FreeValue(engine->state->context, created);
            result->clear();
            return false;
        }
        result->append(value);
    }
    return true;
}

static void freeValues(QScriptEngineState *state, QVector<JSValue> *values)
{
    if (!state || !state->context || !values)
        return;
    for (const JSValue &value : std::as_const(*values))
        JS_FreeValue(state->context, value);
    values->clear();
}

static QScriptValue exceptionValue(QScriptEnginePrivate *engine)
{
    if (!engine || !engine->state || !engine->state->context)
        return invalidValue();
    QScriptEngineState *state = engine->state.data();
    JSValue exception = JS_GetException(state->context);
    const int existingLine = state->hasException
        && JS_IsStrictEqual(state->context, exception, state->exception)
        ? state->exceptionLine : -1;
    state->rememberException(JS_DupValue(state->context, exception), existingLine);
    return engine->fromOwned(exception);
}

static QScriptValue fromOwnedValue(QScriptValuePrivate *owner, JSValue value)
{
    if (!owner || !owner->state || !owner->state->context)
        return invalidValue();
    return QScriptValuePrivate::toPublic(new QScriptValuePrivate(owner->state, value, true));
}

static QScriptValue argumentListError(QScriptValuePrivate *owner)
{
    if (!owner || !owner->state || !owner->state->context)
        return invalidValue();
    return fromOwnedValue(owner, JS_NewTypeError(owner->state->context,
                                                  "%s", "Arguments must be an array"));
}

static double detachedNumber(const QScriptValuePrivate *value)
{
    if (!value || value->kind == QScriptValuePrivate::Invalid)
        return 0;
    switch (value->kind) {
    case QScriptValuePrivate::DetachedUndefined:
        return std::numeric_limits<double>::quiet_NaN();
    case QScriptValuePrivate::DetachedNull:
        return 0;
    case QScriptValuePrivate::DetachedBoolean:
        return value->booleanValue ? 1 : 0;
    case QScriptValuePrivate::DetachedNumber:
        return value->numberValue;
    case QScriptValuePrivate::DetachedString: {
        const QString text = value->stringValue.trimmed();
        if (text.isEmpty())
            return 0;
        if (text == QStringLiteral("Infinity"))
            return std::numeric_limits<double>::infinity();
        if (text == QStringLiteral("+Infinity"))
            return std::numeric_limits<double>::infinity();
        if (text == QStringLiteral("-Infinity"))
            return -std::numeric_limits<double>::infinity();
        bool ok = false;
        const double result = text.toDouble(&ok);
        if (ok)
            return result;
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            const quint64 integer = text.mid(2).toULongLong(&ok, 16);
            if (ok)
                return double(integer);
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    case QScriptValuePrivate::QuickJSValue:
        break;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

static QString detachedString(const QScriptValuePrivate *value)
{
    if (!value || value->kind == QScriptValuePrivate::Invalid)
        return QString();
    switch (value->kind) {
    case QScriptValuePrivate::DetachedUndefined:
        return QStringLiteral("undefined");
    case QScriptValuePrivate::DetachedNull:
        return QStringLiteral("null");
    case QScriptValuePrivate::DetachedBoolean:
        return value->booleanValue ? QStringLiteral("true") : QStringLiteral("false");
    case QScriptValuePrivate::DetachedNumber:
        if (std::isnan(value->numberValue))
            return QStringLiteral("NaN");
        if (std::isinf(value->numberValue))
            return value->numberValue < 0 ? QStringLiteral("-Infinity")
                                          : QStringLiteral("Infinity");
        return QString::number(value->numberValue, 'g', 15);
    case QScriptValuePrivate::DetachedString:
        return value->stringValue;
    case QScriptValuePrivate::QuickJSValue:
        break;
    }
    return QStringLiteral("undefined");
}

static bool detachedBoolean(const QScriptValuePrivate *value)
{
    if (!value || value->kind == QScriptValuePrivate::Invalid)
        return false;
    switch (value->kind) {
    case QScriptValuePrivate::DetachedUndefined:
    case QScriptValuePrivate::DetachedNull:
        return false;
    case QScriptValuePrivate::DetachedBoolean:
        return value->booleanValue;
    case QScriptValuePrivate::DetachedNumber:
        return value->numberValue != 0 && !std::isnan(value->numberValue);
    case QScriptValuePrivate::DetachedString:
        return !value->stringValue.isEmpty();
    case QScriptValuePrivate::QuickJSValue:
        break;
    }
    return false;
}

static qint32 detachedInt32(double number)
{
    if (!std::isfinite(number) || number == 0)
        return 0;
    const double truncated = std::trunc(number);
    double modulo = std::fmod(truncated, 4294967296.0);
    if (modulo < 0)
        modulo += 4294967296.0;
    if (modulo >= 2147483648.0)
        modulo -= 4294967296.0;
    return qint32(modulo);
}

static quint32 detachedUInt32(double number)
{
    return quint32(detachedInt32(number));
}

static bool propertyArguments(JSContext *context, JSValueConst arguments,
                              QVector<JSValue> *values)
{
    if (!context || !values || !JS_IsObject(arguments))
        return false;
    if (!JS_IsArray(arguments) && !JS_IsArgumentsObject(arguments))
        return false;
    int64_t length = 0;
    if (JS_GetLength(context, arguments, &length) < 0) {
        discardException(context);
        return false;
    }
    length = qMax<int64_t>(0, qMin<int64_t>(length, std::numeric_limits<int>::max()));
    values->clear();
    values->reserve(int(length));
    for (int64_t index = 0; index < length; ++index) {
        JSValue value = JS_GetPropertyUint32(context, arguments, uint32_t(index));
        if (JS_IsException(value)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
            freeValues(static_cast<QScriptEngineState *>(JS_GetContextOpaque(context)), values);
            return false;
        }
        values->append(value);
    }
    return true;
}

static QVariant variantFromObjectImpl(const QScriptValue &value,
                                      QSet<const void *> *visited)
{
    QScriptValuePrivate *privateValue = QScriptValuePrivate::get(value);
    if (!isQuickValue(privateValue))
        return QVariant();
    QScriptEngineState *state = privateValue->state.data();
    JSContext *context = state->context;
    const JSValueConst object = privateValue->value;
    QScriptEnginePrivate *engine = state->engine ? QScriptEnginePrivate::get(state->engine)
                                                  : nullptr;

    if (!JS_IsObject(object))
        return value.toVariant();

    const void *identity = JS_VALUE_GET_PTR(object);
    if (identity && visited->contains(identity))
        return JS_IsArray(object) ? QVariant(QVariantList()) : QVariant(QVariantMap());
    if (identity)
        visited->insert(identity);

    if (!qScriptQuickJSScriptClass(value)) {
        if (const QScriptVariantPayload *payload = qscriptVariantPayload(state, object)) {
            const QVariant result = payload->value;
            if (identity)
                visited->remove(identity);
            return result;
        }
    }

    if (!engine) {
        if (identity)
            visited->remove(identity);
        return QVariant();
    }

    if (JS_IsArray(object)) {
        int64_t length = 0;
        if (JS_GetLength(context, object, &length) < 0) {
            discardException(context);
            if (identity)
                visited->remove(identity);
            return QVariant();
        }
        QVariantList list;
        list.reserve(int(qMin<int64_t>(length, std::numeric_limits<int>::max())));
        for (int64_t index = 0; index < length; ++index) {
            JSValue element = JS_GetPropertyUint32(context, object, uint32_t(index));
            if (JS_IsException(element)) {
                discardException(context);
                if (identity)
                    visited->remove(identity);
                return QVariant();
            }
            QScriptValue elementValue = engine->fromOwned(element);
            list.append(variantFromObjectImpl(elementValue, visited));
        }
        if (identity)
            visited->remove(identity);
        return list;
    }

    QVariantMap map;
    JSPropertyEnum *properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &count, object,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        discardException(context);
        if (identity)
            visited->remove(identity);
        return QVariant();
    }
    for (uint32_t index = 0; index < count; ++index) {
        const char *name = JS_AtomToCString(context, properties[index].atom);
        JSValue property = JS_GetProperty(context, object, properties[index].atom);
        if (name && !JS_IsException(property)) {
            QScriptValue propertyValue = engine->fromOwned(property);
            map.insert(QString::fromUtf8(name), variantFromObjectImpl(propertyValue, visited));
        } else {
            JS_FreeValue(context, property);
        }
        if (name)
            JS_FreeCString(context, name);
    }
    JS_FreePropertyEnum(context, properties, count);
    if (identity)
        visited->remove(identity);
    return map;
}

static QVariant variantFromObject(const QScriptValue &value)
{
    QSet<const void *> visited;
    return variantFromObjectImpl(value, &visited);
}

static const QMetaObject *metaObjectFromValue(const QScriptValue &value)
{
    const QScriptValue marker = value.property(QString::fromLatin1(metaObjectProperty));
    if (!marker.isVariant())
        return nullptr;
    return qvariant_cast<const QMetaObject *>(marker.toVariant());
}

static QScriptValue callWithArguments(QScriptValuePrivate *privateValue,
                                      const QScriptValue &thisObject,
                                      QVector<JSValue> &arguments,
                                      bool construct)
{
    if (!isQuickValue(privateValue))
        return invalidValue();
    QScriptEngineState *state = privateValue->state.data();
    QScriptEnginePrivate *engine = state->engine
        ? QScriptEnginePrivate::get(state->engine) : nullptr;
    if (!engine)
        return invalidValue();

    JSContext *context = state->context;
    JSValue function = JS_DupValue(context, privateValue->value);
    const QScriptValue publicFunction = QScriptValuePrivate::toPublic(privateValue);
    QScriptClass *scriptClass = qScriptQuickJSScriptClass(publicFunction);
    const bool isCallableScriptClass = scriptClass
        && scriptClass->supportsExtension(QScriptClass::Callable);
    if ((scriptClass && !isCallableScriptClass)
        || (!JS_IsFunction(context, function) && !isCallableScriptClass)) {
        JS_FreeValue(context, function);
        return invalidValue();
    }

    QScriptContext *callContext = QScriptContextPrivate::create();
    QScriptContextPrivate *callContextPrivate = QScriptContextPrivate::get(callContext);
    callContextPrivate->engine = state->engine;
    callContextPrivate->parent = state->currentContext;
    callContextPrivate->callee = publicFunction;
    callContextPrivate->thisObject = thisObject.isValid() ? thisObject : state->engine->globalObject();
    callContextPrivate->activationObject = state->engine->newObject();
    callContextPrivate->scopes.append(callContextPrivate->activationObject);
    callContextPrivate->scopes.append(state->engine->globalObject());
    callContextPrivate->calledAsConstructor = construct;
    callContextPrivate->hiddenFromBacktrace = true;
    for (const JSValue &argument : arguments)
        callContextPrivate->arguments.append(engine->fromBorrowed(argument));

    QScriptContext *previousContext = state->currentContext;
    state->currentContext = callContext;
    if (state->agent)
        state->agent->contextPush();

    state->lastNativeReturnInvalid = false;
    JSValue result = JS_UNDEFINED;
    if (construct) {
        result = JS_CallConstructor(context, function, arguments.size(), arguments.data());
    } else {
        JSValue receiver = JS_UNDEFINED;
        bool receiverOwned = false;
        if (thisObject.isValid()) {
            if (!materialize(thisObject, engine, &receiver)) {
                if (state->agent)
                    state->agent->contextPop();
                state->currentContext = previousContext;
                delete callContext;
                JS_FreeValue(context, function);
                return invalidValue();
            }
            receiverOwned = true;
        }
        result = JS_Call(context, function, receiver, arguments.size(), arguments.data());
        if (receiverOwned)
            JS_FreeValue(context, receiver);
    }
    if (state->agent)
        state->agent->contextPop();
    state->currentContext = previousContext;
    delete callContext;
    JS_FreeValue(context, function);

    if (JS_IsException(result)) {
        state->lastNativeReturnInvalid = false;
        QScriptValue exception = exceptionValue(engine);
        if (previousContext) {
            QScriptContextPrivate *previousPrivate = QScriptContextPrivate::get(previousContext);
            if (previousPrivate && previousPrivate->callee.isValid()) {
                previousPrivate->state = QScriptContext::ExceptionState;
                previousPrivate->thrownValue = exception;
            }
        }
        return exception;
    }
    if (state->lastNativeReturnInvalid) {
        state->lastNativeReturnInvalid = false;
        JS_FreeValue(context, result);
        return invalidValue();
    }
    return engine->fromOwned(result);
}

static bool valuesForComparison(const QScriptValue &left, const QScriptValue &right,
                                QScriptEnginePrivate **engine, JSValue *leftValue,
                                JSValue *rightValue)
{
    QScriptValuePrivate *leftPrivate = QScriptValuePrivate::get(left);
    QScriptValuePrivate *rightPrivate = QScriptValuePrivate::get(right);
    if (invalidPrivateValue(leftPrivate) != invalidPrivateValue(rightPrivate))
        return false;
    if (!sameEngine(leftPrivate, rightPrivate))
        return false;

    QScriptEngineState *state = leftPrivate && leftPrivate->state
        ? leftPrivate->state.data()
        : (rightPrivate && rightPrivate->state ? rightPrivate->state.data() : nullptr);
    if (!state || !state->engine)
        return false;
    QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(state->engine);
    if (!privateEngine)
        return false;

    bool leftOk = false;
    bool rightOk = false;
    *leftValue = privateEngine->toQuickJS(left, &leftOk);
    *rightValue = privateEngine->toQuickJS(right, &rightOk);
    if (!leftOk || !rightOk) {
        JS_FreeValue(state->context, *leftValue);
        JS_FreeValue(state->context, *rightValue);
        return false;
    }
    *engine = privateEngine;
    return true;
}

static QString contextFunctionName(const QScriptContextPrivate *privateContext)
{
    if (!privateContext || !privateContext->callee.isValid())
        return QStringLiteral("<native>");
    const QString name = privateContext->callee.property(QStringLiteral("name"),
                                                          QScriptValue::ResolveLocal).toString();
    return name.isEmpty() ? QStringLiteral("<native>") : name;
}

static bool detachedValues(const QScriptValuePrivate *left,
                           const QScriptValuePrivate *right)
{
    return (!left || !left->state) && (!right || !right->state);
}

static bool detachedStrictEquals(const QScriptValuePrivate *left,
                                 const QScriptValuePrivate *right)
{
    const QScriptValuePrivate::Kind leftKind =
        left ? left->kind : QScriptValuePrivate::DetachedUndefined;
    const QScriptValuePrivate::Kind rightKind =
        right ? right->kind : QScriptValuePrivate::DetachedUndefined;
    if ((leftKind == QScriptValuePrivate::Invalid
         || leftKind == QScriptValuePrivate::DetachedUndefined)
        && (rightKind == QScriptValuePrivate::Invalid
            || rightKind == QScriptValuePrivate::DetachedUndefined))
        return true;
    if (leftKind != rightKind)
        return false;
    switch (leftKind) {
    case QScriptValuePrivate::DetachedNumber:
        return left->numberValue == right->numberValue
            && !std::isnan(left->numberValue);
    case QScriptValuePrivate::DetachedString:
        return left->stringValue == right->stringValue;
    case QScriptValuePrivate::DetachedBoolean:
        return left->booleanValue == right->booleanValue;
    case QScriptValuePrivate::DetachedNull:
        return true;
    case QScriptValuePrivate::Invalid:
    case QScriptValuePrivate::DetachedUndefined:
        return true;
    case QScriptValuePrivate::QuickJSValue:
        return false;
    }
    return false;
}

static bool detachedEquals(const QScriptValuePrivate *left,
                           const QScriptValuePrivate *right)
{
    if (detachedStrictEquals(left, right))
        return true;
    const bool leftNullish = !left || left->kind == QScriptValuePrivate::Invalid
        || left->kind == QScriptValuePrivate::DetachedUndefined
        || left->kind == QScriptValuePrivate::DetachedNull;
    const bool rightNullish = !right || right->kind == QScriptValuePrivate::Invalid
        || right->kind == QScriptValuePrivate::DetachedUndefined
        || right->kind == QScriptValuePrivate::DetachedNull;
    if (leftNullish && rightNullish)
        return true;
    if (left && right && left->kind == QScriptValuePrivate::DetachedBoolean)
        return detachedNumber(left) == detachedNumber(right);
    if (left && right && right->kind == QScriptValuePrivate::DetachedBoolean)
        return detachedNumber(left) == detachedNumber(right);
    if (left && right && left->kind == QScriptValuePrivate::DetachedString
        && right->kind == QScriptValuePrivate::DetachedNumber)
        return detachedNumber(left) == right->numberValue;
    if (left && right && right->kind == QScriptValuePrivate::DetachedString
        && left->kind == QScriptValuePrivate::DetachedNumber)
        return left->numberValue == detachedNumber(right);
    return false;
}

} // unnamed namespace

QScriptVariantPayload *qscriptVariantPayload(QScriptEngineState *state,
                                              JSValueConst value)
{
    if (!state || !state->context || !state->variantClassId || !JS_IsObject(value))
        return nullptr;

    if (JS_GetClassID(value) == state->variantClassId)
        return static_cast<QScriptVariantPayload *>(
            JS_GetOpaque(value, state->variantClassId));

    // QScriptEngine::newVariant(object, value) stores the native wrapper in
    // an own, non-enumerable marker.  Reading the descriptor is deliberately
    // different from JS_GetPropertyStr: it cannot invoke a user getter or
    // traverse the generated QObject/QScriptClass prototype bridge while a
    // native overload is already converting an argument.
    JSAtom marker = JS_NewAtom(state->context, "__qtscript_variant__");
    if (marker == JS_ATOM_NULL)
        return nullptr;
    JSPropertyDescriptor descriptor{};
    const int result = JS_GetOwnProperty(state->context, &descriptor, value, marker);
    JS_FreeAtom(state->context, marker);
    if (result < 0) {
        JS_FreeValue(state->context, descriptor.value);
        JS_FreeValue(state->context, descriptor.getter);
        JS_FreeValue(state->context, descriptor.setter);
        discardException(state->context);
        return nullptr;
    }
    if (result == 0)
        return nullptr;

    QScriptVariantPayload *payload = nullptr;
    if (!(descriptor.flags & JS_PROP_GETSET)
        && JS_IsObject(descriptor.value)
        && JS_GetClassID(descriptor.value) == state->variantClassId) {
        payload = static_cast<QScriptVariantPayload *>(
            JS_GetOpaque(descriptor.value, state->variantClassId));
    }
    JS_FreeValue(state->context, descriptor.value);
    JS_FreeValue(state->context, descriptor.getter);
    JS_FreeValue(state->context, descriptor.setter);
    return payload;
}

QScriptValuePrivate::QScriptValuePrivate()
    : kind(Invalid)
{
}

QScriptValuePrivate::QScriptValuePrivate(Kind valueKind)
    : kind(valueKind)
{
}

QScriptValuePrivate::QScriptValuePrivate(bool value)
    : kind(DetachedBoolean), booleanValue(value)
{
}

QScriptValuePrivate::QScriptValuePrivate(qsreal value)
    : kind(DetachedNumber), numberValue(value)
{
}

QScriptValuePrivate::QScriptValuePrivate(const QString &value)
    : kind(DetachedString), stringValue(value)
{
}

QScriptValuePrivate::QScriptValuePrivate(const QSharedPointer<QScriptEngineState> &valueState,
                                         JSValue value, bool adoptValue)
    : kind(QuickJSValue), state(valueState), value(value)
{
    if (!adoptValue && state && state->context)
        this->value = JS_DupValue(state->context, value);
}

QScriptValuePrivate::~QScriptValuePrivate()
{
    if (kind == QuickJSValue && state && state->context)
        JS_FreeValue(state->context, value);
}

JSValue QScriptValuePrivate::materialize(const QSharedPointer<QScriptEngineState> &target,
                                         bool *ok) const
{
    if (ok)
        *ok = true;
    if (!target || !target->context) {
        if (ok)
            *ok = false;
        return JS_UNDEFINED;
    }

    switch (kind) {
    case Invalid:
    case DetachedUndefined:
        return JS_UNDEFINED;
    case DetachedNull:
        return JS_NULL;
    case DetachedBoolean:
        return JS_NewBool(target->context, booleanValue);
    case DetachedNumber:
        return JS_NewFloat64(target->context, numberValue);
    case DetachedString: {
        const QByteArray utf8 = stringValue.toUtf8();
        return JS_NewStringLen(target->context, utf8.constData(), size_t(utf8.size()));
    }
    case QuickJSValue:
        if (state != target) {
            if (ok)
                *ok = false;
            return JS_UNDEFINED;
        }
        return JS_DupValue(target->context, value);
    }
    if (ok)
        *ok = false;
    return JS_UNDEFINED;
}

bool QScriptValuePrivate::belongsTo(const QSharedPointer<QScriptEngineState> &target) const
{
    return kind != QuickJSValue || state == target;
}

QScriptContext *QScriptContextPrivate::create()
{
    QScriptContext *context = new QScriptContext;
    context->d_ptr->q_ptr = context;
    return context;
}

QScriptValue::QScriptValue(QScriptValuePrivate *value)
    : d_ptr(value)
{
}

QScriptValue::QScriptValue()
    : d_ptr(nullptr)
{
}

QScriptValue::~QScriptValue() = default;

QScriptValue::QScriptValue(const QScriptValue &other) = default;

QScriptValue::QScriptValue(QScriptEngine *engine, SpecialValue value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(value == NullValue
                                            ? QScriptValuePrivate::DetachedNull
                                            : QScriptValuePrivate::DetachedUndefined);
        return;
    }
    QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine);
    if (privateEngine) {
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        value == NullValue ? JS_NULL : JS_UNDEFINED, true);
    }
}

QScriptValue::QScriptValue(QScriptEngine *engine, bool value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(value);
        return;
    }
    if (QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine))
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        JS_NewBool(privateEngine->state->context, value), true);
}

QScriptValue::QScriptValue(QScriptEngine *engine, int value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(qsreal(value));
        return;
    }
    if (QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine))
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        JS_NewInt32(privateEngine->state->context, value), true);
}

QScriptValue::QScriptValue(QScriptEngine *engine, uint value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(qsreal(value));
        return;
    }
    if (QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine))
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        JS_NewUint32(privateEngine->state->context, value), true);
}

QScriptValue::QScriptValue(QScriptEngine *engine, qsreal value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(value);
        return;
    }
    if (QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine))
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        JS_NewFloat64(privateEngine->state->context, value), true);
}

QScriptValue::QScriptValue(QScriptEngine *engine, const QString &value)
{
    if (!engine) {
        d_ptr = new QScriptValuePrivate(value);
        return;
    }
    if (QScriptEnginePrivate *privateEngine = QScriptEnginePrivate::get(engine)) {
        const QByteArray utf8 = value.toUtf8();
        d_ptr = new QScriptValuePrivate(privateEngine->state,
                                        JS_NewStringLen(privateEngine->state->context,
                                                        utf8.constData(), size_t(utf8.size())),
                                        true);
    }
}

#ifndef QT_NO_CAST_FROM_ASCII
QScriptValue::QScriptValue(QScriptEngine *engine, const char *value)
    : QScriptValue(engine, QString::fromUtf8(value ? value : ""))
{
}
#endif

QScriptValue::QScriptValue(SpecialValue value)
    : d_ptr(new QScriptValuePrivate(value == NullValue ? QScriptValuePrivate::DetachedNull
                                                       : QScriptValuePrivate::DetachedUndefined))
{
}

QScriptValue::QScriptValue(bool value)
    : d_ptr(new QScriptValuePrivate(value))
{
}

QScriptValue::QScriptValue(int value)
    : d_ptr(new QScriptValuePrivate(qsreal(value)))
{
}

QScriptValue::QScriptValue(uint value)
    : d_ptr(new QScriptValuePrivate(qsreal(value)))
{
}

QScriptValue::QScriptValue(qsreal value)
    : d_ptr(new QScriptValuePrivate(value))
{
}

QScriptValue::QScriptValue(const QString &value)
    : d_ptr(new QScriptValuePrivate(value))
{
}

QScriptValue::QScriptValue(const QLatin1String &value)
    : d_ptr(new QScriptValuePrivate(QString(value)))
{
}

#ifndef QT_NO_CAST_FROM_ASCII
QScriptValue::QScriptValue(const char *value)
    : d_ptr(new QScriptValuePrivate(QString::fromUtf8(value ? value : "")))
{
}
#endif

QScriptValue &QScriptValue::operator=(const QScriptValue &other) = default;

QScriptEngine *QScriptValue::engine() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && value->state ? value->state->engine.data() : nullptr;
}

bool QScriptValue::isValid() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind != QScriptValuePrivate::QuickJSValue
                     ? value->kind != QScriptValuePrivate::Invalid
                     : isQuickValue(value));
}

bool QScriptValue::isBool() const { return isBoolean(); }

bool QScriptValue::isBoolean() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind == QScriptValuePrivate::DetachedBoolean
                     || (isQuickValue(value) && JS_IsBool(value->value)));
}

bool QScriptValue::isNumber() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind == QScriptValuePrivate::DetachedNumber
                     || (isQuickValue(value) && JS_IsNumber(value->value)));
}

bool QScriptValue::isFunction() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return isQuickValue(value) && (JS_IsFunction(value->state->context, value->value)
                                   || JS_IsRegExp(value->value));
}

bool QScriptValue::isNull() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind == QScriptValuePrivate::DetachedNull
                     || (isQuickValue(value) && JS_IsNull(value->value)));
}

bool QScriptValue::isString() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind == QScriptValuePrivate::DetachedString
                     || (isQuickValue(value) && JS_IsString(value->value)));
}

bool QScriptValue::isUndefined() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return value && (value->kind == QScriptValuePrivate::DetachedUndefined
                     || (isQuickValue(value) && JS_IsUndefined(value->value)));
}

bool QScriptValue::isVariant() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return false;
    // A QScriptClass binding deliberately replaces the object's built-in
    // QVariant identity.  The underlying QuickJS class id remains unchanged
    // because the binding is represented by a prototype bridge.
    if (qScriptQuickJSScriptClass(*this))
        return false;
    return qscriptVariantPayload(value->state.data(), value->value) != nullptr;
}

bool QScriptValue::isQObject() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return false;
    if (qScriptQuickJSScriptClass(*this))
        return false;
    if (qobjectOpaque(value->state.data(), value->value))
        return true;

    const QVariant variant = variantFromObject(*this);
    if (!variant.isValid() || !variant.metaType().flags().testFlag(QMetaType::PointerToQObject))
        return false;
    return qvariant_cast<QObject *>(variant) != nullptr;
}

bool QScriptValue::isQMetaObject() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return isQuickValue(value) && metaObjectFromValue(*this);
}

bool QScriptValue::isObject() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return isQuickValue(value) && JS_IsObject(value->value);
}

bool QScriptValue::isDate() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return false;
    if (JS_IsDate(value->value))
        return true;
    JSContext *context = value->state->context;
    JSValue global = JS_GetGlobalObject(context);
    JSValue date = JS_GetPropertyStr(context, global, "Date");
    JSValue prototype = JS_IsException(date)
        ? JS_EXCEPTION : JS_GetPropertyStr(context, date, "prototype");
    const bool result = !JS_IsException(prototype)
        && JS_IsStrictEqual(context, value->value, prototype);
    JS_FreeValue(context, prototype);
    JS_FreeValue(context, date);
    JS_FreeValue(context, global);
    if (JS_HasException(context))
        discardException(context);
    return result;
}

bool QScriptValue::isRegExp() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return isQuickValue(value) && JS_IsRegExp(value->value);
}

bool QScriptValue::isArray() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    return isQuickValue(value) && JS_IsArray(value->value);
}

bool QScriptValue::isError() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return false;
    if (JS_IsError(value->value))
        return true;
    if (!JS_IsObject(value->value))
        return false;

    static constexpr const char *errorConstructors[] = {
        "Error", "EvalError", "RangeError", "ReferenceError",
        "SyntaxError", "TypeError", "URIError"
    };
    JSContext *context = value->state->context;
    JSValue global = JS_GetGlobalObject(context);
    bool result = false;
    for (const char *name : errorConstructors) {
        JSValue constructor = JS_GetPropertyStr(context, global, name);
        JSValue prototype = JS_IsException(constructor)
            ? JS_EXCEPTION : JS_GetPropertyStr(context, constructor, "prototype");
        if (!JS_IsException(prototype)
            && JS_IsStrictEqual(context, value->value, prototype)) {
            result = true;
            JS_FreeValue(context, prototype);
            JS_FreeValue(context, constructor);
            break;
        }
        JS_FreeValue(context, prototype);
        JS_FreeValue(context, constructor);
    }
    JS_FreeValue(context, global);
    return result;
}

QString QScriptValue::toString() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!value || value->kind == QScriptValuePrivate::Invalid)
        return QString();
    if (value->kind != QScriptValuePrivate::QuickJSValue)
        return detachedString(value);
    if (!isQuickValue(value))
        return QString();
    JSContext *context = value->state->context;
    const QString result = qScriptQuickJSString(context, value->value);
    if (JS_HasException(context)) {
        // QtScript returns the text of a failed object-to-string conversion
        // and records the thrown value as the engine's uncaught exception.
        JSValue exception = JS_GetException(context);
        QString exceptionText = JS_IsError(exception)
            ? errorObjectString(context, exception)
            : qScriptQuickJSString(context, exception);
        discardException(context);
        value->state->rememberException(exception);
        return exceptionText;
    }
    return result;
}

qsreal QScriptValue::toNumber() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!value || value->kind != QScriptValuePrivate::QuickJSValue)
        return detachedNumber(value);
    if (!isQuickValue(value))
        return std::numeric_limits<double>::quiet_NaN();
    double result = std::numeric_limits<double>::quiet_NaN();
    if (JS_ToFloat64(value->state->context, &result, value->value) < 0) {
        discardException(value->state->context);
        return std::numeric_limits<double>::quiet_NaN();
    }
    return result;
}

bool QScriptValue::toBool() const { return toBoolean(); }

bool QScriptValue::toBoolean() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!value || value->kind != QScriptValuePrivate::QuickJSValue)
        return detachedBoolean(value);
    if (!isQuickValue(value))
        return false;
    const int result = JS_ToBool(value->state->context, value->value);
    if (result < 0)
        discardException(value->state->context);
    return result > 0;
}

qsreal QScriptValue::toInteger() const
{
    const double number = toNumber();
    if (std::isnan(number) || number == 0)
        return 0;
    if (!std::isfinite(number))
        return number;
    return std::trunc(number);
}

qint32 QScriptValue::toInt32() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (isQuickValue(value)) {
        int32_t result = 0;
        if (JS_ToInt32(value->state->context, &result, value->value) == 0)
            return result;
        discardException(value->state->context);
        return 0;
    }
    return detachedInt32(detachedNumber(value));
}

quint32 QScriptValue::toUInt32() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (isQuickValue(value)) {
        uint32_t result = 0;
        if (JS_ToUint32(value->state->context, &result, value->value) == 0)
            return result;
        discardException(value->state->context);
        return 0;
    }
    return detachedUInt32(detachedNumber(value));
}

quint16 QScriptValue::toUInt16() const
{
    return quint16(toUInt32());
}

QVariant QScriptValue::toVariant() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!value || value->kind == QScriptValuePrivate::Invalid
        || value->kind == QScriptValuePrivate::DetachedUndefined
        || value->kind == QScriptValuePrivate::DetachedNull)
        return QVariant();
    if (value->kind == QScriptValuePrivate::DetachedBoolean)
        return QVariant(value->booleanValue);
    if (value->kind == QScriptValuePrivate::DetachedNumber)
        return QVariant(value->numberValue);
    if (value->kind == QScriptValuePrivate::DetachedString)
        return QVariant(value->stringValue);
    if (!isQuickValue(value))
        return QVariant();
    // Value types registered by QtScript are represented by a dedicated
    // QuickJS class.  Extract that payload before probing QObject/object
    // conversions: the latter can walk the value's generated prototype and
    // re-enter toVariant(), which is both unnecessary and can deadlock while
    // a generated overload is checking the value's QMetaType (QSqlDatabase
    // and QSqlQuery are prominent examples).
    if (JS_IsObject(value->value) && !qScriptQuickJSScriptClass(*this)) {
        if (const QScriptVariantPayload *payload =
                qscriptVariantPayload(value->state.data(), value->value)) {
            return payload->value;
        }
    }

    if (isBool())
        return QVariant(toBoolean());
    if (isNumber()) {
        QScriptValuePrivate *numberValue = QScriptValuePrivate::get(*this);
        if (JS_VALUE_GET_TAG(numberValue->value) == JS_TAG_INT)
            return QVariant(toInt32());
        const double number = toNumber();
        if (std::isfinite(number) && std::trunc(number) == number
            && number >= std::numeric_limits<qint32>::min()
            && number <= std::numeric_limits<qint32>::max())
            return QVariant(qint32(number));
        return QVariant(number);
    }
    if (isString())
        return QVariant(toString());
    if (isNull() || isUndefined())
        return QVariant();
    if (isQObject())
        return QVariant::fromValue(toQObject());
    if (isQMetaObject())
        return QVariant::fromValue(toQMetaObject());
    if (isDate())
        return QVariant(toDateTime());
    if (isRegExp())
        return QVariant::fromValue(toRegExp());
    return variantFromObject(*this);
}

QObject *QScriptValue::toQObject() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return nullptr;
    if (qScriptQuickJSScriptClass(*this))
        return nullptr;
    const auto *payload = static_cast<const QObjectPayloadView *>(
        qobjectOpaque(value->state.data(), value->value));
    if (payload)
        return payload->object.data();

    const QVariant variant = variantFromObject(*this);
    if (variant.isValid() && variant.metaType().flags().testFlag(QMetaType::PointerToQObject))
        return qvariant_cast<QObject *>(variant);
    return nullptr;
}

const QMetaObject *QScriptValue::toQMetaObject() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return nullptr;
    return metaObjectFromValue(*this);
}

QScriptValue QScriptValue::toObject() const
{
    if (isObject())
        return *this;
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || isNull() || isUndefined())
        return invalidValue();
    JSValue object = JS_ToObject(value->state->context, value->value);
    if (JS_IsException(object))
        return exceptionValue(QScriptEnginePrivate::get(value->state->engine));
    return fromOwnedValue(value, object);
}

QDateTime QScriptValue::toDateTime() const
{
    if (!isDate())
        return QDateTime();
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    JSContext *context = value->state->context;
    JSValue getter = JS_GetPropertyStr(context, value->value, "getTime");
    if (JS_IsException(getter)) {
        discardException(context);
        return QDateTime();
    }
    JSValue milliseconds = JS_Call(context, getter, value->value, 0, nullptr);
    JS_FreeValue(context, getter);
    if (JS_IsException(milliseconds)) {
        discardException(context);
        return QDateTime();
    }
    double timestamp = 0;
    const bool ok = JS_ToFloat64(context, &timestamp, milliseconds) == 0;
    JS_FreeValue(context, milliseconds);
    if (!ok || !std::isfinite(timestamp))
        return QDateTime();
    return QDateTime::fromMSecsSinceEpoch(qint64(timestamp), Qt::LocalTime);
}

QRegExp QScriptValue::toRegExp() const
{
    if (!isRegExp())
        return QRegExp();
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    JSContext *context = value->state->context;
    JSValue pattern = JS_GetPropertyStr(context, value->value, "source");
    JSValue flags = JS_GetPropertyStr(context, value->value, "flags");
    const QString patternString = JS_IsException(pattern) ? QString()
                                                            : qScriptQuickJSString(context, pattern);
    const QString flagsString = JS_IsException(flags) ? QString()
                                                        : qScriptQuickJSString(context, flags);
    JS_FreeValue(context, pattern);
    JS_FreeValue(context, flags);
    discardException(context);
    QRegExp result(patternString,
                   flagsString.contains(QLatin1Char('i')) ? Qt::CaseInsensitive
                                                          : Qt::CaseSensitive,
                   QRegExp::RegExp2);
    result.setMinimal(flagsString.contains(QLatin1Char('U')));
    return result;
}

bool QScriptValue::instanceOf(const QScriptValue &other) const
{
    QScriptValuePrivate *left = QScriptValuePrivate::get(*this);
    QScriptValuePrivate *right = QScriptValuePrivate::get(other);
    if (!sameEngine(left, right)) {
        qWarning("QScriptValue::instanceof: cannot perform operation on a value created in a different engine");
        return false;
    }
    if (invalidPrivateValue(left) || invalidPrivateValue(right)
        || !isQuickValue(left) || !isQuickValue(right))
        return false;
    const int result = JS_IsInstanceOf(left->state->engine ? left->state->context : nullptr,
                                       left->value, right->value);
    return result > 0;
}

bool QScriptValue::lessThan(const QScriptValue &other) const
{
    QScriptValuePrivate *leftPrivate = QScriptValuePrivate::get(*this);
    QScriptValuePrivate *rightPrivate = QScriptValuePrivate::get(other);
    if (detachedValues(leftPrivate, rightPrivate)) {
        if (leftPrivate && rightPrivate
            && leftPrivate->kind == QScriptValuePrivate::DetachedString
            && rightPrivate->kind == QScriptValuePrivate::DetachedString)
            return leftPrivate->stringValue < rightPrivate->stringValue;
        const double left = detachedNumber(leftPrivate);
        const double right = detachedNumber(rightPrivate);
        return !std::isnan(left) && !std::isnan(right) && left < right;
    }
    if (invalidPrivateValue(leftPrivate) || invalidPrivateValue(rightPrivate))
        return false;
    if (!sameEngine(leftPrivate, rightPrivate)) {
        qWarning("QScriptValue::lessThan: cannot compare to a value created in a different engine");
        return false;
    }
    QScriptEnginePrivate *engine = nullptr;
    JSValue left = JS_UNDEFINED;
    JSValue right = JS_UNDEFINED;
    if (!valuesForComparison(*this, other, &engine, &left, &right))
        return false;
    JSContext *context = engine->state->context;
    static const char source[] = "(function(a,b){return a < b;})";
    JSValue function = JS_Eval(context, source, sizeof(source) - 1,
                               "<qscript-value-compare>", JS_EVAL_TYPE_GLOBAL);
    JSValueConst arguments[] = { left, right };
    JSValue result = JS_IsException(function)
        ? JS_EXCEPTION : JS_Call(context, function, JS_UNDEFINED, 2, arguments);
    if (!JS_IsException(function))
        JS_FreeValue(context, function);
    JS_FreeValue(context, left);
    JS_FreeValue(context, right);
    if (JS_IsException(result)) {
        JS_GetException(context);
        return false;
    }
    const bool less = JS_ToBool(context, result) > 0;
    JS_FreeValue(context, result);
    return less;
}

bool QScriptValue::equals(const QScriptValue &other) const
{
    QScriptValuePrivate *leftPrivate = QScriptValuePrivate::get(*this);
    QScriptValuePrivate *rightPrivate = QScriptValuePrivate::get(other);
    if (detachedValues(leftPrivate, rightPrivate))
        return detachedEquals(leftPrivate, rightPrivate);
    if (invalidPrivateValue(leftPrivate) || invalidPrivateValue(rightPrivate))
        return false;
    if (!sameEngine(leftPrivate, rightPrivate)) {
        qWarning("QScriptValue::equals: cannot compare to a value created in a different engine");
        return false;
    }
    if (leftPrivate->state && leftPrivate->state->customGlobalObject) {
        const bool leftRuntime = JS_IsStrictEqual(leftPrivate->state->context,
                                                  leftPrivate->value,
                                                  leftPrivate->state->runtimeGlobal);
        const bool rightRuntime = JS_IsStrictEqual(leftPrivate->state->context,
                                                   rightPrivate->value,
                                                   leftPrivate->state->runtimeGlobal);
        const bool leftLogical = JS_IsStrictEqual(leftPrivate->state->context,
                                                  leftPrivate->value,
                                                  leftPrivate->state->logicalGlobal);
        const bool rightLogical = JS_IsStrictEqual(leftPrivate->state->context,
                                                   rightPrivate->value,
                                                   leftPrivate->state->logicalGlobal);
        if ((leftRuntime && rightLogical) || (rightRuntime && leftLogical))
            return true;
    }
    if (isQObject() && other.isQObject())
        return toQObject() == other.toQObject();
    const bool leftVariant = isVariant();
    const bool rightVariant = other.isVariant();
    if (leftVariant || rightVariant) {
        if (leftVariant && rightVariant)
            return toVariant() == other.toVariant();
        const QScriptValue &variantValue = leftVariant ? *this : other;
        const QScriptValue &otherValue = leftVariant ? other : *this;
        QScriptValuePrivate *variantPrivate = QScriptValuePrivate::get(variantValue);
        if (!variantPrivate || !variantPrivate->state || !variantPrivate->state->engine)
            return false;
        QScriptEnginePrivate *engine =
            QScriptEnginePrivate::get(variantPrivate->state->engine.data());
        if (!engine)
            return false;
        return engine->fromVariant(variantValue.toVariant()).equals(otherValue);
    }
    QScriptEnginePrivate *engine = nullptr;
    JSValue left = JS_UNDEFINED;
    JSValue right = JS_UNDEFINED;
    if (!valuesForComparison(*this, other, &engine, &left, &right))
        return false;
    const int equal = JS_IsEqual(engine->state->context, left, right);
    JS_FreeValue(engine->state->context, left);
    JS_FreeValue(engine->state->context, right);
    return equal > 0;
}

bool QScriptValue::strictlyEquals(const QScriptValue &other) const
{
    QScriptValuePrivate *leftPrivate = QScriptValuePrivate::get(*this);
    QScriptValuePrivate *rightPrivate = QScriptValuePrivate::get(other);
    if (detachedValues(leftPrivate, rightPrivate))
        return detachedStrictEquals(leftPrivate, rightPrivate);
    if (invalidPrivateValue(leftPrivate) || invalidPrivateValue(rightPrivate))
        return false;
    if (!sameEngine(leftPrivate, rightPrivate)) {
        qWarning("QScriptValue::strictlyEquals: cannot compare to a value created in a different engine");
        return false;
    }
    QScriptEnginePrivate *engine = nullptr;
    JSValue left = JS_UNDEFINED;
    JSValue right = JS_UNDEFINED;
    if (!valuesForComparison(*this, other, &engine, &left, &right))
        return false;
    const bool equal = JS_IsStrictEqual(engine->state->context, left, right);
    JS_FreeValue(engine->state->context, left);
    JS_FreeValue(engine->state->context, right);
    if (equal)
        return true;

    if (leftPrivate->state && leftPrivate->state->customGlobalObject) {
        const bool leftRuntime = JS_IsStrictEqual(engine->state->context,
                                                  leftPrivate->value,
                                                  leftPrivate->state->runtimeGlobal);
        const bool rightRuntime = JS_IsStrictEqual(engine->state->context,
                                                   rightPrivate->value,
                                                   leftPrivate->state->runtimeGlobal);
        const bool leftLogical = JS_IsStrictEqual(engine->state->context,
                                                  leftPrivate->value,
                                                  leftPrivate->state->logicalGlobal);
        const bool rightLogical = JS_IsStrictEqual(engine->state->context,
                                                   rightPrivate->value,
                                                   leftPrivate->state->logicalGlobal);
        if ((leftRuntime && rightLogical) || (rightRuntime && leftLogical))
            return true;
    }

    // Strict equality is identity-based for objects, including QObject and
    // QVariant wrappers. Do not inspect host-object payloads here: probing a
    // marker property can invoke user accessors during native callback setup.
    return false;
}

QScriptValue QScriptValue::prototype() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return invalidValue();
    JSValue prototype = JS_GetPrototype(value->state->context, value->value);
    if (JS_IsException(prototype)) {
        discardException(value->state->context);
        return invalidValue();
    }
    if (JS_IsNull(prototype)) {
        JS_FreeValue(value->state->context, prototype);
        return QScriptValue(value->state->engine.data(), QScriptValue::NullValue);
    }
    return fromOwnedValue(value, prototype);
}

static void synchronizeCustomGlobalPrototype(QScriptValuePrivate *value,
                                             JSValueConst prototype)
{
    if (!value || !value->state || !value->state->customGlobalObject
        || !JS_IsStrictEqual(value->state->context, value->value,
                             value->state->logicalGlobal))
        return;
    JSContext *context = value->state->context;
    const bool prototypeIsRuntimeGlobal = JS_IsStrictEqual(
        context, prototype, value->state->runtimeGlobal);
    const JSValueConst runtimePrototype = prototypeIsRuntimeGlobal
        ? value->state->globalBuiltins : prototype;
    if (JS_SetPrototype(context, value->state->runtimeGlobal, runtimePrototype) < 0) {
        discardException(context);
        return;
    }

    static constexpr const char *const qtScriptGlobals[] = {
        "print", "gc", "version"
    };
    for (const char *name : qtScriptGlobals) {
        JSAtom atom = JS_NewAtom(context, name);
        if (atom == JS_ATOM_NULL)
            continue;
        if (prototypeIsRuntimeGlobal) {
            JSValue builtin = JS_GetProperty(context, value->state->globalBuiltins, atom);
            if (!JS_IsException(builtin))
                JS_DefinePropertyValue(context, value->state->runtimeGlobal, atom,
                                       builtin, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            else
                discardException(context);
        } else {
            JS_DeleteProperty(context, value->state->runtimeGlobal, atom, 0);
            discardException(context);
        }
        JS_FreeAtom(context, atom);
    }
}

void QScriptValue::setPrototype(const QScriptValue &prototypeValue)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return;
    if (!prototypeValue.isValid())
        return;
    if (prototypeValue.engine() && prototypeValue.engine() != engine()) {
        qWarning("QScriptValue::setPrototype() failed: cannot set a prototype created in a different engine");
        return;
    }
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(value->state->engine);
    JSValue prototype = JS_NULL;
    bool owned = false;
    if (!prototypeValue.isNull()) {
        if (!materialize(prototypeValue, engine, &prototype))
            return;
        if (!JS_IsObject(prototype)) {
            JS_FreeValue(value->state->context, prototype);
            return;
        }
        owned = true;
    }
    if (JS_SetPrototype(value->state->context, value->value, prototype) < 0) {
        discardException(value->state->context);
        qWarning("QScriptValue::setPrototype() failed: cyclic prototype value");
    } else {
        if (value->state->customGlobalObject
            && JS_IsStrictEqual(value->state->context, value->value,
                                value->state->runtimeGlobal)) {
            static constexpr const char *const qtScriptGlobals[] = {
                "print", "gc", "version", "hasOwnProperty"
            };
            for (const char *name : qtScriptGlobals) {
                JSAtom atom = JS_NewAtom(value->state->context, name);
                if (atom == JS_ATOM_NULL)
                    continue;
                JSValue builtin = JS_GetProperty(value->state->context,
                                                 value->state->globalBuiltins, atom);
                if (!JS_IsException(builtin))
                    JS_DefinePropertyValue(value->state->context,
                                           value->state->runtimeGlobal, atom, builtin,
                                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
                else
                    discardException(value->state->context);
                JS_FreeAtom(value->state->context, atom);
            }
        }
        synchronizeCustomGlobalPrototype(value, prototype);
    }
    if (owned)
        JS_FreeValue(value->state->context, prototype);
}

QScriptValue QScriptValue::scope() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return invalidValue();
    JSValue scope = JS_GetPropertyStr(value->state->context, value->value,
                                       scopeProperty);
    if (JS_IsException(scope)) {
        discardException(value->state->context);
        return invalidValue();
    }
    if (JS_IsUndefined(scope)) {
        JS_FreeValue(value->state->context, scope);
        return invalidValue();
    }
    return fromOwnedValue(value, scope);
}

void QScriptValue::setScope(const QScriptValue &scope)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return;
    if (scope.engine() && scope.engine() != engine()) {
        qWarning("QScriptValue::setScope() failed: cannot set a scope object created in a different engine");
        return;
    }
    JSContext *context = value->state->context;
    JSAtom atom = JS_NewAtom(context, scopeProperty);
    if (atom == JS_ATOM_NULL)
        return;
    if (!scope.isValid()) {
        JS_DeleteProperty(context, value->value, atom, 0);
        discardException(context);
    } else {
        QScriptEnginePrivate *enginePrivate =
            QScriptEnginePrivate::get(value->state->engine);
        bool ok = false;
        JSValue scopeValue = enginePrivate->toQuickJS(scope, &ok);
        if (ok) {
            if (JS_DefinePropertyValue(context, value->value, atom, scopeValue,
                                       JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
                discardException(context);
        }
    }
    JS_FreeAtom(context, atom);
}

QScriptValue QScriptValue::property(const QString &name, const ResolveFlags &mode) const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return invalidValue();
    const QByteArray utf8 = name.toUtf8();
    JSAtom atom = JS_NewAtomLen(value->state->context, utf8.constData(), size_t(utf8.size()));
    if (atom == JS_ATOM_NULL)
        return invalidValue();
    const bool exists = resolvedProperty(value->state->context, value->value, atom, mode);
    if (!exists) {
        JS_FreeAtom(value->state->context, atom);
        if (mode & ResolveScope) {
            JSValue scope = JS_GetPropertyStr(value->state->context, value->value,
                                               scopeProperty);
            if (JS_IsException(scope)) {
                discardException(value->state->context);
            } else if (JS_IsObject(scope)
                       && !JS_IsStrictEqual(value->state->context, scope, value->value)) {
                QScriptValue scopeValue = fromOwnedValue(value, scope);
                return scopeValue.property(name, mode);
            } else {
                JS_FreeValue(value->state->context, scope);
            }
        }
        return invalidValue();
    }
    JSValue result = JS_GetProperty(value->state->context, value->value, atom);
    JS_FreeAtom(value->state->context, atom);
    if (JS_IsException(result))
        return exceptionValue(QScriptEnginePrivate::get(value->state->engine));
    return fromOwnedValue(value, result);
}

void QScriptValue::setProperty(const QString &name, const QScriptValue &propertyValue,
                               const PropertyFlags &flags)
{
    QScriptValuePrivate *object = QScriptValuePrivate::get(*this);
    if (!isQuickValue(object) || !JS_IsObject(object->value))
        return;
    if (propertyValue.isValid() && !(flags & (PropertyGetter | PropertySetter))
        && qScriptQuickJSOwnPropertyShadowsClass(*this, name))
        return;
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(object->state->engine);
    JSContext *context = object->state->context;
    const QByteArray utf8 = name.toUtf8();
    if (!propertyValue.isValid() && !(flags & (PropertyGetter | PropertySetter))) {
        JSAtom atom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
        if (atom != JS_ATOM_NULL) {
            if (JS_DeleteProperty(context, object->value, atom, 0) < 0) {
                JSValue exception = JS_GetException(context);
                object->state->rememberException(exception);
            } else {
                storePropertyFlags(context, object->value, name, {});
            }
            JS_FreeAtom(context, atom);
        }
        return;
    }
    if (propertyValue.engine() && propertyValue.engine() != this->engine()) {
        qWarning("QScriptValue::setProperty(%s) failed: cannot set value created in a different engine",
                 utf8.constData());
        return;
    }
    JSValue value = JS_UNDEFINED;
    if (!materialize(propertyValue, engine, &value))
        return;
    if (!propertyValue.engine()) {
        QScriptValue *mutablePropertyValue = const_cast<QScriptValue *>(&propertyValue);
        mutablePropertyValue->d_ptr = new QScriptValuePrivate(
            object->state, JS_DupValue(context, value), true);
    }
    const PropertyFlags flagsToStore = flags & KeepExistingFlags
        ? storedPropertyFlags(context, object->value, name)
        : flags & UserRange;
    JSAtom existingAtom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
    JSPropertyDescriptor existingDescriptor{};
    const bool hasExistingAccessor = !(flags & (PropertyGetter | PropertySetter))
        && existingAtom != JS_ATOM_NULL
        && ownProperty(context, object->value, existingAtom, &existingDescriptor)
        && (existingDescriptor.flags & JS_PROP_GETSET)
        && !JS_IsUndefined(existingDescriptor.getter)
        && JS_IsUndefined(existingDescriptor.setter);
    if (hasExistingAccessor) {
        qWarning("QScriptValue::setProperty() failed: property '%s' has a getter but no setter",
                 utf8.constData());
        JS_FreeValue(context, value);
        JS_FreeValue(context, existingDescriptor.value);
        JS_FreeValue(context, existingDescriptor.getter);
        JS_FreeValue(context, existingDescriptor.setter);
        if (existingAtom != JS_ATOM_NULL)
            JS_FreeAtom(context, existingAtom);
        return;
    }
    JS_FreeValue(context, existingDescriptor.value);
    JS_FreeValue(context, existingDescriptor.getter);
    JS_FreeValue(context, existingDescriptor.setter);
    if (existingAtom != JS_ATOM_NULL)
        JS_FreeAtom(context, existingAtom);
    if (flags & (PropertyGetter | PropertySetter)) {
        JSAtom atom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(context, value);
            return;
        }
        if (name == QLatin1String("__proto__")
            && !ownProperty(context, object->value, atom)) {
            if (propertyValue.isValid()) {
                qWarning("QScriptValue::setProperty() failed: cannot set getter or setter "
                         "of native property `__proto__'");
            }
            JS_FreeValue(context, value);
            JS_FreeAtom(context, atom);
            return;
        }
        JSPropertyDescriptor descriptor{};
        const bool exists = ownProperty(context, object->value, atom, &descriptor);
        JSValue getter = exists && (descriptor.flags & JS_PROP_GETSET)
            ? descriptor.getter : JS_UNDEFINED;
        JSValue setter = exists && (descriptor.flags & JS_PROP_GETSET)
            ? descriptor.setter : JS_UNDEFINED;
        if (exists && !(descriptor.flags & JS_PROP_GETSET)) {
            JS_FreeValue(context, descriptor.value);
            JS_FreeValue(context, descriptor.getter);
            JS_FreeValue(context, descriptor.setter);
            getter = JS_UNDEFINED;
            setter = JS_UNDEFINED;
        }
        if (flags & PropertyGetter) {
            if (exists && (descriptor.flags & JS_PROP_GETSET))
                JS_FreeValue(context, getter);
            getter = value;
        } else {
            JS_FreeValue(context, value);
        }
        if (flags & PropertySetter) {
            if (exists && (descriptor.flags & JS_PROP_GETSET))
                JS_FreeValue(context, setter);
            setter = propertyValue.isValid() ? engine->toQuickJS(propertyValue) : JS_UNDEFINED;
        }
        const int propertyFlags = flags & KeepExistingFlags
            ? (exists ? descriptor.flags : JS_PROP_C_W_E)
            : qScriptQuickJSPropertyFlags(flags);
        if (JS_IsUndefined(getter) && JS_IsUndefined(setter)) {
            if (JS_DeleteProperty(context, object->value, atom, 0) < 0) {
                JSValue exception = JS_GetException(context);
                object->state->rememberException(exception);
            }
            JS_FreeValue(context, getter);
            JS_FreeValue(context, setter);
        } else if (JS_DefinePropertyGetSet(context, object->value, atom, getter, setter,
                                           propertyFlags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) < 0) {
            JSValue exception = JS_GetException(context);
            object->state->rememberException(exception);
        }
        JS_FreeAtom(context, atom);
        if (exists && (descriptor.flags & JS_PROP_GETSET)) {
            JS_FreeValue(context, descriptor.value);
        }
        storePropertyFlags(context, object->value, name, {});
        return;
    }
    if (flags & KeepExistingFlags) {
        JSAtom atom = JS_NewAtomLen(context, utf8.constData(), size_t(utf8.size()));
        const bool exists = atom != JS_ATOM_NULL && ownProperty(context, object->value, atom);
        if (atom != JS_ATOM_NULL)
            JS_FreeAtom(context, atom);
        if (!exists && isStaticScopeObject(context, object->value)) {
            if (JS_DefinePropertyValueStr(context, object->value, utf8.constData(), value,
                                          JS_PROP_WRITABLE | JS_PROP_ENUMERABLE) < 0) {
                JSValue exception = JS_GetException(context);
                object->state->rememberException(exception);
            }
        } else if (JS_SetPropertyStr(context, object->value, utf8.constData(), value) < 0) {
            JSValue exception = JS_GetException(context);
            object->state->rememberException(exception);
        }
    } else {
        PropertyFlags effectiveFlags = flags;
        if (isStaticScopeObject(context, object->value))
            effectiveFlags |= Undeletable;
        if (JS_DefinePropertyValueStr(context, object->value, utf8.constData(), value,
                                      qScriptQuickJSPropertyFlags(effectiveFlags)) < 0) {
            JSValue exception = JS_GetException(context);
            object->state->rememberException(exception);
        }
    }
    storePropertyFlags(context, object->value, name, flagsToStore);
}

QScriptValue QScriptValue::property(quint32 index, const ResolveFlags &mode) const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return invalidValue();
    JSAtom atom = JS_NewAtomUInt32(value->state->context, index);
    const bool exists = resolvedProperty(value->state->context, value->value, atom, mode);
    if (!exists) {
        JS_FreeAtom(value->state->context, atom);
        return invalidValue();
    }
    JSValue result = JS_GetPropertyUint32(value->state->context, value->value, index);
    JS_FreeAtom(value->state->context, atom);
    if (JS_IsException(result))
        return exceptionValue(QScriptEnginePrivate::get(value->state->engine));
    return fromOwnedValue(value, result);
}

void QScriptValue::setProperty(quint32 index, const QScriptValue &propertyValue,
                               const PropertyFlags &flags)
{
    QScriptValuePrivate *object = QScriptValuePrivate::get(*this);
    if (!isQuickValue(object) || !JS_IsObject(object->value))
        return;
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(object->state->engine);
    if (!propertyValue.isValid() && !(flags & (PropertyGetter | PropertySetter))) {
        JSAtom atom = JS_NewAtomUInt32(object->state->context, index);
        if (JS_DeleteProperty(object->state->context, object->value, atom, 0) < 0) {
            JSValue exception = JS_GetException(object->state->context);
            object->state->rememberException(exception);
        } else {
            storePropertyFlags(object->state->context, object->value,
                               QString::number(index), {});
        }
        JS_FreeAtom(object->state->context, atom);
        return;
    }
    if (propertyValue.engine() && propertyValue.engine() != this->engine()) {
        qWarning("QScriptValue::setProperty(%u) failed: cannot set value created in a different engine",
                 index);
        return;
    }
    JSValue value = JS_UNDEFINED;
    if (!materialize(propertyValue, engine, &value))
        return;
    if (!propertyValue.engine()) {
        QScriptValue *mutablePropertyValue = const_cast<QScriptValue *>(&propertyValue);
        mutablePropertyValue->d_ptr = new QScriptValuePrivate(
            object->state, JS_DupValue(object->state->context, value), true);
    }
    const QString propertyName = QString::number(index);
    const PropertyFlags flagsToStore = flags & KeepExistingFlags
        ? storedPropertyFlags(object->state->context, object->value, propertyName)
        : flags & UserRange;
    if (flags & (PropertyGetter | PropertySetter)) {
        JSAtom atom = JS_NewAtomUInt32(object->state->context, index);
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(object->state->context, value);
            return;
        }
        JSPropertyDescriptor descriptor{};
        const bool exists = ownProperty(object->state->context, object->value, atom, &descriptor);
        JSValue getter = exists && (descriptor.flags & JS_PROP_GETSET)
            ? descriptor.getter : JS_UNDEFINED;
        JSValue setter = exists && (descriptor.flags & JS_PROP_GETSET)
            ? descriptor.setter : JS_UNDEFINED;
        if (exists && !(descriptor.flags & JS_PROP_GETSET)) {
            JS_FreeValue(object->state->context, descriptor.value);
            JS_FreeValue(object->state->context, descriptor.getter);
            JS_FreeValue(object->state->context, descriptor.setter);
            getter = JS_UNDEFINED;
            setter = JS_UNDEFINED;
        }
        if (flags & PropertyGetter) {
            if (exists && (descriptor.flags & JS_PROP_GETSET))
                JS_FreeValue(object->state->context, getter);
            getter = value;
        } else {
            JS_FreeValue(object->state->context, value);
        }
        if (flags & PropertySetter) {
            if (exists && (descriptor.flags & JS_PROP_GETSET))
                JS_FreeValue(object->state->context, setter);
            setter = propertyValue.isValid() ? engine->toQuickJS(propertyValue) : JS_UNDEFINED;
        }
        const int propertyFlags = flags & KeepExistingFlags
            ? (exists ? descriptor.flags : JS_PROP_C_W_E)
            : qScriptQuickJSPropertyFlags(flags);
        if (JS_IsUndefined(getter) && JS_IsUndefined(setter)) {
            if (JS_DeleteProperty(object->state->context, object->value, atom, 0) < 0) {
                JSValue exception = JS_GetException(object->state->context);
                object->state->rememberException(exception);
            }
            JS_FreeValue(object->state->context, getter);
            JS_FreeValue(object->state->context, setter);
        } else if (JS_DefinePropertyGetSet(object->state->context, object->value, atom,
                                           getter, setter,
                                           propertyFlags & (JS_PROP_CONFIGURABLE
                                                            | JS_PROP_ENUMERABLE)) < 0) {
            JSValue exception = JS_GetException(object->state->context);
            object->state->rememberException(exception);
        }
        JS_FreeAtom(object->state->context, atom);
        if (exists && (descriptor.flags & JS_PROP_GETSET))
            JS_FreeValue(object->state->context, descriptor.value);
        storePropertyFlags(object->state->context, object->value, propertyName, {});
        return;
    }
    if (flags & KeepExistingFlags)
        JS_SetPropertyUint32(object->state->context, object->value, index, value);
    else
        JS_DefinePropertyValueUint32(object->state->context, object->value, index, value,
                                     qScriptQuickJSPropertyFlags(flags));
    storePropertyFlags(object->state->context, object->value, propertyName, flagsToStore);
}

QScriptValue QScriptValue::property(const QScriptString &name, const ResolveFlags &mode) const
{
    return property(name.toString(), mode);
}

void QScriptValue::setProperty(const QScriptString &name, const QScriptValue &propertyValue,
                               const PropertyFlags &flags)
{
    setProperty(name.toString(), propertyValue, flags);
}

QScriptValue::PropertyFlags QScriptValue::propertyFlags(const QString &name,
                                                        const ResolveFlags &mode) const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return PropertyFlags();
    const QByteArray utf8 = name.toUtf8();
    JSAtom atom = JS_NewAtomLen(value->state->context, utf8.constData(), size_t(utf8.size()));
    if (atom == JS_ATOM_NULL)
        return PropertyFlags();
    JSPropertyDescriptor descriptor{};
    const bool exists = resolvedProperty(value->state->context, value->value, atom, mode,
                                         &descriptor);
    JS_FreeAtom(value->state->context, atom);
    if (!exists)
        return PropertyFlags();
    PropertyFlags result = propertyFlagsFromDescriptor(descriptor);
    result |= storedPropertyFlags(value->state->context, value->value, name);
    JSValue global = JS_GetGlobalObject(value->state->context);
    const bool isGlobal = JS_IsStrictEqual(value->state->context, global, value->value);
    JS_FreeValue(value->state->context, global);
    if (isGlobal && (name == QLatin1String("NaN")
                     || name == QLatin1String("Infinity")
                     || name == QLatin1String("undefined")))
        result &= ~QScriptValue::ReadOnly;
    if (const auto *payload = static_cast<const QObjectPayloadView *>(
            qobjectOpaque(value->state.data(), value->value))) {
        if (payload->object) {
            const QMetaObject *metaObject = payload->object->metaObject();
            const QByteArray memberName = name.toUtf8();
            bool isMember = metaObject->indexOfProperty(memberName.constData()) >= 0;
            bool isMethod = false;
            for (int index = 0; index < metaObject->methodCount(); ++index) {
                const QMetaMethod method = metaObject->method(index);
                const bool matches = method.name() == memberName
                    || method.methodSignature() == memberName;
                isMember = isMember || matches;
                isMethod = isMethod || matches;
            }
            for (const QByteArray &dynamicName : payload->object->dynamicPropertyNames())
                isMember = isMember || dynamicName == memberName;
            if (isMember) {
                result |= QObjectMember;
                if (isMethod && !name.contains(QLatin1Char('(')))
                    result &= ~QScriptValue::SkipInEnumeration;
                if ((descriptor.flags & JS_PROP_GETSET)
                    && JS_IsUndefined(descriptor.setter))
                    result |= QScriptValue::ReadOnly;
            }
            for (QObject *child : payload->object->children()) {
                if (child->objectName().toUtf8() == memberName) {
                    result &= ~QScriptValue::QObjectMember;
                    result |= QScriptValue::ReadOnly | QScriptValue::Undeletable
                        | QScriptValue::SkipInEnumeration;
                    break;
                }
            }
        }
    }
    JS_FreeValue(value->state->context, descriptor.value);
    JS_FreeValue(value->state->context, descriptor.getter);
    JS_FreeValue(value->state->context, descriptor.setter);
    return result;
}

QScriptValue::PropertyFlags QScriptValue::propertyFlags(const QScriptString &name,
                                                        const ResolveFlags &mode) const
{
    return propertyFlags(name.toString(), mode);
}

QScriptValue QScriptValue::call(const QScriptValue &thisObject,
                                const QScriptValueList &arguments)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return invalidValue();
    if (thisObject.engine() && thisObject.engine() != engine()) {
        qWarning("QScriptValue::call() failed: cannot call function with thisObject created in a different engine");
        return invalidValue();
    }
    for (const QScriptValue &argument : arguments) {
        if (argument.engine() && argument.engine() != engine()) {
            qWarning("QScriptValue::call() failed: cannot call function with argument created in a different engine");
            return invalidValue();
        }
    }
    QVector<JSValue> values;
    if (!materializeArguments(arguments, QScriptEnginePrivate::get(value->state->engine), &values))
        return invalidValue();
    QScriptValue result = callWithArguments(value, thisObject, values, false);
    freeValues(value->state.data(), &values);
    return result;
}

QScriptValue QScriptValue::call(const QScriptValue &thisObject,
                                const QScriptValue &arguments)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return invalidValue();
    if (thisObject.engine() && thisObject.engine() != engine()) {
        qWarning("QScriptValue::call() failed: cannot call function with thisObject created in a different engine");
        return invalidValue();
    }
    QVector<JSValue> values;
    if (arguments.isValid() && !arguments.isNull() && !arguments.isUndefined()) {
        QScriptValuePrivate *argumentValue = QScriptValuePrivate::get(arguments);
        if (!isQuickValue(argumentValue) || argumentValue->state != value->state
            || !propertyArguments(value->state->context, argumentValue->value, &values)) {
            if (argumentValue && argumentValue->state != value->state)
                qWarning("QScriptValue::call() failed: cannot call function with argument created in a different engine");
            return argumentListError(value);
        }
    }
    QScriptValue result = callWithArguments(value, thisObject, values, false);
    freeValues(value->state.data(), &values);
    return result;
}

QScriptValue QScriptValue::construct(const QScriptValueList &arguments)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return invalidValue();
    for (const QScriptValue &argument : arguments) {
        if (argument.engine() && argument.engine() != engine()) {
            qWarning("QScriptValue::construct() failed: cannot construct function with argument created in a different engine");
            return invalidValue();
        }
    }
    QVector<JSValue> values;
    if (!materializeArguments(arguments, QScriptEnginePrivate::get(value->state->engine), &values))
        return invalidValue();
    QScriptValue result = callWithArguments(value, QScriptValue(), values, true);
    freeValues(value->state.data(), &values);
    return result;
}

QScriptValue QScriptValue::construct(const QScriptValue &arguments)
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value))
        return invalidValue();
    if (arguments.engine() && arguments.engine() != engine()) {
        qWarning("QScriptValue::construct() failed: cannot construct function with argument created in a different engine");
        return invalidValue();
    }
    QVector<JSValue> values;
    if (arguments.isValid() && !arguments.isNull() && !arguments.isUndefined()) {
        QScriptValuePrivate *argumentValue = QScriptValuePrivate::get(arguments);
        if (!isQuickValue(argumentValue) || argumentValue->state != value->state
            || !propertyArguments(value->state->context, argumentValue->value, &values)) {
            if (argumentValue && argumentValue->state != value->state)
                qWarning("QScriptValue::construct() failed: cannot construct function with argument created in a different engine");
            return argumentListError(value);
        }
    }
    QScriptValue result = callWithArguments(value, QScriptValue(), values, true);
    freeValues(value->state.data(), &values);
    return result;
}

QScriptValue QScriptValue::data() const
{
    return property(QString::fromLatin1(dataProperty), ResolveLocal);
}

void QScriptValue::setData(const QScriptValue &dataValue)
{
    if (!isObject())
        return;
    if (!dataValue.isValid()) {
        QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
        const QByteArray name = QByteArray(dataProperty);
        JSAtom atom = JS_NewAtom(value->state->context, name.constData());
        JS_DeleteProperty(value->state->context, value->value, atom, 0);
        JS_FreeAtom(value->state->context, atom);
        return;
    }
    setProperty(QString::fromLatin1(dataProperty), dataValue,
                SkipInEnumeration);
}

QScriptClass *QScriptValue::scriptClass() const
{
    return qScriptQuickJSScriptClass(*this);
}

void QScriptValue::setScriptClass(QScriptClass *scriptClass)
{
    qScriptQuickJSSetScriptClass(*this, scriptClass);
}

qint64 QScriptValue::objectId() const
{
    QScriptValuePrivate *value = QScriptValuePrivate::get(*this);
    if (!isQuickValue(value) || !JS_IsObject(value->value))
        return -1;
    const qint64 id = qint64(quintptr(JS_VALUE_GET_PTR(value->value)));
    if (!value->state->objectIds.contains(id))
        value->state->objectIds.insert(id,
                                       JS_DupValue(value->state->context, value->value));
    return id;
}

QScriptContext::QScriptContext()
    : d_ptr(new QScriptContextPrivate)
{
    d_ptr->q_ptr = this;
}

QScriptContext::~QScriptContext()
{
    delete d_ptr;
}

QScriptContext *QScriptContext::parentContext() const
{
    QScriptContext *parent = d_ptr ? d_ptr->parent : nullptr;
    while (parent) {
        QScriptContextPrivate *parentPrivate = QScriptContextPrivate::get(parent);
        if (!parentPrivate || !parentPrivate->hiddenFromBacktrace)
            break;
        parent = parentPrivate->parent;
    }
    return parent;
}

QScriptEngine *QScriptContext::engine() const
{
    return d_ptr ? d_ptr->engine : nullptr;
}

QScriptContext::ExecutionState QScriptContext::state() const
{
    return d_ptr ? d_ptr->state : NormalState;
}

QScriptValue QScriptContext::callee() const
{
    return d_ptr ? d_ptr->callee : QScriptValue();
}

int QScriptContext::argumentCount() const
{
    return d_ptr ? d_ptr->arguments.size() : 0;
}

QScriptValue QScriptContext::argument(int index) const
{
    if (!d_ptr || index < 0)
        return invalidValue();
    if (index >= d_ptr->arguments.size())
        return undefinedValue();
    return d_ptr->arguments.at(index);
}

QScriptValue QScriptContext::argumentsObject() const
{
    if (!d_ptr || !d_ptr->engine)
        return invalidValue();
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(d_ptr->engine);
    if (!engine)
        return invalidValue();
    if (d_ptr->argumentsObjectCache.isValid())
        return d_ptr->argumentsObjectCache;
    QScriptValue result = d_ptr->engine->newArray(uint(d_ptr->arguments.size()));
    if (d_ptr->callee.isValid())
        result.setProperty(QStringLiteral("callee"), d_ptr->callee,
                           QScriptValue::SkipInEnumeration);
    for (int index = 0; index < d_ptr->arguments.size(); ++index)
        result.setProperty(uint(index), d_ptr->arguments.at(index));
    d_ptr->argumentsObjectCache = result;
    return result;
}

QScriptValueList QScriptContext::scopeChain() const
{
    return d_ptr ? d_ptr->scopes : QScriptValueList();
}

void QScriptContext::pushScope(const QScriptValue &object)
{
    if (!d_ptr || !object.isObject())
        return;
    if (object.engine() && object.engine() != d_ptr->engine) {
        qWarning("QScriptContext::pushScope() failed: cannot push an object created in a different engine");
        return;
    }
    if (d_ptr->scopes.isEmpty()
        && !object.strictlyEquals(d_ptr->engine->globalObject())) {
        qWarning("QScriptContext::pushScope() failed: initial object in scope chain has to be the Global Object");
        return;
    }
    d_ptr->scopes.prepend(object);
    if (d_ptr->engine && d_ptr->scopes.size() > 1
        && d_ptr->activationObject.strictlyEquals(d_ptr->scopes.at(1))
        && !d_ptr->activationObject.strictlyEquals(d_ptr->engine->globalObject()))
        d_ptr->activationObject = object;
}

QScriptValue QScriptContext::popScope()
{
    if (!d_ptr || d_ptr->scopes.isEmpty())
        return invalidValue();
    const QScriptValue result = d_ptr->scopes.takeFirst();
    if (result.strictlyEquals(d_ptr->activationObject))
        d_ptr->activationObject = d_ptr->scopes.isEmpty() ? QScriptValue()
                                                          : d_ptr->scopes.first();
    return result;
}

QScriptValue QScriptContext::returnValue() const
{
    return d_ptr ? d_ptr->returnValue : QScriptValue();
}

void QScriptContext::setReturnValue(const QScriptValue &result)
{
    if (d_ptr && (!result.engine() || result.engine() == d_ptr->engine))
        d_ptr->returnValue = result;
}

QScriptValue QScriptContext::activationObject() const
{
    if (!d_ptr)
        return invalidValue();
    if (d_ptr->activationObject.isValid())
        return d_ptr->activationObject;
    return d_ptr->engine ? d_ptr->engine->globalObject() : QScriptValue();
}

void QScriptContext::setActivationObject(const QScriptValue &activation)
{
    if (!d_ptr || !activation.isObject())
        return;
    if (activation.engine() && activation.engine() != d_ptr->engine) {
        qWarning("QScriptContext::setActivationObject() failed: cannot set an object created in a different engine");
        return;
    }
    const QScriptValue oldActivation = d_ptr->activationObject;
    d_ptr->activationObject = activation;
    d_ptr->activationObjectWasSet = true;
    if (!d_ptr->scopes.isEmpty() && d_ptr->scopes.first().strictlyEquals(oldActivation))
        d_ptr->scopes[0] = activation;
}

QScriptValue QScriptContext::thisObject() const
{
    if (!d_ptr)
        return invalidValue();
    return d_ptr->thisObject.isValid() ? d_ptr->thisObject
                                       : (d_ptr->engine ? d_ptr->engine->globalObject()
                                                        : QScriptValue());
}

void QScriptContext::setThisObject(const QScriptValue &thisValue)
{
    if (!d_ptr || !thisValue.isObject())
        return;
    if (thisValue.engine() && thisValue.engine() != d_ptr->engine) {
        qWarning("QScriptContext::setThisObject() failed: cannot set an object created in a different engine");
        return;
    }
    d_ptr->thisObject = thisValue;
}

bool QScriptContext::isCalledAsConstructor() const
{
    return d_ptr && d_ptr->calledAsConstructor;
}

QScriptValue QScriptContext::throwValue(const QScriptValue &value)
{
    if (!d_ptr || !d_ptr->engine)
        return invalidValue();
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(d_ptr->engine);
    bool ok = false;
    JSValue materialized = engine ? engine->toQuickJS(value, &ok) : JS_UNDEFINED;
    if (!ok)
        return invalidValue();
    const int nativeLine = d_ptr->lineNumber >= 0
        ? d_ptr->lineNumber
        : (engine->state->evaluating ? -1 : 0);
    engine->state->rememberException(JS_DupValue(engine->state->context, materialized),
                                     nativeLine);
    JS_FreeValue(engine->state->context, materialized);
    d_ptr->thrownValue = value;
    d_ptr->state = ExceptionState;
    return value;
}

QScriptValue QScriptContext::throwError(Error error, const QString &text)
{
    if (!d_ptr || !d_ptr->engine)
        return invalidValue();
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(d_ptr->engine);
    if (!engine || !engine->state || !engine->state->context)
        return invalidValue();
    const QByteArray utf8 = text.toUtf8();
    const char *errorName = "Error";
    switch (error) {
    case ReferenceError: errorName = "ReferenceError"; break;
    case SyntaxError: errorName = "SyntaxError"; break;
    case TypeError: errorName = "TypeError"; break;
    case RangeError: errorName = "RangeError"; break;
    case URIError: errorName = "URIError"; break;
    case UnknownError: break;
    }

    // The printf-style JS_New*Error helpers use a bounded formatting buffer;
    // long QtScript diagnostics (notably constructor overload lists) were
    // silently truncated.  Build the Error object with length-aware strings.
    JSContext *context = engine->state->context;
    JSValue result = JS_NewError(context);
    JS_DefinePropertyValueStr(context, result, "name",
                              JS_NewString(context, errorName), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, result, "message",
                              JS_NewStringLen(context, utf8.constData(),
                                              size_t(utf8.size())),
                              JS_PROP_C_W_E);
    QScriptValue thrown = engine->fromOwned(result);
    return throwValue(thrown);
}

QScriptValue QScriptContext::throwError(const QString &text)
{
    return throwError(UnknownError, text);
}

QStringList QScriptContext::backtrace() const
{
    if (!d_ptr)
        return QStringList();
    QStringList result;
    for (const QScriptContext *context = this; context; context = context->parentContext()) {
        const QScriptContextPrivate *contextPrivate =
            QScriptContextPrivate::get(const_cast<QScriptContext *>(context));
        if (contextPrivate && contextPrivate->hiddenFromBacktrace)
            continue;
        result.append(context->toString());
    }
    return result;
}

QString QScriptContext::toString() const
{
    if (!d_ptr)
        return QString();
    QString result = d_ptr->backtraceName.isEmpty() ? contextFunctionName(d_ptr)
                                                    : d_ptr->backtraceName;
    result += QLatin1Char('(');
    for (int index = 0; index < d_ptr->arguments.size(); ++index) {
        if (index)
            result += QStringLiteral(", ");
        if (index < d_ptr->parameterNames.size()
            && !d_ptr->parameterNames.at(index).isEmpty()) {
            result += d_ptr->parameterNames.at(index);
            result += QStringLiteral(" = ");
        }
        const QScriptValue argumentValue = d_ptr->arguments.at(index);
        if (argumentValue.isString())
            result += QLatin1Char('\'');
        result += argumentValue.toString();
        if (argumentValue.isString())
            result += QLatin1Char('\'');
    }
    result += QStringLiteral(") at ");
    if (!d_ptr->fileName.isEmpty())
        result += d_ptr->fileName + QLatin1Char(':') + QString::number(d_ptr->lineNumber);
    else
        result += QString::number(d_ptr->lineNumber);
    return result;
}

QT_END_NAMESPACE
