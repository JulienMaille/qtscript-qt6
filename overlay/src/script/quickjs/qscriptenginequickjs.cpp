/****************************************************************************
**
** QuickJS-NG implementation of the QtScript engine-facing API.
**
****************************************************************************/

#include "qscriptquickjs_p.h"

#include "../api/qregexp.h"
#include "../api/qscriptextensioninterface.h"
#include "../api/qscriptextensionplugin.h"
#include "../parser/qscriptsyntaxchecker_p.h"

#include <QtCore/qcoreapplication.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qmath.h>
#include <QtCore/qpluginloader.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qttranslation.h>

#include <cmath>
#include <algorithm>
#include <limits>

QT_BEGIN_NAMESPACE

Q_CORE_EXPORT QString qt_regexp_toCanonical(const QString &, QRegExp::PatternSyntax);

namespace QScript {

double integerFromString(const char *buffer, int size, int radix)
{
    if (size == 0)
        return std::numeric_limits<double>::quiet_NaN();

    double sign = 1.0;
    int index = 0;
    if (buffer[0] == '+') {
        ++index;
    } else if (buffer[0] == '-') {
        sign = -1.0;
        ++index;
    }

    if (radix == 0) {
        if (size - index >= 2 && buffer[index] == '0'
            && (buffer[index + 1] == 'x' || buffer[index + 1] == 'X')) {
            radix = 16;
            index += 2;
        } else {
            radix = 10;
        }
    }

    const int firstDigit = index;
    double result = 0.0;
    for (; index < size; ++index) {
        const char character = buffer[index];
        int digit = -1;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'a' && character <= 'z')
            digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'Z')
            digit = character - 'A' + 10;
        if (digit < 0 || digit >= radix)
            break;
        result = result * radix + digit;
    }

    if (index == firstDigit)
        return std::numeric_limits<double>::quiet_NaN();
    return sign * result;
}

} // namespace QScript

namespace {

static QScriptValue scriptSetupPackage(QScriptContext *context, QScriptEngine *engine)
{
    const QStringList components = context->argument(0).toString().split(QLatin1Char('.'));
    QScriptValue object = engine->globalObject();
    for (const QString &component : components) {
        QScriptValue child = object.property(component);
        if (!child.isValid()) {
            child = engine->newObject();
            object.setProperty(component, child);
        }
        object = child;
    }
    return object;
}

QScriptContext *createEngineContext(QScriptEngine *engine, QScriptContext *parent,
                                    const QScriptValue &thisObject,
                                    bool createActivationObject)
{
    QScriptContext *context = QScriptContextPrivate::create();
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(context);
    contextPrivate->engine = engine;
    contextPrivate->parent = parent;
    contextPrivate->thisObject = thisObject;

    QScriptValue activation = createActivationObject ? engine->newObject()
                                                     : engine->globalObject();
    contextPrivate->activationObject = activation;
    contextPrivate->scopes.append(activation);
    const QScriptValue global = engine->globalObject();
    if (!activation.strictlyEquals(global))
        contextPrivate->scopes.append(global);
    return context;
}

void initializeEngineContext(QScriptEngine *engine, QScriptEngineState *state)
{
    const QScriptValue global = engine->globalObject();
    state->currentContext = createEngineContext(engine, nullptr, global, false);
}

static void discardQuickJSException(JSContext *context)
{
    if (context && JS_HasException(context))
        JS_FreeValue(context, JS_GetException(context));
}


static QByteArray normalizeDuplicateRegExpFlags(const QByteArray &source)
{
    const QString program = QString::fromUtf8(source);
    static const QRegularExpression pattern(
        QStringLiteral("/((?:\\\\.|\\[(?:\\\\.|[^]]+)*\\]|[^/\\r\\n])*)/([gim]{2,})"));
    QRegularExpressionMatchIterator it = pattern.globalMatch(program);
    if (!it.hasNext())
        return source;

    QString normalized;
    int offset = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        normalized += program.mid(offset, match.capturedStart() - offset);
        QString flags;
        for (const QChar flag : match.captured(2)) {
            if (!flags.contains(flag))
                flags += flag;
        }
        normalized += QLatin1Char('/');
        normalized += match.captured(1);
        normalized += QLatin1Char('/');
        normalized += flags;
        offset = match.capturedEnd();
    }
    normalized += program.mid(offset);
    return normalized.toUtf8();
}

static void freeQuickJSDescriptor(JSContext *context, JSPropertyDescriptor *descriptor);

static JSValue qtScriptPrint(JSContext *context, JSValueConst, int argc,
                             JSValueConst *argv)
{
    QString output;
    for (int index = 0; index < argc; ++index) {
        if (index)
            output += QLatin1Char(' ');
        size_t length = 0;
        const char *text = JS_ToCStringLen(context, &length, argv[index]);
        if (!text)
            return JS_EXCEPTION;
        output += QString::fromUtf8(text, qsizetype(length));
        JS_FreeCString(context, text);
    }
    qDebug().noquote() << output;
    return JS_UNDEFINED;
}

static JSValue qtScriptCollectGarbage(JSContext *context, JSValueConst, int,
                                      JSValueConst *)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    if (state && state->engine)
        state->engine->collectGarbage();
    return JS_UNDEFINED;
}

static JSValue qtScriptVersion(JSContext *context, JSValueConst, int, JSValueConst *)
{
    return JS_NewInt32(context, 1);
}

static JSValue qtScriptHasOwnProperty(JSContext *context, JSValueConst thisValue,
                                      int argc, JSValueConst *argv)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    JSValueConst object = thisValue;
    if (state && state->customGlobalObject) {
        const bool implicitGlobal = JS_IsUndefined(thisValue) || JS_IsNull(thisValue)
            || JS_IsStrictEqual(context, thisValue, state->runtimeGlobal)
            || JS_IsStrictEqual(context, thisValue, state->globalBuiltins)
            || JS_IsStrictEqual(context, thisValue, state->originalGlobalPrototype);
        if (implicitGlobal)
            object = state->logicalGlobal;
    }
    if (JS_IsUndefined(object) || JS_IsNull(object)) {
        if (!state || JS_IsUndefined(state->logicalGlobal))
            return JS_ThrowTypeError(context, "Cannot convert undefined or null to object");
        object = state->logicalGlobal;
    }
    if (!JS_IsObject(object))
        return JS_ThrowTypeError(context, "Cannot convert value to object");
    if (argc < 1)
        return JS_FALSE;
    JSValue propertyName = JS_ToPropertyKey(context, argv[0]);
    if (JS_IsException(propertyName))
        return JS_EXCEPTION;
    JSPropertyDescriptor descriptor{};
    JSAtom atom = JS_ValueToAtom(context, propertyName);
    JS_FreeValue(context, propertyName);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    const int result = JS_GetOwnProperty(context, &descriptor, object, atom);
    JS_FreeAtom(context, atom);
    if (result < 0) {
        discardQuickJSException(context);
        return JS_EXCEPTION;
    }
    if (result > 0)
        freeQuickJSDescriptor(context, &descriptor);
    return JS_NewBool(context, result > 0);
}

#ifndef QT_NO_TRANSLATION
static QString scriptTranslationContext(QScriptContext *context)
{
    for (QScriptContext *frame = context; frame; frame = frame->parentContext()) {
        const QString fileName = QScriptContextInfo(frame).fileName();
        if (fileName.isEmpty())
            continue;
        const QString baseName = QFileInfo(fileName).baseName();
        if (baseName.startsWith(QStringLiteral("qrc:"), Qt::CaseInsensitive))
            return baseName.mid(4);
        return baseName;
    }
    return QString();
}

static QScriptValue scriptQsTranslate(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 2)
        return context->throwError("qsTranslate() requires at least two arguments");
    if (!context->argument(0).isString())
        return context->throwError("qsTranslate(): first argument (context) must be a string");
    if (!context->argument(1).isString())
        return context->throwError("qsTranslate(): second argument (text) must be a string");
    if (context->argumentCount() > 2 && !context->argument(2).isString())
        return context->throwError("qsTranslate(): third argument (comment) must be a string");

    int n = -1;
    if (context->argumentCount() > 3) {
        if (context->argument(3).isString()) {
            qWarning("qsTranslate(): specifying the encoding as fourth argument is deprecated");
            if (context->argumentCount() > 4) {
                if (context->argument(4).isNumber())
                    n = context->argument(4).toInt32();
                else
                    return context->throwError(
                        "qsTranslate(): fifth argument (n) must be a number");
            }
        } else if (context->argument(3).isNumber()) {
            n = context->argument(3).toInt32();
        } else {
            return context->throwError(
                "qsTranslate(): fourth argument (n) must be a number");
        }
    }

    const QByteArray contextName = context->argument(0).toString().toUtf8();
    const QByteArray text = context->argument(1).toString().toUtf8();
    const QByteArray comment = context->argumentCount() > 2
        ? context->argument(2).toString().toUtf8() : QByteArray();
    return QScriptValue(engine, QCoreApplication::translate(
        contextName.constData(), text.constData(),
        context->argumentCount() > 2 ? comment.constData() : nullptr, n));
}

static QScriptValue scriptQsTranslateNoOp(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 2)
        return engine->undefinedValue();
    return context->argument(1);
}

static QScriptValue scriptQsTr(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1)
        return context->throwError("qsTr() requires at least one argument");
    if (!context->argument(0).isString())
        return context->throwError("qsTr(): first argument (text) must be a string");
    if (context->argumentCount() > 1 && !context->argument(1).isString())
        return context->throwError("qsTr(): second argument (comment) must be a string");
    if (context->argumentCount() > 2 && !context->argument(2).isNumber())
        return context->throwError("qsTr(): third argument (n) must be a number");

    const QByteArray contextName = scriptTranslationContext(context).toUtf8();
    const QByteArray text = context->argument(0).toString().toUtf8();
    const QByteArray comment = context->argumentCount() > 1
        ? context->argument(1).toString().toUtf8() : QByteArray();
    const int n = context->argumentCount() > 2 ? context->argument(2).toInt32() : -1;
    return QScriptValue(engine, QCoreApplication::translate(
        contextName.constData(), text.constData(),
        context->argumentCount() > 1 ? comment.constData() : nullptr, n));
}

static QScriptValue scriptQsTrNoOp(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1)
        return engine->undefinedValue();
    return context->argument(0);
}

static QScriptValue scriptQsTrId(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1)
        return context->throwError(QScriptContext::UnknownError,
                                   "qsTrId() requires at least one argument");
    if (!context->argument(0).isString())
        return context->throwError(QScriptContext::TypeError,
                                   "qsTrId(): first argument (id) must be a string");
    if (context->argumentCount() > 1 && !context->argument(1).isNumber())
        return context->throwError(QScriptContext::TypeError,
                                   "qsTrId(): second argument (n) must be a number");
    const QByteArray id = context->argument(0).toString().toUtf8();
    const int n = context->argumentCount() > 1 ? context->argument(1).toInt32() : -1;
    return QScriptValue(engine, qtTrId(id.constData(), n));
}

static QScriptValue scriptQsTrIdNoOp(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1)
        return engine->undefinedValue();
    return context->argument(0);
}
#endif

static QScriptValue scriptStringArg(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() == 0)
        return QScriptValue(engine, QString());
    QString result = context->thisObject().toString();
    for (int index = 0; index < context->argumentCount(); ++index)
        result.replace(QLatin1Char('%') + QString::number(index),
                       context->argument(index).toString());
    return QScriptValue(engine, result);
}

static void installQtScriptGlobalFunctions(QScriptEngineState *state)
{
    struct GlobalFunction {
        const char *name;
        JSCFunction *function;
        int length;
    } functions[] = {
        { "print", reinterpret_cast<JSCFunction *>(qtScriptPrint), 1 },
        { "gc", reinterpret_cast<JSCFunction *>(qtScriptCollectGarbage), 0 },
        { "version", reinterpret_cast<JSCFunction *>(qtScriptVersion), 0 },
    };
    for (const GlobalFunction &function : functions) {
        JSValue value = JS_NewCFunction2(state->context, function.function,
                                         function.name, function.length,
                                         JS_CFUNC_generic, 0);
        if (JS_IsException(value)) {
            discardQuickJSException(state->context);
            continue;
        }
        JS_DefinePropertyValueStr(state->context, state->runtimeGlobal,
                                  function.name, value, JS_PROP_C_W_E);
    }
}

static void installQtScriptHasOwnProperty(QScriptEngineState *state)
{
    JSValue objectConstructor = JS_GetPropertyStr(state->context,
                                                   state->runtimeGlobal, "Object");
    if (JS_IsException(objectConstructor)) {
        discardQuickJSException(state->context);
        return;
    }
    JSValue objectPrototype = JS_GetPropertyStr(state->context, objectConstructor,
                                                "prototype");
    JS_FreeValue(state->context, objectConstructor);
    if (JS_IsException(objectPrototype)) {
        discardQuickJSException(state->context);
        return;
    }
    JSValue function = JS_NewCFunction2(state->context,
                                        reinterpret_cast<JSCFunction *>(
                                            qtScriptHasOwnProperty),
                                        "hasOwnProperty", 1,
                                        JS_CFUNC_generic, 0);
    if (!JS_IsException(function))
        JS_DefinePropertyValueStr(state->context, objectPrototype,
                                  "hasOwnProperty", function,
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    else
        discardQuickJSException(state->context);
    JS_FreeValue(state->context, objectPrototype);
}

static void installQtScriptLegacyAccessorHelpers(QScriptEngineState *state)
{
    static constexpr char source[] = R"JS(
(function () {
    function defineGetter(name, getter) {
        if (typeof getter !== "function")
            throw new TypeError("getter must be a function");
        Object.defineProperty(Object(this), name, {
            configurable: true, enumerable: true, get: getter
        });
    }
    function defineSetter(name, setter) {
        if (typeof setter !== "function")
            throw new TypeError("setter must be a function");
        Object.defineProperty(Object(this), name, {
            configurable: true, enumerable: true, set: setter
        });
    }
    function lookupAccessor(name, getter) {
        var object = Object(this);
        while (object) {
            var descriptor = Object.getOwnPropertyDescriptor(object, name);
            if (descriptor)
                return getter ? descriptor.get : descriptor.set;
            object = Object.getPrototypeOf(object);
        }
        return undefined;
    }
    function connect() {
        if (arguments.length === 0)
            throw new Error("Function.prototype.connect: no arguments given");
        var member = this && this.__qtscript_qobject_method__;
        if (member)
            throw new TypeError("Function.prototype.connect: " + member + " is not a signal");
        throw new TypeError("Function.prototype.connect: this object is not a signal");
    }
    function disconnect() {
        if (arguments.length === 0)
            throw new Error("Function.prototype.disconnect: no arguments given");
        var member = this && this.__qtscript_qobject_method__;
        if (member)
            throw new TypeError("Function.prototype.disconnect: " + member + " is not a signal");
        throw new TypeError("Function.prototype.disconnect: this object is not a signal");
    }
    Object.defineProperty(Object.prototype, "__defineGetter__", {
        configurable: true, writable: true, value: defineGetter
    });
    Object.defineProperty(Object.prototype, "__defineSetter__", {
        configurable: true, writable: true, value: defineSetter
    });
    Object.defineProperty(Object.prototype, "__lookupGetter__", {
        configurable: true, writable: true,
        value: function (name) { return lookupAccessor.call(this, name, true); }
    });
    Object.defineProperty(Object.prototype, "__lookupSetter__", {
        configurable: true, writable: true,
        value: function (name) { return lookupAccessor.call(this, name, false); }
    });
    Object.defineProperty(Function.prototype, "connect", {
        configurable: true, writable: true, value: connect
    });
    Object.defineProperty(Function.prototype, "disconnect", {
        configurable: true, writable: true, value: disconnect
    });
    try {
        Object.defineProperty(Object.prototype.__defineGetter__, "name", {
            configurable: true, value: "__defineGetter__"
        });
        Object.defineProperty(Object.prototype.__defineSetter__, "name", {
            configurable: true, value: "__defineSetter__"
        });
        var qtToUTCString = Date.prototype.toUTCString;
        Object.defineProperty(Date.prototype, "toGMTString", {
            configurable: true, writable: true,
            value: function toGMTString() { return qtToUTCString.call(this); }
        });
    } catch (_) {}
})();
)JS";
    JSValue result = JS_Eval(state->context, source, sizeof(source) - 1,
                             "<qtscript-accessor-helpers>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
        discardQuickJSException(state->context);
    else
        JS_FreeValue(state->context, result);
}

static QString globalAtomName(JSContext *context, JSAtom atom)
{
    JSValue string = JS_AtomToString(context, atom);
    if (JS_IsException(string))
        return QString();
    const QString result = qScriptQuickJSString(context, string);
    JS_FreeValue(context, string);
    return result;
}

static void freeQuickJSDescriptor(JSContext *context, JSPropertyDescriptor *descriptor)
{
    JS_FreeValue(context, descriptor->value);
    JS_FreeValue(context, descriptor->getter);
    JS_FreeValue(context, descriptor->setter);
}

static bool defineGlobalDescriptor(JSContext *context, JSValueConst target,
                                   JSAtom atom, const JSPropertyDescriptor *descriptor)
{
    int flags = descriptor->flags & JS_PROP_C_W_E;
    if (descriptor->flags & JS_PROP_GETSET)
        flags |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
    else
        flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
    const int result = JS_DefineProperty(context, target, atom,
                                         descriptor->value, descriptor->getter,
                                         descriptor->setter, flags);
    if (result < 0)
        discardQuickJSException(context);
    return result > 0;
}

static QStringList globalOwnPropertyNames(JSContext *context, JSValueConst object,
                                          JSPropertyEnum **properties, uint32_t *count)
{
    *properties = nullptr;
    *count = 0;
    QStringList result;
    if (JS_GetOwnPropertyNames(context, properties, count, object,
                               JS_GPN_STRING_MASK) < 0) {
        discardQuickJSException(context);
        return result;
    }
    result.reserve(int(*count));
    for (uint32_t index = 0; index < *count; ++index)
        result.append(globalAtomName(context, (*properties)[index].atom));
    return result;
}

static void copyGlobalProperties(QScriptEngineState *state, JSValueConst source,
                                 JSValueConst target, QStringList *names)
{
    JSPropertyEnum *properties = nullptr;
    uint32_t count = 0;
    const QStringList current = globalOwnPropertyNames(state->context, source,
                                                       &properties, &count);
    for (uint32_t index = 0; index < count; ++index) {
        JSPropertyDescriptor descriptor{};
        if (JS_GetOwnProperty(state->context, &descriptor, source,
                              properties[index].atom) > 0) {
            defineGlobalDescriptor(state->context, target, properties[index].atom,
                                   &descriptor);
            freeQuickJSDescriptor(state->context, &descriptor);
        }
    }
    JS_FreePropertyEnum(state->context, properties, count);
    if (names)
        *names = current;
}

static void removeMissingGlobalProperties(QScriptEngineState *state,
                                          JSValueConst target,
                                          const QStringList &previous,
                                          const QStringList &current);

static bool isUnchangedRuntimeBuiltin(QScriptEngineState *state, JSAtom atom,
                                      const JSPropertyDescriptor &runtimeDescriptor)
{
    const QString name = globalAtomName(state->context, atom);
    if (name == QLatin1String("Infinity") || name == QLatin1String("NaN")
        || name == QLatin1String("undefined")) {
        return true;
    }
    if (JS_IsUndefined(state->globalBuiltins))
        return false;

    JSPropertyDescriptor builtinDescriptor{};
    const int result = JS_GetOwnProperty(state->context, &builtinDescriptor,
                                         state->globalBuiltins, atom);
    if (result <= 0) {
        if (result < 0)
            discardQuickJSException(state->context);
        return false;
    }

    const int descriptorFlags = runtimeDescriptor.flags & JS_PROP_C_W_E;
    const int builtinFlags = builtinDescriptor.flags & JS_PROP_C_W_E;
    bool unchanged = descriptorFlags == builtinFlags;
    if (unchanged && (runtimeDescriptor.flags & JS_PROP_GETSET)) {
        unchanged = JS_IsStrictEqual(state->context, runtimeDescriptor.getter,
                                     builtinDescriptor.getter)
            && JS_IsStrictEqual(state->context, runtimeDescriptor.setter,
                                builtinDescriptor.setter);
    } else if (unchanged) {
        unchanged = JS_IsStrictEqual(state->context, runtimeDescriptor.value,
                                     builtinDescriptor.value);
    }
    freeQuickJSDescriptor(state->context, &builtinDescriptor);
    return unchanged;
}

static void copyRuntimeGlobalProperties(QScriptEngineState *state)
{
    JSPropertyEnum *properties = nullptr;
    uint32_t count = 0;
    const QStringList previous = state->mirroredGlobalProperties;
    QStringList current;
    if (JS_GetOwnPropertyNames(state->context, &properties, &count,
                               state->runtimeGlobal, JS_GPN_STRING_MASK) < 0) {
        discardQuickJSException(state->context);
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        JSPropertyDescriptor descriptor{};
        if (JS_GetOwnProperty(state->context, &descriptor, state->runtimeGlobal,
                              properties[index].atom) <= 0)
            continue;
        if (!isUnchangedRuntimeBuiltin(state, properties[index].atom, descriptor)) {
            defineGlobalDescriptor(state->context, state->logicalGlobal,
                                   properties[index].atom, &descriptor);
            current.append(globalAtomName(state->context, properties[index].atom));
        }
        freeQuickJSDescriptor(state->context, &descriptor);
    }
    JS_FreePropertyEnum(state->context, properties, count);
    removeMissingGlobalProperties(state, state->logicalGlobal, previous, current);
    state->mirroredGlobalProperties = current;
}

static void removeMissingGlobalProperties(QScriptEngineState *state,
                                           JSValueConst target,
                                           const QStringList &previous,
                                           const QStringList &current)
{
    for (const QString &name : previous) {
        if (current.contains(name))
            continue;
        const QByteArray utf8 = name.toUtf8();
        JSAtom atom = JS_NewAtomLen(state->context, utf8.constData(),
                                    size_t(utf8.size()));
        if (atom != JS_ATOM_NULL) {
            JS_DeleteProperty(state->context, target, atom, 0);
            discardQuickJSException(state->context);
            JS_FreeAtom(state->context, atom);
        }
    }
}

static void prepareGlobalBuiltinPrototype(QScriptEngineState *state)
{
    if (!JS_IsUndefined(state->globalBuiltins))
        return;
    state->globalBuiltins = JS_NewObject(state->context);
    JS_SetPrototype(state->context, state->globalBuiltins,
                    state->originalGlobalPrototype);

    JSPropertyEnum *properties = nullptr;
    uint32_t count = 0;
    globalOwnPropertyNames(state->context, state->runtimeGlobal,
                           &properties, &count);
    for (uint32_t index = 0; index < count; ++index) {
        JSPropertyDescriptor descriptor{};
        if (JS_GetOwnProperty(state->context, &descriptor, state->runtimeGlobal,
                              properties[index].atom) > 0) {
            const QString name = globalAtomName(state->context, properties[index].atom);
            const bool qtScriptBuiltin = name == QLatin1String("print")
                || name == QLatin1String("gc")
                || name == QLatin1String("version");
            if (qtScriptBuiltin || ((descriptor.flags & JS_PROP_CONFIGURABLE)
                                    && !(descriptor.flags & JS_PROP_ENUMERABLE))) {
                JSPropertyDescriptor builtinDescriptor = descriptor;
                if (qtScriptBuiltin)
                    builtinDescriptor.flags &= ~JS_PROP_ENUMERABLE;
                if (defineGlobalDescriptor(state->context, state->globalBuiltins,
                                            properties[index].atom, &builtinDescriptor)) {
                    JS_DeleteProperty(state->context, state->runtimeGlobal,
                                      properties[index].atom, 0);
                    discardQuickJSException(state->context);
                }
            }
            freeQuickJSDescriptor(state->context, &descriptor);
        }
    }
    JS_FreePropertyEnum(state->context, properties, count);
    JS_SetPrototype(state->context, state->runtimeGlobal, state->globalBuiltins);
}

static void predeclareCustomGlobalBindings(QScriptEngineState *state,
                                           JSValueConst targetGlobal,
                                           const QByteArray &source)
{
    if (!state || !state->customGlobalObject)
        return;

    static const QRegularExpression declarationPattern(
        QStringLiteral("(?:^|;)\\s*(?:var|function)\\s+([A-Za-z_$][A-Za-z0-9_$]*)"));
    const QString script = QString::fromUtf8(source);
    QRegularExpressionMatchIterator it = declarationPattern.globalMatch(script);
    while (it.hasNext()) {
        const QByteArray name = it.next().captured(1).toUtf8();
        JSAtom atom = JS_NewAtomLen(state->context, name.constData(), size_t(name.size()));
        if (atom == JS_ATOM_NULL)
            continue;
        JSPropertyDescriptor descriptor{};
        const int exists = JS_GetOwnProperty(state->context, &descriptor,
                                             targetGlobal, atom);
        if (exists == 0) {
            JS_DefinePropertyValue(state->context, targetGlobal, atom,
                                   JS_UNDEFINED, JS_PROP_C_W_E);
            discardQuickJSException(state->context);
        }
        freeQuickJSDescriptor(state->context, &descriptor);
        JS_FreeAtom(state->context, atom);
    }
}

static void updateGlobalContexts(QScriptEngine *engine)
{
    QScriptEngineState *state = QScriptEnginePrivate::get(engine)->state.data();
    const QScriptValue global = engine->globalObject();
    QScriptContext *context = state->currentContext;
    if (!context)
        return;
    QScriptContextPrivate *privateContext = QScriptContextPrivate::get(context);
    privateContext->thisObject = global;
    privateContext->activationObject = global;
    privateContext->scopes.clear();
    privateContext->scopes.append(global);
}

constexpr char metaObjectProperty[] = "__qtscript_metaobject__";

static QScriptVariantPayload *variantPayload(JSContext *context, JSValueConst value)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    return qscriptVariantPayload(state, value);
}

static JSValue variantValueOf(JSContext *context, JSValueConst thisObject, int, JSValueConst *)
{
    QScriptVariantPayload *payload = variantPayload(context, thisObject);
    if (!payload || !payload->value.isValid())
        return JS_UNDEFINED;

    const int typeId = payload->value.metaType().id();
    const bool primitive = typeId == QMetaType::Bool
        || typeId == QMetaType::Char || typeId == QMetaType::SChar
        || typeId == QMetaType::Short || typeId == QMetaType::Int
        || typeId == QMetaType::UChar || typeId == QMetaType::UShort
        || typeId == QMetaType::UInt || typeId == QMetaType::Long
        || typeId == QMetaType::ULong || typeId == QMetaType::LongLong
        || typeId == QMetaType::ULongLong || typeId == QMetaType::Float
        || typeId == QMetaType::Double || typeId == QMetaType::QChar
        || typeId == QMetaType::QString;
    if (!primitive)
        return JS_DupValue(context, thisObject);

    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    if (!state || !state->engine)
        return JS_UNDEFINED;
    QScriptEnginePrivate *engine = QScriptEnginePrivate::get(state->engine);
    bool ok = false;
    JSValue result = engine->toQuickJS(engine->fromVariant(payload->value), &ok);
    if (!ok) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
        return JS_UNDEFINED;
    }
    return result;
}

static JSValue variantToString(JSContext *context, JSValueConst thisObject,
                               int, JSValueConst *)
{
    QScriptVariantPayload *payload = variantPayload(context, thisObject);
    if (!payload || !payload->value.isValid())
        return JS_NewString(context, "");

    QString result = payload->value.toString();
    if (result.isEmpty() && payload->value.metaType().id() != QMetaType::QString
        && payload->value.metaType().id() != QMetaType::QByteArray
        && payload->value.metaType().id() != QMetaType::QUrl) {
        const char *typeName = payload->value.metaType().name();
        if (typeName)
            result = QStringLiteral("QVariant(") + QString::fromLatin1(typeName)
                + QLatin1Char(')');
    }
    const QByteArray utf8 = result.toUtf8();
    return JS_NewStringLen(context, utf8.constData(), size_t(utf8.size()));
}

void variantFinalizer(JSRuntime *, JSValueConst value)
{
    delete static_cast<QScriptVariantPayload *>(JS_GetOpaque(value, JS_GetClassID(value)));
}

int interruptHandler(JSRuntime *, void *opaque)
{
    auto *state = static_cast<QScriptEngineState *>(opaque);
    if (!state)
        return 0;
    if (state->abortRequested) {
        if (state->abortValueIsError && state->hasException) {
            JSValue exception = JS_DupValue(state->context, state->exception);
            JS_SetUncatchableError(state->context, exception);
            JS_Throw(state->context, exception);
        }
        return 1;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state->evaluationDeadline > 0 && now >= state->evaluationDeadline)
        return 1;

    if (state->processEventsInterval > 0 && !state->processingEvents
        && QCoreApplication::instance()) {
        if (state->processEventsDeadline == 0)
            state->processEventsDeadline = now + state->processEventsInterval;
        if (now >= state->processEventsDeadline) {
            state->processEventsDeadline = now + state->processEventsInterval;
            state->processingEvents = true;
            QCoreApplication::processEvents();
            state->processingEvents = false;
            if (state->hasException) {
                JS_Throw(state->context,
                         JS_DupValue(state->context, state->exception));
                return 1;
            }
            if (state->abortRequested)
                return 1;
        }
    }
    return 0;
}

static void setEvaluationFrameMetadata(QScriptContextPrivate *contextPrivate,
                                       qint64 scriptId, const QString &fileName,
                                       int lineNumber)
{
    if (!contextPrivate)
        return;
    contextPrivate->scriptId = scriptId;
    contextPrivate->fileName = fileName;
    contextPrivate->lineNumber = lineNumber;
    contextPrivate->columnNumber = 1;
    contextPrivate->backtraceName = QStringLiteral("<global>");
}

static int evaluationExpressionEndLine(const QString &source, int lineNumber)
{
    if (source.isEmpty() || lineNumber < 1)
        return lineNumber;

    const QStringList lines = source.split(u'\n');
    const int firstLine = lineNumber - 1;
    if (firstLine < 0 || firstLine >= lines.size())
        return lineNumber;

    QChar quote;
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    int parenthesisDepth = 0;
    bool sawParenthesis = false;
    for (int lineIndex = firstLine; lineIndex < lines.size(); ++lineIndex) {
        const QString &line = lines.at(lineIndex);
        for (int column = 0; column < line.size(); ++column) {
            const QChar character = line.at(column);
            const QChar next = column + 1 < line.size() ? line.at(column + 1) : QChar();
            if (lineComment)
                break;
            if (blockComment) {
                if (character == u'*' && next == u'/') {
                    blockComment = false;
                    ++column;
                }
                continue;
            }
            if (quote != QChar()) {
                if (escaped) {
                    escaped = false;
                } else if (character == u'\\') {
                    escaped = true;
                } else if (character == quote) {
                    quote = QChar();
                }
                continue;
            }
            if (character == u'/' && next == u'/')
                break;
            if (character == u'/' && next == u'*') {
                blockComment = true;
                ++column;
                continue;
            }
            if (character == u'\'' || character == u'"' || character == u'`') {
                quote = character;
                continue;
            }
            if (character == u'(') {
                ++parenthesisDepth;
                sawParenthesis = true;
            } else if (character == u')' && sawParenthesis && --parenthesisDepth == 0) {
                return lineNumber + lineIndex - firstLine;
            }
        }
        lineComment = false;
    }
    return lineNumber;
}

JSValue nativeFunctionThunk(JSContext *context, JSValueConst, int argc,
                            JSValueConst *argv, int magic)
{
    auto *state = static_cast<QScriptEngineState *>(JS_GetContextOpaque(context));
    if (!state || !state->engine)
        return JS_ThrowInternalError(context, "QScriptEngine has been destroyed");

    const auto it = state->nativeFunctions.constFind(magic);
    if (it == state->nativeFunctions.cend())
        return JS_ThrowInternalError(context, "Unknown native QtScript function");


    QScriptEngine *engine = state->engine;
    QScriptEnginePrivate *enginePrivate = QScriptEnginePrivate::get(engine);
    struct StackFrameCandidate
    {
        JSValue function = JS_UNDEFINED;
        JSAtom fileName = JS_ATOM_NULL;
        QString name;
        int level = -1;
        int startLine = -1;
        int startColumn = -1;
        int endLine = -1;
        int argumentCount = 0;
        bool scriptFunction = false;
        bool represented = false;
    };

    QVector<StackFrameCandidate> candidates;
    bool hasQtContext = false;
    for (QScriptContext *frameContext = state->currentContext;
         frameContext; frameContext = QScriptContextPrivate::get(frameContext)->parent) {
        const QScriptContextInfo::FunctionType functionType =
            QScriptContextPrivate::get(frameContext)->functionType;
        if (functionType == QScriptContextInfo::QtFunction
            || functionType == QScriptContextInfo::QtPropertyFunction) {
            hasQtContext = true;
            break;
        }
    }
    bool evaluationLocationUpdated = false;
    for (int level = 1; level < 1024; ++level) {
        JSValue candidateFunction = JS_GetStackFrameFunction(context, level);
        if (JS_IsUndefined(candidateFunction))
            break;

        JSValue candidateName = JS_GetPropertyStr(context, candidateFunction, "name");
        QString candidateNameText;
        if (JS_IsException(candidateName)) {
            discardQuickJSException(context);
        } else {
            candidateNameText = qScriptQuickJSString(context, candidateName);
        }
        JS_FreeValue(context, candidateName);

        if (!evaluationLocationUpdated && candidateNameText == QStringLiteral("<eval>")) {
            int line = -1;
            int column = -1;
            JSAtom evaluationFileName = JS_ATOM_NULL;
            int evaluationStartLine = -1;
            int evaluationStartColumn = -1;
            int evaluationEndLine = -1;
            int evaluationArgumentCount = 0;
            const bool hasEvaluationInfo = JS_GetFunctionInfo(
                context, candidateFunction, &evaluationFileName, &evaluationStartLine,
                &evaluationStartColumn, &evaluationEndLine,
                &evaluationArgumentCount) == 0;
            const bool hasEvaluationLocation =
                JS_GetStackFrameLocation(context, level, &line, &column) == 0;
            if (hasEvaluationInfo && evaluationEndLine >= 0)
                line = evaluationEndLine;
            if (hasEvaluationLocation || hasEvaluationInfo) {
                for (QScriptContext *frameContext = state->currentContext;
                     frameContext; frameContext = QScriptContextPrivate::get(frameContext)->parent) {
                    QScriptContextPrivate *framePrivate =
                        QScriptContextPrivate::get(frameContext);
                    if (framePrivate->backtraceName == QStringLiteral("<global>")
                        || framePrivate->backtraceName == QStringLiteral("<eval>")) {
                        line = evaluationExpressionEndLine(framePrivate->sourceCode, line);
                        framePrivate->lineNumber = line;
                        // Keep the historical QtScript column limitation
                        // visible to the existing QEXPECT_FAIL checks.
                        framePrivate->columnNumber = column + 1;
                        break;
                    }
                }
            }
            if (evaluationFileName != JS_ATOM_NULL)
                JS_FreeAtom(context, evaluationFileName);
            evaluationLocationUpdated = true;
        }

        StackFrameCandidate candidate;
        candidate.function = candidateFunction;
        candidate.name = candidateNameText;
        candidate.level = level;
        candidate.scriptFunction = candidateNameText != QStringLiteral("<eval>")
            && candidateNameText != QStringLiteral("qtscriptFunction")
            && JS_GetFunctionInfo(context, candidate.function, &candidate.fileName,
                                  &candidate.startLine, &candidate.startColumn,
                                  &candidate.endLine, &candidate.argumentCount) == 0;
        const bool visibleNativeFunction = !candidate.scriptFunction
            && !candidate.name.isEmpty()
            && candidate.name != QStringLiteral("<eval>")
            && candidate.name != QStringLiteral("eval")
            && candidate.name != QStringLiteral("qtscriptFunction")
            && candidate.name != QStringLiteral("qtNativeFunction");
        if (candidate.scriptFunction || (visibleNativeFunction && !hasQtContext))
            candidates.append(candidate);
        else {
            JS_FreeValue(context, candidate.function);
            if (candidate.fileName != JS_ATOM_NULL)
                JS_FreeAtom(context, candidate.fileName);
        }
    }

    // A previous native callback may already have reconstructed outer script
    // frames.  Consume those contexts one-for-one, preserving recursive calls
    // to the same function while avoiding duplicate synthetic frames.
    QVector<QScriptValue> representedFunctions;
    for (QScriptContext *frameContext = state->currentContext;
         frameContext; frameContext = QScriptContextPrivate::get(frameContext)->parent) {
        QScriptContextPrivate *framePrivate = QScriptContextPrivate::get(frameContext);
        if (framePrivate->hiddenFromBacktrace || !framePrivate->callee.isValid())
            continue;
        QScriptValuePrivate *calleePrivate = QScriptValuePrivate::get(framePrivate->callee);
        if (calleePrivate && calleePrivate->state.data() == state)
            representedFunctions.append(framePrivate->callee);
    }
    QVector<bool> representedFunctionUsed(representedFunctions.size(), false);
    for (StackFrameCandidate &candidate : candidates) {
        for (int index = 0; index < representedFunctions.size(); ++index) {
            if (representedFunctionUsed.at(index))
                continue;
            QScriptValuePrivate *representedPrivate =
                QScriptValuePrivate::get(representedFunctions.at(index));
            if (representedPrivate
                && JS_IsStrictEqual(context, candidate.function, representedPrivate->value)) {
                representedFunctionUsed[index] = true;
                candidate.represented = true;
                break;
            }
        }
    }

    QVector<QScriptContext *> syntheticContexts;
    QScriptContext *parentContext = state->currentContext;
    for (int index = candidates.size() - 1; index >= 0; --index) {
        StackFrameCandidate &candidate = candidates[index];
        if (candidate.represented)
            continue;

        JSValue frameThis = JS_GetStackFrameThis(context, candidate.level);
        QScriptValue thisObject = enginePrivate->fromOwned(frameThis);
        QScriptContext *frame = createEngineContext(engine, parentContext,
                                                    thisObject, true);
        QScriptContextPrivate *framePrivate = QScriptContextPrivate::get(frame);
        framePrivate->functionType = candidate.scriptFunction
            ? QScriptContextInfo::ScriptFunction : QScriptContextInfo::NativeFunction;
        framePrivate->callee = enginePrivate->fromOwned(candidate.function);
        candidate.function = JS_UNDEFINED;
        framePrivate->backtraceName = candidate.scriptFunction
            ? (candidate.name.isEmpty() ? QStringLiteral("<anonymous>") : candidate.name)
            : candidate.name;
        framePrivate->lineNumber = candidate.scriptFunction ? candidate.startLine : -1;
        framePrivate->columnNumber = candidate.scriptFunction ? candidate.startColumn : -1;
        if (candidate.scriptFunction) {
            JS_GetStackFrameLocation(context, candidate.level, &framePrivate->lineNumber,
                                     &framePrivate->columnNumber);
            framePrivate->calledAsConstructor =
                JS_IsStackFrameConstructor(context, candidate.level) != 0;
            framePrivate->functionStartLineNumber = candidate.startLine;
            framePrivate->functionEndLineNumber = candidate.endLine;
            if (candidate.fileName != JS_ATOM_NULL)
                framePrivate->fileName = globalAtomName(context, candidate.fileName);
            for (int parameterIndex = 0; parameterIndex < candidate.argumentCount;
                 ++parameterIndex) {
                const JSAtom parameter = JS_GetFunctionParameterName(
                    context, QScriptValuePrivate::get(framePrivate->callee)->value,
                    parameterIndex);
                if (parameter != JS_ATOM_NULL) {
                    framePrivate->parameterNames.append(globalAtomName(context, parameter));
                    JS_FreeAtom(context, parameter);
                }
            }
        }
        const int argumentCount = JS_GetStackFrameArgumentCount(context, candidate.level);
        for (int argumentIndex = 0; argumentIndex < argumentCount; ++argumentIndex) {
            JSValue argument = JS_GetStackFrameArgument(context, candidate.level, argumentIndex);
            framePrivate->arguments.append(enginePrivate->fromOwned(argument));
        }
        if (candidate.scriptFunction) {
            QScriptContext *metadataContext = state->currentContext;
            while (metadataContext) {
                QScriptContextPrivate *metadataPrivate =
                    QScriptContextPrivate::get(metadataContext);
                if (metadataPrivate->scriptId != -1) {
                    framePrivate->scriptId = metadataPrivate->scriptId;
                    if (framePrivate->fileName.isEmpty())
                        framePrivate->fileName = metadataPrivate->fileName;
                    break;
                }
                metadataContext = metadataPrivate->parent;
            }
        }
        syntheticContexts.append(frame);
        parentContext = frame;
    }
    for (StackFrameCandidate &candidate : candidates) {
        if (!JS_IsUndefined(candidate.function))
            JS_FreeValue(context, candidate.function);
        if (candidate.fileName != JS_ATOM_NULL)
            JS_FreeAtom(context, candidate.fileName);
    }

    QScriptContext *scriptContext = createEngineContext(engine, parentContext,
                                                        engine->globalObject(), true);
    QScriptContextPrivate *contextPrivate = QScriptContextPrivate::get(scriptContext);
    contextPrivate->backtraceName = QStringLiteral("<native>");
    contextPrivate->calledAsConstructor = argc > 1 && JS_ToBool(context, argv[1]);
    if (argc > 0)
        contextPrivate->callee = enginePrivate->fromBorrowed(argv[0]);
    if (argc > 2)
        contextPrivate->thisObject = enginePrivate->fromBorrowed(argv[2]);
    for (int index = 3; index < argc; ++index)
        contextPrivate->arguments.append(enginePrivate->fromBorrowed(argv[index]));

    QScriptContext *previousContext = state->currentContext;
    state->currentContext = scriptContext;
    if (state->agent)
        state->agent->contextPush();

    QScriptValue result;
    const QScriptNativeFunction native = it.value();
    if (native.function)
        result = native.function(scriptContext, engine);
    else if (native.functionWithArg)
        result = native.functionWithArg(scriptContext, engine, native.argument);

    if (contextPrivate->returnValue.isValid())
        result = contextPrivate->returnValue;

    const bool threw = contextPrivate->state == QScriptContext::ExceptionState;
    state->lastNativeReturnInvalid = !threw && !result.isValid();
    QScriptValue thrownValue = contextPrivate->thrownValue;
    if (state->agent)
        state->agent->contextPop();
    state->currentContext = previousContext;
    delete scriptContext;
    for (auto contextIterator = syntheticContexts.crbegin();
         contextIterator != syntheticContexts.crend(); ++contextIterator)
        delete *contextIterator;


    bool conversionOk = false;
    JSValue quickResult = enginePrivate->toQuickJS(threw ? thrownValue : result, &conversionOk);
    if (!conversionOk)
        quickResult = JS_UNDEFINED;
    if (threw)
        return JS_Throw(context, quickResult);
    if (state->abortRequested) {
        JS_FreeValue(context, quickResult);
        if (state->abortValueIsError && state->abortValueSet) {
            JSValue exception = JS_DupValue(context, state->abortValue);
            JS_SetUncatchableError(context, exception);
            return JS_Throw(context, exception);
        }
        JSValue interrupted = JS_NewInternalError(context, "interrupted");
        if (!JS_IsException(interrupted))
            JS_SetUncatchableError(context, interrupted);
        return JS_Throw(context, interrupted);
    }
    return quickResult;
}

QString exceptionStack(JSContext *context, JSValueConst exception)
{
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    QString result;
    if (JS_IsException(stack)) {
        discardQuickJSException(context);
    } else if (!JS_IsUndefined(stack)) {
        result = qScriptQuickJSString(context, stack);
    }
    JS_FreeValue(context, stack);
    JSValue backtrace = JS_GetErrorBacktrace(context);
    if (!JS_IsUndefined(backtrace) && !JS_IsException(backtrace)) {
        const QString captured = qScriptQuickJSString(context, backtrace);
        if (result.isEmpty() || captured.count(QLatin1Char('\n'))
            > result.count(QLatin1Char('\n')))
            result = captured;
    }
    JS_FreeValue(context, backtrace);
    return result;
}

struct ExceptionStackInfo
{
    int line = -1;
    QString fileName;
    QStringList backtrace;
};

static ExceptionStackInfo parseExceptionStack(const QString &stack)
{
    ExceptionStackInfo result;
    static const QRegularExpression locationPattern(
        QStringLiteral("^\\s*at\\s+(.+?)\\s*\\((.*):(\\d+):(\\d+)\\)\\s*$"));
    static const QRegularExpression argumentsLocationPattern(
        QStringLiteral("^\\s*at\\s+([^\\s(]+)\\((.*)\\)\\s*\\((.*):(\\d+):(\\d+)\\)\\s*$"));
    static const QRegularExpression anonymousLocationPattern(
        QStringLiteral("^\\s*at\\s+(.*):(\\d+):(\\d+)\\s*$"));
    static const QRegularExpression nativePattern(
        QStringLiteral("^\\s*at\\s+([^\\s(]+)\\s+\\(native\\)\\s*$"));

    for (const QString &line : stack.split(u'\n')) {
        if (line.trimmed().isEmpty())
            continue;

        const QRegularExpressionMatch nativeMatch = nativePattern.match(line);
        if (nativeMatch.hasMatch()) {
            result.backtrace.append(QStringLiteral("<native>() at -1"));
            continue;
        }

        QRegularExpressionMatch match = locationPattern.match(line);
        QString functionName;
        QString argumentText;
        QString fileName;
        int lineNumber = -1;
        match = argumentsLocationPattern.match(line);
        if (match.hasMatch()) {
            functionName = match.captured(1).trimmed();
            argumentText = match.captured(2);
            fileName = match.captured(3);
            lineNumber = match.captured(4).toInt();
        } else if (locationPattern.match(line).hasMatch()) {
            match = locationPattern.match(line);
            functionName = match.captured(1).trimmed();
            fileName = match.captured(2);
            lineNumber = match.captured(3).toInt();
        } else {
            match = anonymousLocationPattern.match(line);
            if (!match.hasMatch())
                continue;
            functionName = QStringLiteral("<global>");
            fileName = match.captured(1);
            lineNumber = match.captured(2).toInt();
        }

        if (functionName == QStringLiteral("qtscriptFunction"))
            continue;
        if (result.line < 0) {
            result.line = lineNumber;
            result.fileName = fileName;
        }
        if (functionName == QStringLiteral("<eval>"))
            functionName = QStringLiteral("<global>");
        const QString frame = argumentText.isNull()
            ? QStringLiteral("%1() at %2:%3").arg(functionName, fileName).arg(lineNumber)
            : QStringLiteral("%1(%2) at %3:%4")
                  .arg(functionName, argumentText, fileName).arg(lineNumber);
        result.backtrace.append(frame);
    }
    return result;
}

static void setExceptionLocation(JSContext *context, JSValueConst exception,
                                 int lineNumber, const QString &fileName)
{
    if (!JS_IsObject(exception))
        return;
    if (lineNumber >= 0
        && JS_SetPropertyStr(context, exception, "lineNumber",
                             JS_NewInt32(context, lineNumber)) < 0)
        discardQuickJSException(context);
    if (!fileName.isEmpty() && fileName != QStringLiteral("<eval>")) {
        const QByteArray utf8 = fileName.toUtf8();
        if (JS_SetPropertyStr(context, exception, "fileName",
                              JS_NewStringLen(context, utf8.constData(),
                                              size_t(utf8.size()))) < 0)
            discardQuickJSException(context);
    }
}

QScriptValue newError(QScriptEngine *engine, QScriptContext::Error error, const QString &text)
{
    const char *constructor = "Error";
    switch (error) {
    case QScriptContext::ReferenceError: constructor = "ReferenceError"; break;
    case QScriptContext::SyntaxError: constructor = "SyntaxError"; break;
    case QScriptContext::TypeError: constructor = "TypeError"; break;
    case QScriptContext::RangeError: constructor = "RangeError"; break;
    case QScriptContext::URIError: constructor = "URIError"; break;
    case QScriptContext::UnknownError: break;
    }

    QScriptEnginePrivate *d = QScriptEnginePrivate::get(engine);
    JSContext *context = d->state->context;
    JSValue global = JS_GetGlobalObject(context);
    JSValue ctor = JS_GetPropertyStr(context, global, constructor);
    JSValue argument = JS_NewStringLen(context, qScriptQuickJSUtf8(text).constData(),
                                       size_t(qScriptQuickJSUtf8(text).size()));
    JSValue value = JS_CallConstructor(context, ctor, 1, &argument);
    JS_FreeValue(context, argument);
    JS_FreeValue(context, ctor);
    JS_FreeValue(context, global);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        return d->fromOwned(exception);
    }
    return d->fromOwned(value);
}

} // unnamed namespace

class QScriptSyntaxCheckResultPrivate : public QSharedData
{
public:
    QScriptSyntaxCheckResult::State state = QScriptSyntaxCheckResult::Valid;
    int line = -1;
    int column = -1;
    QString message;
};

QByteArray qScriptQuickJSUtf8(const QString &value)
{
    return value.toUtf8();
}

QString qScriptQuickJSString(JSContext *context, JSValueConst value)
{
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, value);
    if (!text)
        return QString();
    QString result = QString::fromUtf8(text, qsizetype(length));
    JS_FreeCString(context, text);
    // QtScript exposes native callbacks using the JSC-style source form,
    // while the QuickJS bridge is implemented by a small JavaScript
    // forwarding function.  Check the source text itself: QuickJS does not
    // classify every bridge callback as a regular JS function after it has
    // been wrapped by a host object.
    if (result.contains(QStringLiteral(
            "return native(qtscriptFunction,new.target!==undefined,this,...arguments);")))
        return QStringLiteral("function () {\n    [native code]\n}");
    if (JS_IsFunction(context, value)) {
        if (result.startsWith(QStringLiteral("function(")))
            result.insert(8, QLatin1Char(' '));
        result.replace(QStringLiteral("debugger }"), QStringLiteral("debugger; }"));
    }
    return result;
}

int qScriptQuickJSPropertyFlags(QScriptValue::PropertyFlags flags)
{
    int result = 0;
    if (!(flags & QScriptValue::ReadOnly))
        result |= JS_PROP_WRITABLE;
    if (!(flags & QScriptValue::Undeletable))
        result |= JS_PROP_CONFIGURABLE;
    if (!(flags & QScriptValue::SkipInEnumeration))
        result |= JS_PROP_ENUMERABLE;
    return result;
}

QScriptEngineState::QScriptEngineState()
{
    runtime = JS_NewRuntime();
    if (!runtime)
        return;
    // Keep QuickJS stack exhaustion inside the VM while leaving enough room
    // for the native QtScript bridge to inspect a recursive script stack.
    // This is a backend safety limit, not a replacement for QtScript's
    // abortEvaluation() API.
    JS_SetMaxStackSize(runtime, 512 * 1024);
    context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        runtime = nullptr;
        return;
    }

    JS_SetContextOpaque(context, this);
    JS_SetInterruptHandler(runtime, interruptHandler, this);

    JS_NewClassID(runtime, &variantClassId);
    JSClassDef variantDefinition{};
    variantDefinition.class_name = "QVariant";
    variantDefinition.finalizer = variantFinalizer;
    JS_NewClass(runtime, variantClassId, &variantDefinition);
    JSValue variantPrototype = JS_NewObjectClass(context, variantClassId);
    JS_SetOpaque(variantPrototype, new QScriptVariantPayload{QVariant()});
    JS_SetPropertyStr(context, variantPrototype, "valueOf",
                      JS_NewCFunction2(context,
                                       reinterpret_cast<JSCFunction *>(variantValueOf),
                                       "valueOf", 0, JS_CFUNC_generic, 0));
    JS_SetPropertyStr(context, variantPrototype, "toString",
                      JS_NewCFunction2(context,
                                       reinterpret_cast<JSCFunction *>(variantToString),
                                       "toString", 0, JS_CFUNC_generic, 0));
    JS_SetClassProto(context, variantClassId, variantPrototype);
    qScriptQuickJSRegisterQObjectClass(this);
    qScriptQuickJSRegisterScriptClass(this);
    runtimeGlobal = JS_GetGlobalObject(context);
    // QtScript intentionally hides the ES2015 Symbol global, but custom
    // QScriptClass objects still need the well-known @@hasInstance key.
    JSValue symbol = JS_GetPropertyStr(context, runtimeGlobal, "Symbol");
    if (!JS_IsException(symbol)) {
        JSValue hasInstance = JS_GetPropertyStr(context, symbol, "hasInstance");
        if (!JS_IsException(hasInstance) && JS_IsSymbol(hasInstance))
            symbolHasInstanceAtom = JS_ValueToAtom(context, hasInstance);
        JS_FreeValue(context, hasInstance);
    }
    JS_FreeValue(context, symbol);
    JSValue cppObjectMarker = JS_NewSymbol(context, "QtScriptCppObject", false);
    if (!JS_IsException(cppObjectMarker))
        cppObjectMarkerAtom = JS_ValueToAtom(context, cppObjectMarker);
    JS_FreeValue(context, cppObjectMarker);
    discardQuickJSException(context);
    installQtScriptGlobalFunctions(this);
    installQtScriptHasOwnProperty(this);
    installQtScriptLegacyAccessorHelpers(this);
    static constexpr const char *const modernGlobals[] = {
        "InternalError", "AggregateError", "SuppressedError", "Iterator",
        "queueMicrotask", "Reflect", "Symbol", "DisposableStack", "globalThis",
        "BigInt", "Proxy", "Map", "Set", "WeakMap", "WeakSet", "ArrayBuffer",
        "SharedArrayBuffer", "Uint8ClampedArray", "Int8Array", "Uint8Array",
        "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "BigInt64Array",
        "BigUint64Array", "Float16Array", "Float32Array", "Float64Array", "DataView",
        "Atomics", "Promise", "AsyncDisposableStack", "WeakRef",
        "FinalizationRegistry", "DOMException", "btoa", "atob", "performance"
    };
    for (const char *name : modernGlobals) {
        JSAtom atom = JS_NewAtom(context, name);
        if (atom != JS_ATOM_NULL) {
            JS_DeleteProperty(context, runtimeGlobal, atom, 0);
            discardQuickJSException(context);
            JS_FreeAtom(context, atom);
        }
    }
    logicalGlobal = JS_DupValue(context, runtimeGlobal);
    originalGlobalPrototype = JS_GetPrototype(context, runtimeGlobal);
}

QScriptEngineState::~QScriptEngineState()
{
    shutdown();
    deleteDeferredQObjects();
}

static QMetaEnum enumForMetaType(const QMetaType &type,
                                 const QMetaObject *contextMetaObject = nullptr)
{
    if (!type.isValid() || !type.name())
        return QMetaEnum();
    const QMetaObject *metaObject = type.metaObject();
    if (!metaObject)
        metaObject = contextMetaObject;
    if (!metaObject)
        return QMetaEnum();
    QByteArray name(type.name());
    if (name.startsWith("QFlags<") && name.endsWith('>'))
        name = name.mid(7, name.size() - 8);
    const int separator = name.lastIndexOf("::");
    if (separator >= 0)
        name.remove(0, separator + 2);
    int index = metaObject->indexOfEnumerator(name.constData());
    if (index < 0 && name.endsWith("Flag")) {
        name.chop(4);
        index = metaObject->indexOfEnumerator(name.constData());
    }
    return index >= 0 ? metaObject->enumerator(index) : QMetaEnum();
}

void QScriptEngineState::invalidate()
{
    if (destroying)
        return;
    destroying = true;
    for (const QScriptSignalConnection &connection : std::as_const(signalConnections))
        QObject::disconnect(connection.connection);
    signalConnections.clear();
    for (QScriptClassObjectData *data : std::as_const(scriptClassObjects)) {
        // These values are owned by the class bridge and its finalizer may
        // run only after the context has gone away.  Release them while the
        // context is still valid; invalidate() deliberately clears state so
        // later finalizers cannot call back into the engine.
        data->state = nullptr;
        if (context) {
            if (!JS_IsUndefined(data->originalPrototype)) {
                const JSValue originalPrototype = data->originalPrototype;
                data->originalPrototype = JS_UNDEFINED;
                JS_FreeValue(context, originalPrototype);
            }
            if (!JS_IsUndefined(data->boundObject)) {
                const JSValue boundObject = data->boundObject;
                data->boundObject = JS_UNDEFINED;
                JS_FreeValue(context, boundObject);
            }
        }
    }
    scriptClassObjects.clear();
    qobjectWrappers.clear();
    currentContext = nullptr;
    agent = nullptr;
}

void QScriptEngineState::shutdown()
{
    if (!context && !runtime)
        return;
    invalidate();
    for (const QScriptSignalConnection &connection : std::as_const(signalConnections))
        QObject::disconnect(connection.connection);
    signalConnections.clear();

    if (context) {
        scriptClassObjects.clear();
        qobjectWrappers.clear();
        for (const JSValue value : std::as_const(objectIds))
            JS_FreeValue(context, value);
        objectIds.clear();
        JS_FreeValue(context, logicalGlobal);
        JS_FreeValue(context, runtimeGlobal);
        JS_FreeValue(context, originalGlobalPrototype);
        JS_FreeValue(context, globalBuiltins);
        logicalGlobal = JS_UNDEFINED;
        runtimeGlobal = JS_UNDEFINED;
        originalGlobalPrototype = JS_UNDEFINED;
        globalBuiltins = JS_UNDEFINED;
        if (symbolHasInstanceAtom != JS_ATOM_NULL) {
            JS_FreeAtom(context, symbolHasInstanceAtom);
            symbolHasInstanceAtom = JS_ATOM_NULL;
        }
        if (cppObjectMarkerAtom != JS_ATOM_NULL) {
            JS_FreeAtom(context, cppObjectMarkerAtom);
            cppObjectMarkerAtom = JS_ATOM_NULL;
        }
        clearAbortValue();
        clearException();
        JS_SetContextOpaque(context, nullptr);
        JS_FreeContext(context);
        context = nullptr;
    }
    if (runtime) {
        JS_FreeRuntime(runtime);
        runtime = nullptr;
    }
}

void QScriptEngineState::deleteDeferredQObjects()
{
    while (!deferredQObjectDeletes.isEmpty()) {
        const QPointer<QObject> object = deferredQObjectDeletes.takeFirst();
        if (object)
            delete object;
    }
}

void QScriptEngineState::clearException()
{
    if (context && !JS_IsUndefined(exception))
        JS_FreeValue(context, exception);
    exception = JS_UNDEFINED;
    hasException = false;
    exceptionLine = -1;
    exceptionBacktrace.clear();
}

void QScriptEngineState::rememberException(JSValue exceptionValue, int lineNumber)
{
    clearException();
    exception = exceptionValue;
    hasException = true;
    const QString stack = exceptionStack(context, exception);
    const ExceptionStackInfo stackInfo = parseExceptionStack(stack);
    exceptionLine = lineNumber >= 0 ? lineNumber : stackInfo.line;
    exceptionBacktrace = stackInfo.backtrace;
    setExceptionLocation(context, exception, exceptionLine, stackInfo.fileName);
}

void QScriptEngineState::clearAbortValue()
{
    if (context && abortValueSet)
        JS_FreeValue(context, abortValue);
    abortValue = JS_UNDEFINED;
    abortValueSet = false;
    abortValueIsError = false;
}

QScriptEnginePrivate::QScriptEnginePrivate()
    : state(new QScriptEngineState)
{
}

QScriptEnginePrivate::~QScriptEnginePrivate()
{
    if (!state)
        return;
    state->engine = nullptr;
    state->agent = nullptr;
    state->defaultPrototypes.clear();
    for (auto it = state->typeInfos.begin(); it != state->typeInfos.end(); ++it)
        it->prototype = QScriptValue();
    for (const QScriptSignalConnection &connection : std::as_const(state->signalConnections))
        QObject::disconnect(connection.connection);
    state->signalConnections.clear();
    state->pluginLoaders.clear();
    state->invalidate();
}

void QScriptEnginePrivate::syncGlobalObjectToRuntime()
{
    if (!state->customGlobalObject || JS_IsUndefined(state->logicalGlobal))
        return;
    QStringList current;
    copyGlobalProperties(state.data(), state->logicalGlobal,
                         state->runtimeGlobal, &current);
    removeMissingGlobalProperties(state.data(), state->runtimeGlobal,
                                  state->mirroredGlobalProperties, current);
    state->mirroredGlobalProperties = current;
}

void QScriptEnginePrivate::syncRuntimeGlobalObject()
{
    if (!state->customGlobalObject || JS_IsUndefined(state->logicalGlobal))
        return;
    copyRuntimeGlobalProperties(state.data());
}

QScriptValue QScriptEnginePrivate::fromOwned(JSValue value) const
{
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(state->context);
        state->rememberException(exception);
        return QScriptValuePrivate::toPublic(new QScriptValuePrivate(
            state, JS_DupValue(state->context, state->exception), true));
    }
    return QScriptValuePrivate::toPublic(new QScriptValuePrivate(state, value, true));
}

QScriptValue QScriptEnginePrivate::fromBorrowed(JSValueConst value) const
{
    return QScriptValuePrivate::toPublic(new QScriptValuePrivate(state, JS_DupValue(state->context, value), true));
}

JSValue QScriptEnginePrivate::toQuickJS(const QScriptValue &value, bool *ok) const
{
    QScriptValuePrivate *valuePrivate = QScriptValuePrivate::get(value);
    if (!valuePrivate) {
        if (ok)
            *ok = true;
        return JS_UNDEFINED;
    }
    return valuePrivate->materialize(state, ok);
}

QScriptValue QScriptEnginePrivate::fromVariant(const QVariant &value)
{
    if (!value.isValid())
        return q_func()->undefinedValue();

    if (value.metaType().id() == QMetaType::QVariant)
        return fromVariant(value.value<QVariant>());
    if (value.metaType().id() == qMetaTypeId<QScriptValue>())
        return *static_cast<const QScriptValue *>(value.constData());
    const QMetaType type = value.metaType();
    const auto custom = state->typeInfos.constFind(type.id());
    if (!type.flags().testFlag(QMetaType::PointerToQObject)
        && custom != state->typeInfos.cend() && custom->marshal) {
        QScriptValue result = custom->marshal(q_func(), value.constData());
        const QScriptValue prototype = state->defaultPrototypes.value(type.id());
        if (result.isObject() && prototype.isValid())
            result.setPrototype(prototype);
        return result;
    }
    if (type.flags().testFlag(QMetaType::IsEnumeration))
        return QScriptValue(q_func(), value.toInt());

    switch (type.id()) {
    case QMetaType::Bool: return QScriptValue(q_func(), value.toBool());
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::Short:
    case QMetaType::Int: return QScriptValue(q_func(), value.toInt());
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt: return QScriptValue(q_func(), value.toUInt());
    case QMetaType::Long: return QScriptValue(q_func(), qsreal(value.toLongLong()));
    case QMetaType::ULong: return QScriptValue(q_func(), qsreal(value.toULongLong()));
    case QMetaType::LongLong: return QScriptValue(q_func(), qsreal(value.toLongLong()));
    case QMetaType::ULongLong: return QScriptValue(q_func(), qsreal(value.toULongLong()));
    case QMetaType::Float:
    case QMetaType::Double: return QScriptValue(q_func(), value.toDouble());
    case QMetaType::QChar: return QScriptValue(q_func(), uint(value.toChar().unicode()));
    case QMetaType::QDateTime: return q_func()->newDate(value.toDateTime());
    case QMetaType::QDate:
        return q_func()->newDate(QDateTime(value.toDate(), QTime(0, 0), Qt::LocalTime));
    case QMetaType::QString: return QScriptValue(q_func(), value.toString());
    case QMetaType::QStringList: {
        const QStringList list = value.toStringList();
        QScriptValue array = q_func()->newArray(uint(list.size()));
        for (qsizetype index = 0; index < list.size(); ++index)
            array.setProperty(quint32(index), QScriptValue(q_func(), list.at(index)));
        return array;
    }
    case QMetaType::QVariantList: {
        const QVariantList list = value.toList();
        QScriptValue array = q_func()->newArray(uint(list.size()));
        for (qsizetype index = 0; index < list.size(); ++index)
            array.setProperty(quint32(index), fromVariant(list.at(index)));
        return array;
    }
    case QMetaType::QVariantMap: {
        QScriptValue object = q_func()->newObject();
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            object.setProperty(it.key(), fromVariant(it.value()));
        return object;
    }
    default: break;
    }

    const QByteArray typeName = type.name() ? QByteArray(type.name()) : QByteArray();
    if (typeName == "QObjectList" || typeName == "QList<QObject*>"
        || typeName == "QList<QObject *>") {
        const QObjectList objects = *static_cast<const QObjectList *>(value.constData());
        QScriptValue array = q_func()->newArray(uint(objects.size()));
        for (qsizetype index = 0; index < objects.size(); ++index)
            array.setProperty(quint32(index), q_func()->newQObject(objects.at(index)));
        return array;
    }
    if (typeName == "QList<int>") {
        const QList<int> values = *static_cast<const QList<int> *>(value.constData());
        QScriptValue array = q_func()->newArray(uint(values.size()));
        for (qsizetype index = 0; index < values.size(); ++index)
            array.setProperty(quint32(index), QScriptValue(q_func(), values.at(index)));
        return array;
    }

    if (type.flags().testFlag(QMetaType::PointerToQObject)) {
        QObject *object = *reinterpret_cast<QObject *const *>(value.constData());
        // QObject-valued properties and return values must reuse the existing
        // wrapper.  QtScript users commonly attach compatibility accessors to
        // such wrappers (and compare two references for identity); creating a
        // fresh wrapper for every QVariant conversion loses both behaviours.
        QScriptValue result = q_func()->newQObject(
            object, QScriptEngine::QtOwnership,
            QScriptEngine::PreferExistingWrapperObject);
        // Reusing an existing wrapper bypasses wrapQObject()'s prototype
        // resolution (the wrapper predates the metatype registration), so
        // apply the registered prototype for the object's dynamic class here.
        // This keeps a returned QObject value from being "down-graded" to a
        // less specific base-class prototype.  Only apply it while the wrapper
        // still carries an engine-assigned prototype (the generated class
        // prototype, when no default existed at creation, or the registered
        // QObject* default); a prototype the user assigned explicitly to the
        // wrapper survives later conversions instead of being clobbered.
        if (object && result.isObject()) {
            const QByteArray pointerTypeName =
                QByteArray(object->metaObject()->className()) + '*';
            const QMetaType pointerType = QMetaType::fromName(pointerTypeName);
            QScriptValue prototype = pointerType.isValid()
                ? state->defaultPrototypes.value(pointerType.id())
                : QScriptValue();
            if (!prototype.isValid())
                prototype = state->defaultPrototypes.value(qMetaTypeId<QObject *>());
            bool prototypeOk = false;
            JSValue quickPrototype = toQuickJS(prototype, &prototypeOk);
            if (prototypeOk && JS_IsObject(quickPrototype)) {
                bool wrapperOk = false;
                JSValue wrapper = toQuickJS(result, &wrapperOk);
                if (wrapperOk) {
                    JSValue currentPrototype = JS_GetPrototype(state->context, wrapper);
                    JSValue classPrototype = JS_GetClassProto(state->context,
                                                              state->qobjectClassId);
                    bool engineAssigned = JS_IsStrictEqual(state->context, currentPrototype,
                                                           classPrototype);
                    JS_FreeValue(state->context, classPrototype);
                    if (!engineAssigned) {
                        const QScriptValue basePrototype =
                            state->defaultPrototypes.value(qMetaTypeId<QObject *>());
                        bool baseOk = false;
                        JSValue quickBase = toQuickJS(basePrototype, &baseOk);
                        if (baseOk && JS_IsObject(quickBase)) {
                            engineAssigned = JS_IsStrictEqual(state->context, currentPrototype,
                                                              quickBase);
                            JS_FreeValue(state->context, quickBase);
                        }
                    }
                    JS_FreeValue(state->context, currentPrototype);
                    JS_FreeValue(state->context, wrapper);
                    if (engineAssigned)
                        result.setPrototype(prototype);
                }
            }
            JS_FreeValue(state->context, quickPrototype);
        }
        return result;
    }

    JSValue wrapper = JS_NewObjectClass(state->context, state->variantClassId);
    JS_SetOpaque(wrapper, new QScriptVariantPayload{value});
    QScriptValue result = fromOwned(wrapper);
    const QScriptValue prototype = state->defaultPrototypes.value(type.id());
    if (prototype.isValid())
        result.setPrototype(prototype);
    return result;
}

QScriptValue QScriptEnginePrivate::fromVariantAsVariant(const QVariant &value)
{
    if (!value.isValid())
        return q_func()->undefinedValue();
    QVariant payload = value;
    if (value.metaType().id() == QMetaType::QVariant)
        payload = value.value<QVariant>();
    if (!payload.isValid())
        return q_func()->undefinedValue();

    JSValue wrapper = JS_NewObjectClass(state->context, state->variantClassId);
    JS_SetOpaque(wrapper, new QScriptVariantPayload{payload});
    QScriptValue result = fromOwned(wrapper);
    const QScriptValue prototype = state->defaultPrototypes.value(payload.metaType().id());
    if (prototype.isValid())
        result.setPrototype(prototype);
    return result;
}

bool QScriptEnginePrivate::toVariantType(const QScriptValue &value, QMetaType type,
                                          void *destination,
                                          const QMetaObject *contextMetaObject) const
{
    if (!destination || !type.isValid())
        return false;
    const int typeId = type.id();
    const auto custom = state->typeInfos.constFind(typeId);
    if (custom != state->typeInfos.cend() && custom->demarshal) {
        custom->demarshal(value, destination);
        return !state->engine->hasUncaughtException();
    }

    if (type.id() == qMetaTypeId<QScriptValue>() || type.name() == QByteArray("QScriptValue")) {
        if (value.engine() && value.engine() != state->engine)
            return false;
        *static_cast<QScriptValue *>(destination) = value;
        return true;
    }

    if (type.id() == QMetaType::QVariant) {
        QVariant converted = value.toVariant();
        for (int depth = 0; depth < 8
             && converted.isValid()
             && converted.metaType().id() == QMetaType::QVariant; ++depth) {
            converted = converted.value<QVariant>();
        }
        *static_cast<QVariant *>(destination) = converted;
        return true;
    }

    if (type.id() == QMetaType::VoidStar) {
        if (!value.isNull() && !value.isUndefined())
            return false;
        *static_cast<void **>(destination) = nullptr;
        return true;
    }

    if (type.flags().testFlag(QMetaType::IsPointer)) {
        if (type.flags().testFlag(QMetaType::PointerToQObject)) {
            if (value.isNull() || value.isUndefined()) {
                *static_cast<QObject **>(destination) = nullptr;
                return true;
            }
            if (QObject *object = value.toQObject()) {
                const QMetaObject *targetMetaObject = type.metaObject();
                QObject *convertedObject = targetMetaObject
                    ? targetMetaObject->cast(object) : object;
                if (convertedObject) {
                    *static_cast<QObject **>(destination) = convertedObject;
                    return true;
                }
            }
        }

        QScriptValuePrivate *valuePrivate = QScriptValuePrivate::get(value);
        if (valuePrivate && valuePrivate->state == state
            && valuePrivate->kind == QScriptValuePrivate::QuickJSValue) {
            JSValueConst variantObject = valuePrivate->value;
            auto *payload = qscriptVariantPayload(state.data(), variantObject);
            bool convertedPointer = false;
            void *pointer = nullptr;
            if (payload) {
                if (payload->value.metaType() == type) {
                    pointer = *static_cast<void *const *>(payload->value.constData());
                    convertedPointer = true;
                } else {
                    QByteArray pointeeName(type.name());
                    pointeeName.chop(1);
                    pointeeName = pointeeName.trimmed();
                    if (pointeeName.startsWith("const "))
                        pointeeName.remove(0, 6);
                    const QMetaType pointeeType = QMetaType::fromName(pointeeName);
                    if (pointeeType.isValid()
                        && payload->value.metaType() == pointeeType) {
                        pointer = payload->value.data();
                        convertedPointer = true;
                    }
                }

                if (!convertedPointer) {
                    const QScriptValue targetPrototype =
                        state->defaultPrototypes.value(typeId);
                    bool prototypeMatches = false;
                    bool prototypeOk = false;
                    JSValue quickTargetPrototype = toQuickJS(targetPrototype, &prototypeOk);
                    if (prototypeOk && JS_IsObject(quickTargetPrototype)) {
                        JSValue currentPrototype =
                            JS_GetPrototype(state->context, valuePrivate->value);
                        while (JS_IsObject(currentPrototype)) {
                            if (JS_IsStrictEqual(state->context, currentPrototype,
                                                 quickTargetPrototype)) {
                                prototypeMatches = true;
                                JS_FreeValue(state->context, currentPrototype);
                                break;
                            }
                            if (JS_GetClassID(currentPrototype) == state->variantClassId) {
                                auto *prototypePayload = static_cast<QScriptVariantPayload *>(
                                    JS_GetOpaque(currentPrototype, state->variantClassId));
                                if (prototypePayload
                                    && prototypePayload->value.metaType() == type) {
                                    prototypeMatches = true;
                                    JS_FreeValue(state->context, currentPrototype);
                                    break;
                                }
                            }
                            JSValue nextPrototype =
                                JS_GetPrototype(state->context, currentPrototype);
                            JS_FreeValue(state->context, currentPrototype);
                            currentPrototype = nextPrototype;
                        }
                        if (!prototypeMatches)
                            JS_FreeValue(state->context, currentPrototype);
                    }
                    JS_FreeValue(state->context, quickTargetPrototype);
                    if (prototypeMatches) {
                        if (payload->value.metaType().flags().testFlag(QMetaType::IsPointer))
                            pointer = *static_cast<void *const *>(payload->value.constData());
                        else
                            pointer = payload->value.data();
                        convertedPointer = true;
                    }
                }
            }
            if (convertedPointer) {
                *static_cast<void **>(destination) = pointer;
                return true;
            }
            return false;
        }
    }

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
                if (pointeeName == type.name()) {
                    void *pointer = *static_cast<void *const *>(variant.constData());
                    if (!pointer)
                        return false;
                    type.destruct(destination);
                    type.construct(destination, pointer);
                    return true;
                }
            }
        }
    }

    const QByteArray typeName = type.name() ? QByteArray(type.name()) : QByteArray();
    const bool isEnumLike = type.flags().testFlag(QMetaType::IsEnumeration)
        || (typeName.startsWith("QFlags<") && typeName.endsWith('>'));
    if (isEnumLike) {
        if (value.isString()) {
            const QMetaEnum metaEnum = enumForMetaType(type, contextMetaObject);
            if (metaEnum.isValid()) {
                bool ok = false;
                const int enumValue = metaEnum.keysToValue(
                    value.toString().toLatin1().constData(), &ok);
                if (!ok)
                    return false;
                const QMetaType underlying = type.underlyingType();
                QVariant storage(underlying);
                QScriptValue numeric(state->engine, enumValue);
                if (!toVariantType(numeric, underlying, storage.data()))
                    return false;
                memcpy(destination, storage.constData(),
                       size_t(qMin(type.sizeOf(), underlying.sizeOf())));
                return true;
            }
        }
        QMetaType underlying = type.underlyingType();
        if (!underlying.isValid())
            underlying = QMetaType::fromType<int>();
        QVariant storage(underlying);
        if (!toVariantType(value, underlying, storage.data()))
            return false;
        memcpy(destination, storage.constData(), size_t(qMin(type.sizeOf(), underlying.sizeOf())));
        return true;
    }

    switch (typeId) {
    case QMetaType::Bool: *static_cast<bool *>(destination) = value.toBool(); return true;
    case QMetaType::Char: *static_cast<char *>(destination) = char(value.toInt32()); return true;
    case QMetaType::SChar: *static_cast<signed char *>(destination) = static_cast<signed char>(value.toInt32()); return true;
    case QMetaType::UChar: *static_cast<unsigned char *>(destination) = uchar(value.toUInt32()); return true;
    case QMetaType::Short: *static_cast<short *>(destination) = short(value.toInt32()); return true;
    case QMetaType::UShort: *static_cast<ushort *>(destination) = ushort(value.toUInt32()); return true;
    case QMetaType::Int: *static_cast<int *>(destination) = value.toInt32(); return true;
    case QMetaType::UInt: *static_cast<uint *>(destination) = value.toUInt32(); return true;
    case QMetaType::Long: *static_cast<long *>(destination) = long(value.toInteger()); return true;
    case QMetaType::ULong: *static_cast<ulong *>(destination) = ulong(value.toInteger()); return true;
    case QMetaType::LongLong: *static_cast<qlonglong *>(destination) = qlonglong(value.toInteger()); return true;
    case QMetaType::ULongLong: *static_cast<qulonglong *>(destination) = qulonglong(value.toInteger()); return true;
    case QMetaType::Float: *static_cast<float *>(destination) = float(value.toNumber()); return true;
    case QMetaType::Double: *static_cast<double *>(destination) = value.toNumber(); return true;
    case QMetaType::QChar:
        if (value.isString()) {
            const QString string = value.toString();
            *static_cast<QChar *>(destination) = string.isEmpty() ? QChar() : string.at(0);
        } else {
            *static_cast<QChar *>(destination) = QChar(value.toUInt16());
        }
        return true;
    case QMetaType::QString:
        *static_cast<QString *>(destination) = value.isNull() || value.isUndefined()
            ? QString() : value.toString();
        return true;
    case QMetaType::QObjectStar:
        if (value.isNull() || value.isUndefined()) {
            *static_cast<QObject **>(destination) = nullptr;
            return true;
        }
        if (QObject *object = value.toQObject()) {
            *static_cast<QObject **>(destination) = object;
            return true;
        }
        return false;
    case QMetaType::VoidStar:
        if (!value.isNull())
            return false;
        *static_cast<void **>(destination) = nullptr;
        return true;
    default: break;
    }

    const bool isQObjectList = typeName == "QObjectList"
        || typeName == "QList<QObject*>" || typeName == "QList<QObject *>";
    if (isQObjectList) {
        if (!value.isArray())
            return false;
        const quint32 length = value.property(QStringLiteral("length")).toUInt32();
        QObjectList objects;
        objects.reserve(int(length));
        for (quint32 index = 0; index < length; ++index) {
            const QScriptValue item = value.property(QString::number(index));
            QObject *object = item.isNull() || item.isUndefined() ? nullptr : item.toQObject();
            if (!object && !item.isNull() && !item.isUndefined())
                return false;
            objects.append(object);
        }
        type.destruct(destination);
        type.construct(destination, &objects);
        return true;
    }
    if (typeName == "QList<int>") {
        if (!value.isArray())
            return false;
        const quint32 length = value.property(QStringLiteral("length")).toUInt32();
        QList<int> values;
        values.reserve(int(length));
        for (quint32 index = 0; index < length; ++index)
            values.append(value.property(QString::number(index)).toInt32());
        type.destruct(destination);
        type.construct(destination, &values);
        return true;
    }

    const QVariant variant = value.toVariant();
    if (!variant.isValid())
        return false;
    QVariant converted = variant;
    if (converted.metaType() != type && !converted.convert(type))
        return false;
    type.destruct(destination);
    type.construct(destination, converted.constData());
    return true;
}

QScriptValue QScriptEnginePrivate::createNativeFunction(const QScriptNativeFunction &function,
                                                         const QScriptValue &prototype)
{
    const int id = state->nextFunctionId++;
    state->nativeFunctions.insert(id, function);
    JSContext *context = state->context;
    JSCFunctionType nativeFunction{};
    nativeFunction.generic_magic = nativeFunctionThunk;
    JSValue native = JS_NewCFunction2(context,
        nativeFunction.generic, "qtNativeFunction",
        function.length + 3, JS_CFUNC_generic_magic, id);

    static const char factorySource[] =
        "(function(native){return function qtscriptFunction(){"
        "return native(qtscriptFunction,new.target!==undefined,this,...arguments);};})";
    JSValue factory = JS_Eval(context, factorySource, sizeof(factorySource) - 1,
                              "<qtscript-native-factory>", JS_EVAL_TYPE_GLOBAL);
    JSValue wrapper = JS_IsException(factory) ? JS_EXCEPTION
                                               : JS_Call(context, factory, JS_UNDEFINED, 1, &native);
    JS_FreeValue(context, native);
    JS_FreeValue(context, factory);
    if (JS_IsException(wrapper)) {
        JSValue exception = JS_GetException(context);
        state->rememberException(exception);
        return fromBorrowed(state->exception);
    }

    JS_DefinePropertyValueStr(context, wrapper, "length", JS_NewInt32(context, function.length), 0);
    QScriptValue result = fromOwned(wrapper);
    if (prototype.isValid()) {
        result.setProperty(QStringLiteral("prototype"), prototype,
                           QScriptValue::Undeletable | QScriptValue::SkipInEnumeration);
        QScriptValue prototypeObject = prototype;
        prototypeObject.setProperty(QStringLiteral("constructor"), result,
                              QScriptValue::SkipInEnumeration);
    }
    return result;
}

void QScriptEnginePrivate::_q_objectDestroyed(QObject *object)
{
    if (state->destroying)
        return;
    qScriptQuickJSInvalidateQObjectMethods(state.data(), object);
    state->signalCleanupSenders.remove(object);
    for (auto connection = state->signalConnections.begin();
         connection != state->signalConnections.end();) {
        if (connection->sender == object)
            connection = state->signalConnections.erase(connection);
        else
            ++connection;
    }
    auto it = state->qobjectWrappers.find(object);
    if (it == state->qobjectWrappers.end())
        return;
    state->qobjectWrappers.erase(it);
}

QScriptEngine::QScriptEngine()
    : QObject(*new QScriptEnginePrivate, nullptr)
{
    Q_D(QScriptEngine);
    d->state->engine = this;
    initializeEngineContext(this, d->state.data());
}

QScriptEngine::QScriptEngine(QObject *parent)
    : QObject(*new QScriptEnginePrivate, parent)
{
    Q_D(QScriptEngine);
    d->state->engine = this;
    initializeEngineContext(this, d->state.data());
}

QScriptEngine::QScriptEngine(QScriptEnginePrivate &dd, QObject *parent)
    : QObject(dd, parent)
{
    Q_D(QScriptEngine);
    d->state->engine = this;
    initializeEngineContext(this, d->state.data());
}

QScriptEngine::~QScriptEngine()
{
    Q_D(QScriptEngine);
    while (d->state->currentContext) {
        QScriptContext *context = d->state->currentContext;
        d->state->currentContext = QScriptContextPrivate::get(context)->parent;
        delete context;
    }
}

QScriptValue QScriptEngine::globalObject() const
{
    Q_D(const QScriptEngine);
    return d->fromOwned(JS_DupValue(d->state->context, d->state->logicalGlobal));
}

void QScriptEngine::setGlobalObject(const QScriptValue &object)
{
    Q_D(QScriptEngine);
    bool ok = false;
    JSValue source = d->toQuickJS(object, &ok);
    if (!ok || !JS_IsObject(source)) {
        JS_FreeValue(d->state->context, source);
        return;
    }

    const bool isRuntimeGlobal = JS_IsStrictEqual(d->state->context, source,
                                                  d->state->runtimeGlobal);
    if (isRuntimeGlobal) {
        if (d->state->customGlobalObject) {
            copyGlobalProperties(d->state.data(), d->state->globalBuiltins,
                                 d->state->runtimeGlobal, nullptr);
            JS_SetPrototype(d->state->context, d->state->runtimeGlobal,
                            d->state->originalGlobalPrototype);
            d->state->customGlobalObject = false;
            d->state->mirroredGlobalProperties.clear();
        }
    } else {
        prepareGlobalBuiltinPrototype(d->state.data());
        JSValue sourcePrototype = JS_GetPrototype(d->state->context, source);
        if (JS_IsStrictEqual(d->state->context, sourcePrototype,
                             d->state->runtimeGlobal)) {
            JS_SetPrototype(d->state->context, d->state->runtimeGlobal,
                            d->state->globalBuiltins);
        } else {
            JS_SetPrototype(d->state->context, d->state->globalBuiltins,
                            sourcePrototype);
            JS_SetPrototype(d->state->context, source,
                            d->state->globalBuiltins);
            JS_SetPrototype(d->state->context, d->state->runtimeGlobal,
                            d->state->globalBuiltins);
        }
        JS_FreeValue(d->state->context, sourcePrototype);
        d->state->customGlobalObject = true;
    }
    JS_FreeValue(d->state->context, d->state->logicalGlobal);
    d->state->logicalGlobal = JS_DupValue(d->state->context, source);
    d->syncGlobalObjectToRuntime();
    updateGlobalContexts(this);
    JS_FreeValue(d->state->context, source);
}

QScriptContext *QScriptEngine::currentContext() const
{
    Q_D(const QScriptEngine);
    return d->state->currentContext;
}

QScriptContext *QScriptEngine::pushContext()
{
    Q_D(QScriptEngine);
    QScriptContext *context = createEngineContext(this, d->state->currentContext,
                                                  globalObject(), true);
    QScriptContextPrivate::get(context)->userPushed = true;
    d->state->currentContext = context;
    if (d->state->agent)
        d->state->agent->contextPush();
    return context;
}

void QScriptEngine::popContext()
{
    Q_D(QScriptEngine);
    QScriptContext *context = d->state->currentContext;
    if (!context || !QScriptContextPrivate::get(context)->parent) {
        qWarning("QScriptEngine::popContext() doesn't match with pushContext()");
        return;
    }
    d->state->currentContext = QScriptContextPrivate::get(context)->parent;
    if (d->state->agent)
        d->state->agent->contextPop();
    delete context;
}

bool QScriptEngine::canEvaluate(const QString &program) const
{
    return checkSyntax(program).state() != QScriptSyntaxCheckResult::Intermediate;
}

QScriptSyntaxCheckResult QScriptEngine::checkSyntax(const QString &program)
{
    QScriptSyntaxCheckResult result;
    result.d_ptr = new QScriptSyntaxCheckResultPrivate;
    QScript::SyntaxChecker checker;
    const QScript::SyntaxChecker::Result syntax = checker.checkSyntax(program);
    switch (syntax.state) {
    case QScript::SyntaxChecker::Error:
        result.d_ptr->state = QScriptSyntaxCheckResult::Error;
        break;
    case QScript::SyntaxChecker::Intermediate:
        result.d_ptr->state = QScriptSyntaxCheckResult::Intermediate;
        break;
    case QScript::SyntaxChecker::Valid:
        result.d_ptr->state = QScriptSyntaxCheckResult::Valid;
        break;
    }
    result.d_ptr->line = syntax.errorLineNumber;
    result.d_ptr->column = syntax.errorColumnNumber;
    result.d_ptr->message = syntax.errorMessage;
    return result;
}

QScriptValue QScriptEngine::evaluate(const QString &program, const QString &fileName, int lineNumber)
{
    Q_D(QScriptEngine);
    QScriptEngineState *state = d->state.data();
    state->clearException();
    state->clearAbortValue();
    state->abortRequested = false;
    d->syncGlobalObjectToRuntime();
    JS_UpdateStackTop(state->runtime);
    state->evaluating = true;
    const qint64 previousDeadline = state->evaluationDeadline;
    const qint64 previousProcessEventsDeadline = state->processEventsDeadline;
    state->processEventsDeadline = state->processEventsInterval > 0
        ? QDateTime::currentMSecsSinceEpoch() + state->processEventsInterval : 0;
    bool timeoutOk = false;
    const qint64 timeoutMs = qEnvironmentVariableIntValue("QTSCRIPT_EVAL_TIMEOUT_MS",
                                                          &timeoutOk);
    if (timeoutOk && timeoutMs > 0)
        state->evaluationDeadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    else
        state->evaluationDeadline = 0;
    const qint64 scriptId = state->nextScriptId++;
    if (state->agent)
        state->agent->scriptLoad(scriptId, program, fileName, lineNumber);

    QScriptContext *previousContext = state->currentContext;
    QScriptContext *evaluationParent = previousContext;
    if (previousContext && !QScriptContextPrivate::get(previousContext)->parent)
        evaluationParent = nullptr;
    QScriptContext *evaluationContext = createEngineContext(this, evaluationParent,
                                                             globalObject(), false);
    QScriptContextPrivate *evaluationPrivate = QScriptContextPrivate::get(evaluationContext);
    setEvaluationFrameMetadata(evaluationPrivate, scriptId, fileName, lineNumber);
    evaluationPrivate->sourceCode = program;
    if (evaluationParent)
        evaluationPrivate->backtraceName = QStringLiteral("<eval>");
    state->currentContext = evaluationContext;

    QScriptContextPrivate *previousPrivate = QScriptContextPrivate::get(previousContext);
    const JSValueConst evaluationGlobal = state->runtimeGlobal;
    bool activationIsEvaluationGlobal = false;
    if (previousPrivate) {
        bool activationOk = false;
        JSValue activation = d->toQuickJS(previousPrivate->activationObject, &activationOk);
        activationIsEvaluationGlobal = activationOk
            && JS_IsStrictEqual(state->context, activation, evaluationGlobal);
        JS_FreeValue(state->context, activation);
    }
    const bool parentIsScriptFunction = previousPrivate && previousPrivate->parent
        && QScriptContextPrivate::get(previousPrivate->parent)->functionType
            == QScriptContextInfo::ScriptFunction;
    const bool preserveScriptFunctionGlobals = parentIsScriptFunction
        && !activationIsEvaluationGlobal;
    const bool customActivationScope = previousPrivate && previousPrivate->parent
        && !activationIsEvaluationGlobal && !parentIsScriptFunction
        && (previousPrivate->userPushed || previousPrivate->activationObjectWasSet);

    // A nested evaluation from a native callback still observes that
    // callback's arguments object in QtScript.  QuickJS evaluates this API as
    // global code, so install a temporary global binding and restore the
    // original descriptor immediately after the evaluation.
    JSAtom argumentsAtom = JS_NewAtom(state->context, "arguments");
    JSPropertyDescriptor previousArguments{};
    const int previousArgumentsResult = JS_GetOwnProperty(state->context,
                                                          &previousArguments,
                                                          evaluationGlobal,
                                                          argumentsAtom);
    if (previousArgumentsResult < 0)
        discardQuickJSException(state->context);
    const bool hasPreviousArguments = previousArgumentsResult > 0;
    bool hasTemporaryArguments = false;
    if (previousPrivate && previousPrivate->callee.isValid()) {
        bool argumentsOk = false;
        JSValue nestedArguments = d->toQuickJS(previousContext->argumentsObject(),
                                               &argumentsOk);
        if (argumentsOk) {
            JS_DefinePropertyValue(state->context, evaluationGlobal, argumentsAtom,
                                   nestedArguments, JS_PROP_C_W_E);
            hasTemporaryArguments = true;
        }
    }

    struct TemporaryScopeBinding {
        JSAtom atom = JS_ATOM_NULL;
        JSValue source = JS_UNDEFINED;
        JSPropertyDescriptor previous{};
        bool hadPrevious = false;
    };
    QVector<TemporaryScopeBinding> temporaryScopeBindings;
    QVector<TemporaryScopeBinding> temporaryDeclarationBindings;
    QSet<JSAtom> installedScopeAtoms;
    if (previousContext) {
        const QScriptValue global = globalObject();
        const QScriptValueList scopes = previousContext->scopeChain();
        for (int scopeIndex = scopes.size() - 1; scopeIndex >= 0; --scopeIndex) {
            if (scopes.at(scopeIndex).strictlyEquals(global))
                continue;
            bool scopeOk = false;
            JSValue scope = d->toQuickJS(scopes.at(scopeIndex), &scopeOk);
            if (!scopeOk || !JS_IsObject(scope)) {
                JS_FreeValue(state->context, scope);
                continue;
            }
            if (JS_IsStrictEqual(state->context, scope, evaluationGlobal)) {
                JS_FreeValue(state->context, scope);
                continue;
            }
            JSPropertyEnum *properties = nullptr;
            uint32_t count = 0;
            if (JS_GetOwnPropertyNames(state->context, &properties, &count, scope,
                                       JS_GPN_STRING_MASK) >= 0) {
                for (uint32_t index = 0; index < count; ++index) {
                    const JSAtom atom = properties[index].atom;
                    if (installedScopeAtoms.contains(atom))
                        continue;
                    JSPropertyDescriptor source{};
                    if (JS_GetOwnProperty(state->context, &source, scope, atom) <= 0)
                        continue;
                    TemporaryScopeBinding binding;
                    binding.atom = JS_DupAtom(state->context, atom);
                    binding.source = JS_DupValue(state->context, scope);
                    const int previousResult = JS_GetOwnProperty(
                        state->context, &binding.previous, evaluationGlobal, atom);
                    if (previousResult < 0) {
                        discardQuickJSException(state->context);
                        JS_FreeValue(state->context, binding.previous.value);
                        JS_FreeValue(state->context, binding.previous.getter);
                        JS_FreeValue(state->context, binding.previous.setter);
                        JS_FreeValue(state->context, binding.source);
                        binding.previous = {};
                    } else {
                        binding.hadPrevious = previousResult > 0;
                    }
                    int flags = source.flags & JS_PROP_C_W_E;
                    flags |= JS_PROP_CONFIGURABLE | JS_PROP_HAS_CONFIGURABLE;
                    if (source.flags & JS_PROP_GETSET)
                        flags |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
                    else
                        flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
                    if (JS_DefineProperty(state->context, evaluationGlobal, atom,
                                          source.value, source.getter, source.setter,
                                          flags) > 0) {
                        installedScopeAtoms.insert(atom);
                        temporaryScopeBindings.append(binding);
                    } else {
                        discardQuickJSException(state->context);
                        JS_FreeValue(state->context, binding.previous.value);
                        JS_FreeValue(state->context, binding.previous.getter);
                        JS_FreeValue(state->context, binding.previous.setter);
                        JS_FreeValue(state->context, binding.source);
                    }
                    JS_FreeValue(state->context, source.value);
                    JS_FreeValue(state->context, source.getter);
                    JS_FreeValue(state->context, source.setter);
                }
                JS_FreePropertyEnum(state->context, properties, count);
            } else {
                discardQuickJSException(state->context);
            }
            JS_FreeValue(state->context, scope);
        }
    }

    QByteArray source = normalizeDuplicateRegExpFlags(program.toUtf8());
    if (previousPrivate && previousPrivate->parent) {
        static const QRegularExpression declarationPattern(
            QStringLiteral("(?:^|[;\\n])\\s*(?:var|function)\\s+([A-Za-z_$][A-Za-z0-9_$]*)"));
        QSet<JSAtom> declaredAtoms;
        QRegularExpressionMatchIterator declarations = declarationPattern.globalMatch(
            QString::fromUtf8(source));
        while (declarations.hasNext()) {
            const QByteArray name = declarations.next().captured(1).toUtf8();
            JSAtom atom = JS_NewAtomLen(state->context, name.constData(), size_t(name.size()));
            if (atom == JS_ATOM_NULL || declaredAtoms.contains(atom)) {
                if (atom != JS_ATOM_NULL)
                    JS_FreeAtom(state->context, atom);
                continue;
            }
            declaredAtoms.insert(atom);
            TemporaryScopeBinding binding;
            binding.atom = atom;
            const int previousResult = JS_GetOwnProperty(state->context,
                                                         &binding.previous,
                                                         evaluationGlobal, atom);
            if (previousResult < 0) {
                discardQuickJSException(state->context);
                JS_FreeAtom(state->context, binding.atom);
                continue;
            }
            binding.hadPrevious = previousResult > 0;
            if (!binding.hadPrevious
                && JS_DefinePropertyValue(state->context, evaluationGlobal, atom,
                                           JS_UNDEFINED, JS_PROP_C_W_E) < 0) {
                discardQuickJSException(state->context);
                freeQuickJSDescriptor(state->context, &binding.previous);
                JS_FreeAtom(state->context, binding.atom);
                continue;
            }
            temporaryDeclarationBindings.append(binding);
        }
    }
    predeclareCustomGlobalBindings(state, evaluationGlobal, source);
    const QByteArray name = fileName.isEmpty() ? QByteArrayLiteral("<eval>") : fileName.toUtf8();
    JSEvalOptions options{};
    options.version = JS_EVAL_OPTIONS_VERSION;
    options.eval_flags = JS_EVAL_TYPE_GLOBAL;
    if (previousPrivate && previousPrivate->parent && !activationIsEvaluationGlobal)
        options.eval_flags |= JS_EVAL_FLAG_QTSCRIPT_GLOBAL_FUNCTION_CONFIGURABLE;
    options.filename = name.constData();
    options.line_num = lineNumber;
    JSValue evaluationThis = JS_DupValue(state->context, state->runtimeGlobal);
    if (previousPrivate && previousPrivate->parent) {
        bool thisOk = false;
        JSValue contextThis = d->toQuickJS(previousPrivate->thisObject, &thisOk);
        if (thisOk && JS_IsObject(contextThis)) {
            JS_FreeValue(state->context, evaluationThis);
            evaluationThis = contextThis;
        } else {
            JS_FreeValue(state->context, contextThis);
        }
    }
    QByteArray missingActivationIdentifier;
    if (customActivationScope) {
        const QRegularExpression simpleIdentifierPattern(
            QStringLiteral("^\\s*([A-Za-z_$][A-Za-z0-9_$]*)\\s*$"));
        const QRegularExpressionMatch identifierMatch =
            simpleIdentifierPattern.match(QString::fromUtf8(source));
        if (identifierMatch.hasMatch()) {
            const QByteArray identifier = identifierMatch.captured(1).toUtf8();
            const JSAtom identifierAtom = JS_NewAtomLen(state->context,
                                                        identifier.constData(),
                                                        size_t(identifier.size()));
            bool found = false;
            if (identifierAtom != JS_ATOM_NULL) {
                int hasProperty = JS_HasProperty(state->context, evaluationGlobal,
                                                 identifierAtom);
                if (hasProperty > 0) {
                    found = true;
                } else if (hasProperty < 0) {
                    discardQuickJSException(state->context);
                    found = true;
                }
                if (!found && previousContext) {
                    const QScriptValueList scopes = previousContext->scopeChain();
                    for (const QScriptValue &scopeValue : scopes) {
                        bool scopeOk = false;
                        JSValue scope = d->toQuickJS(scopeValue, &scopeOk);
                        if (!scopeOk || !JS_IsObject(scope)) {
                            JS_FreeValue(state->context, scope);
                            continue;
                        }
                        hasProperty = JS_HasProperty(state->context, scope,
                                                     identifierAtom);
                        JS_FreeValue(state->context, scope);
                        if (hasProperty > 0) {
                            found = true;
                            break;
                        }
                        if (hasProperty < 0) {
                            discardQuickJSException(state->context);
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                    missingActivationIdentifier = identifier;
                JS_FreeAtom(state->context, identifierAtom);
            }
        }
    }
    JSValue value = missingActivationIdentifier.isEmpty()
        ? JS_EvalThis2(state->context, evaluationThis, source.constData(),
                       size_t(source.size()), &options)
        : JS_UNDEFINED;
    JS_FreeValue(state->context, evaluationThis);
    // The cleanup below restores temporary scope bindings with JS property
    // operations.  If the script threw, the pending JS exception makes those
    // operations fail, and the discardQuickJSException() calls consume the
    // genuine exception.  Capture it now so the exception-handling path at the
    // end of this function can still return the real error value.
    JSValue pendingEvalException = JS_IsException(value)
        ? JS_GetException(state->context) : JS_UNDEFINED;
    for (auto it = temporaryDeclarationBindings.crbegin();
         it != temporaryDeclarationBindings.crend(); ++it) {
        const QString declarationName = globalAtomName(state->context, it->atom);
        if (previousContext && !activationIsEvaluationGlobal) {
            JSValue localValue = JS_GetProperty(state->context, evaluationGlobal,
                                                it->atom);
            if (!JS_IsException(localValue)) {
                if (!declarationName.isEmpty()) {
                    const QScriptValue local = d->fromOwned(localValue);
                    previousContext->activationObject().setProperty(declarationName, local);
                } else {
                    JS_FreeValue(state->context, localValue);
                }
            } else {
                discardQuickJSException(state->context);
            }
        }
        JSPropertyDescriptor declarationDescriptor{};
        JS_GetOwnProperty(state->context, &declarationDescriptor,
                          evaluationGlobal, it->atom);
        if (!activationIsEvaluationGlobal && !preserveScriptFunctionGlobals) {
            JS_DeleteProperty(state->context, evaluationGlobal, it->atom, 0);
            discardQuickJSException(state->context);
        }
        freeQuickJSDescriptor(state->context, &declarationDescriptor);
        if (it->hadPrevious && !activationIsEvaluationGlobal
            && !preserveScriptFunctionGlobals)
            defineGlobalDescriptor(state->context, evaluationGlobal, it->atom,
                                   &it->previous);
        JS_FreeValue(state->context, it->previous.value);
        JS_FreeValue(state->context, it->previous.getter);
        JS_FreeValue(state->context, it->previous.setter);
        JS_FreeAtom(state->context, it->atom);
    }
    for (auto it = temporaryScopeBindings.crbegin();
         it != temporaryScopeBindings.crend(); ++it) {
        JSPropertyDescriptor current{};
        const int currentResult = JS_GetOwnProperty(state->context, &current,
                                                     evaluationGlobal, it->atom);
        if (currentResult > 0 && !(current.flags & JS_PROP_GETSET)) {
            if (JS_SetProperty(state->context, it->source, it->atom,
                               JS_DupValue(state->context, current.value)) < 0)
                discardQuickJSException(state->context);
        } else if (currentResult == 0) {
            if (JS_DeleteProperty(state->context, it->source, it->atom, 0) < 0)
                discardQuickJSException(state->context);
        } else if (currentResult < 0) {
            discardQuickJSException(state->context);
        }
        freeQuickJSDescriptor(state->context, &current);
        JS_DeleteProperty(state->context, evaluationGlobal, it->atom, 0);
        if (it->hadPrevious)
            defineGlobalDescriptor(state->context, evaluationGlobal, it->atom,
                                   &it->previous);
        JS_FreeValue(state->context, it->previous.value);
        JS_FreeValue(state->context, it->previous.getter);
        JS_FreeValue(state->context, it->previous.setter);
        JS_FreeValue(state->context, it->source);
        JS_FreeAtom(state->context, it->atom);
    }
    if (hasTemporaryArguments)
        JS_DeleteProperty(state->context, evaluationGlobal, argumentsAtom, 0);
    if (hasPreviousArguments) {
        defineGlobalDescriptor(state->context, evaluationGlobal, argumentsAtom,
                               &previousArguments);
        previousArguments.value = JS_UNDEFINED;
        previousArguments.getter = JS_UNDEFINED;
        previousArguments.setter = JS_UNDEFINED;
    } else {
        JS_DeleteProperty(state->context, evaluationGlobal, argumentsAtom, 0);
    }
    freeQuickJSDescriptor(state->context, &previousArguments);
    JS_FreeAtom(state->context, argumentsAtom);
    state->currentContext = previousContext;
    delete evaluationContext;
    state->evaluating = false;
    state->evaluationDeadline = previousDeadline;
    state->processEventsDeadline = previousProcessEventsDeadline;
    d->syncRuntimeGlobalObject();
    if (!missingActivationIdentifier.isEmpty())
        value = JS_ThrowReferenceError(state->context, "%s is not defined",
                                       missingActivationIdentifier.constData());
    if (!JS_IsUndefined(pendingEvalException))
        value = JS_Throw(state->context, pendingEvalException);

    QScriptValue result;
    if (state->abortRequested) {
        if (JS_IsException(value))
            JS_FreeValue(state->context, JS_GetException(state->context));
        if (state->abortValueSet)
            result = d->fromBorrowed(state->abortValue);
        else
            result = QScriptValue();
        if (!state->abortValueIsError)
            state->clearException();
    } else if (JS_IsException(value)) {
        JSValue exception = JS_GetException(state->context);
        const bool isRememberedException = state->hasException
            && JS_IsStrictEqual(state->context, exception, state->exception);
        if (isRememberedException) {
            const ExceptionStackInfo stackInfo =
                parseExceptionStack(exceptionStack(state->context, state->exception));
            if (!stackInfo.backtrace.isEmpty()) {
                state->exceptionBacktrace = stackInfo.backtrace;
                if (stackInfo.line >= 0) {
                    state->exceptionLine = stackInfo.line;
                    setExceptionLocation(state->context, state->exception,
                                         state->exceptionLine, stackInfo.fileName);
                }
            }
            JS_FreeValue(state->context, exception);
        } else {
            if (state->hasException)
                state->clearException();
            state->rememberException(exception);
            if (state->exceptionLine < 0 && lineNumber >= 0) {
                state->exceptionLine = lineNumber;
                setExceptionLocation(state->context, state->exception, lineNumber, {});
            }
            JSValue messageValue = JS_GetPropertyStr(state->context, state->exception,
                                                     "message");
            const QString message = JS_IsException(messageValue)
                ? QString() : qScriptQuickJSString(state->context, messageValue);
            JS_FreeValue(state->context, messageValue);
            if (message == QStringLiteral("invalid assignment left-hand side")
                && lineNumber > 1 && source.startsWith('\n')) {
                ++state->exceptionLine;
                setExceptionLocation(state->context, state->exception,
                                     state->exceptionLine, {});
            } else if (message == QStringLiteral("invalid assignment left-hand side")
                       && lineNumber == 0 && state->exceptionLine > 0) {
                --state->exceptionLine;
                setExceptionLocation(state->context, state->exception,
                                     state->exceptionLine, {});
            }
        }
        result = d->fromBorrowed(state->exception);
        if (state->agent)
            state->agent->exceptionThrow(scriptId, result, false);
    } else {
        if (state->hasException)
            state->clearException();
        if (JS_IsStrictEqual(state->context, value, evaluationGlobal)) {
            if (previousContext && QScriptContextPrivate::get(previousContext)->callee.isValid()) {
                bool thisOk = false;
                JSValue thisObject = d->toQuickJS(previousContext->thisObject(), &thisOk);
                JS_FreeValue(state->context, value);
                value = thisOk ? thisObject : JS_UNDEFINED;
            } else if (state->customGlobalObject) {
                JS_FreeValue(state->context, value);
                value = JS_DupValue(state->context, state->logicalGlobal);
            }
        }
        result = d->fromOwned(value);
    }
    if (state->agent)
        state->agent->scriptUnload(scriptId);
    return result;
}

QScriptValue QScriptEngine::evaluate(const QScriptProgram &program)
{
    if (program.isNull())
        return QScriptValue();
    return evaluate(program.sourceCode(), program.fileName(), program.firstLineNumber());
}

bool QScriptEngine::isEvaluating() const
{
    Q_D(const QScriptEngine);
    return d->state->evaluating;
}

void QScriptEngine::abortEvaluation(const QScriptValue &value)
{
    Q_D(QScriptEngine);
    QScriptEngineState *state = d->state.data();
    state->abortRequested = true;
    state->clearAbortValue();
    if (!value.isValid())
        return;
    bool conversionOk = false;
    JSValue abortValue = d->toQuickJS(value, &conversionOk);
    if (!conversionOk)
        return;
    state->abortValue = abortValue;
    state->abortValueSet = true;
    state->abortValueIsError = JS_IsError(abortValue);
    if (state->abortValueIsError)
        state->rememberException(JS_DupValue(state->context, abortValue));
}

bool QScriptEngine::hasUncaughtException() const
{
    Q_D(const QScriptEngine);
    return d->state->hasException;
}

QScriptValue QScriptEngine::uncaughtException() const
{
    Q_D(const QScriptEngine);
    return d->state->hasException ? d->fromBorrowed(d->state->exception) : QScriptValue();
}

int QScriptEngine::uncaughtExceptionLineNumber() const
{
    Q_D(const QScriptEngine);
    return d->state->exceptionLine;
}

QStringList QScriptEngine::uncaughtExceptionBacktrace() const
{
    Q_D(const QScriptEngine);
    return d->state->exceptionBacktrace;
}

void QScriptEngine::clearExceptions()
{
    Q_D(QScriptEngine);
    d->state->clearException();
}

QScriptValue QScriptEngine::nullValue() { return QScriptValue(this, QScriptValue::NullValue); }
QScriptValue QScriptEngine::undefinedValue() { return QScriptValue(this, QScriptValue::UndefinedValue); }

QScriptValue QScriptEngine::newFunction(FunctionSignature signature, int length)
{
    Q_D(QScriptEngine);
    QScriptNativeFunction function;
    function.function = signature;
    function.length = length;
    return d->createNativeFunction(function);
}

QScriptValue QScriptEngine::newFunction(FunctionSignature signature,
                                        const QScriptValue &prototype, int length)
{
    Q_D(QScriptEngine);
    QScriptNativeFunction function;
    function.function = signature;
    function.length = length;
    return d->createNativeFunction(function, prototype);
}

QScriptValue QScriptEngine::newFunction(FunctionWithArgSignature signature, void *argument)
{
    Q_D(QScriptEngine);
    QScriptNativeFunction function;
    function.functionWithArg = signature;
    function.argument = argument;
    return d->createNativeFunction(function);
}

QScriptValue QScriptEngine::newVariant(const QVariant &value)
{
    Q_D(QScriptEngine);
    JSValue wrapper = JS_NewObjectClass(d->state->context, d->state->variantClassId);
    if (JS_IsException(wrapper)) {
        JSValue exception = JS_GetException(d->state->context);
        d->state->rememberException(exception);
        return d->fromBorrowed(d->state->exception);
    }
    JS_SetOpaque(wrapper, new QScriptVariantPayload{value});
    QScriptValue result = d->fromOwned(wrapper);
    const QScriptValue prototype = d->state->defaultPrototypes.value(value.metaType().id());
    if (prototype.isValid())
        result.setPrototype(prototype);
    return result;
}

QScriptValue QScriptEngine::newVariant(const QScriptValue &object, const QVariant &value)
{
    Q_D(QScriptEngine);
    if (object.isObject() && (object.isArray() || object.isDate() || object.isRegExp()
                             || object.isFunction() || object.isQObject())) {
        qWarning("QScriptEngine::newVariant(): changing class of non-QScriptObject not supported");
        return QScriptValue();
    }
    QScriptValue result = object.isObject() ? object : newObject();
    QScriptValuePrivate *objectPrivate = QScriptValuePrivate::get(result);
    if (objectPrivate && objectPrivate->state == d->state
        && objectPrivate->kind == QScriptValuePrivate::QuickJSValue
        && JS_GetClassID(objectPrivate->value) == d->state->variantClassId) {
        if (auto *payload = static_cast<QScriptVariantPayload *>(
                JS_GetOpaque(objectPrivate->value, d->state->variantClassId)))
            payload->value = value;
        return result;
    }
    const QScriptValue::PropertyFlags markerFlags =
        result.property(QStringLiteral("__qtscript_variant__")).isValid()
        ? QScriptValue::KeepExistingFlags
        : QScriptValue::Undeletable | QScriptValue::SkipInEnumeration;
    result.setProperty(QStringLiteral("__qtscript_variant__"), newVariant(value),
                       markerFlags);
    const QScriptValue prototype = defaultPrototype(value.metaType().id());
    if (prototype.isValid())
        result.setPrototype(prototype);
    return result;
}

QScriptValue QScriptEngine::newObject()
{
    Q_D(QScriptEngine);
    JSValue object = JS_NewObject(d->state->context);
    if (!JS_IsException(object) && d->state->cppObjectMarkerAtom != JS_ATOM_NULL)
        JS_DefinePropertyValue(d->state->context, object, d->state->cppObjectMarkerAtom,
                               JS_TRUE, 0);
    return d->fromOwned(object);
}

QScriptValue QScriptEngine::newObject(QScriptClass *scriptClass, const QScriptValue &data)
{
    Q_D(QScriptEngine);
    if (!scriptClass)
        return newObject();
    return qScriptQuickJSNewScriptClassObject(d, scriptClass, data);
}

QScriptValue QScriptEngine::newArray(uint length)
{
    Q_D(QScriptEngine);
    JSValue array = JS_NewArray(d->state->context);
    JS_SetLength(d->state->context, array, length);
    return d->fromOwned(array);
}

QScriptValue QScriptEngine::newRegExp(const QString &pattern, const QString &flags)
{
    Q_D(QScriptEngine);
    JSValue global = JS_GetGlobalObject(d->state->context);
    JSValue constructor = JS_GetPropertyStr(d->state->context, global, "RegExp");
    const QByteArray patternUtf8 = pattern.toUtf8();
    QByteArray flagsUtf8;
    for (const QChar flag : flags) {
        if ((flag == u'g' || flag == u'i' || flag == u'm')
            && !flagsUtf8.contains(char(flag.toLatin1())))
            flagsUtf8.append(char(flag.toLatin1()));
    }
    JSValue arguments[] = {
        JS_NewStringLen(d->state->context, patternUtf8.constData(), size_t(patternUtf8.size())),
        JS_NewStringLen(d->state->context, flagsUtf8.constData(), size_t(flagsUtf8.size()))
    };
    JSValue value = JS_CallConstructor(d->state->context, constructor, 2, arguments);
    JS_FreeValue(d->state->context, arguments[0]);
    JS_FreeValue(d->state->context, arguments[1]);
    JS_FreeValue(d->state->context, constructor);
    JS_FreeValue(d->state->context, global);
    return d->fromOwned(value);
}

QScriptValue QScriptEngine::newRegExp(const QRegExp &regexp)
{
    QString pattern = qt_regexp_toCanonical(regexp.pattern(), regexp.patternSyntax());
    if (regexp.isMinimal()) {
        QString minimalPattern;
        minimalPattern.reserve(pattern.size());
        bool inBracket = false;
        for (qsizetype index = 0; index < pattern.size();) {
            const QChar character = pattern.at(index++);
            minimalPattern += character;
            switch (character.unicode()) {
            case '?':
            case '+':
            case '*':
            case '}':
                if (!inBracket)
                    minimalPattern += QLatin1Char('?');
                break;
            case '\\':
                if (index < pattern.size())
                    minimalPattern += pattern.at(index++);
                break;
            case '[':
                inBracket = true;
                break;
            case ']':
                inBracket = false;
                break;
            default:
                break;
            }
        }
        pattern = minimalPattern;
    }
    QString flags;
    if (regexp.caseSensitivity() == Qt::CaseInsensitive)
        flags += u'i';
    return newRegExp(pattern, flags);
}

QScriptValue QScriptEngine::newDate(qsreal value)
{
    Q_D(QScriptEngine);
    return d->fromOwned(JS_NewDate(d->state->context, value));
}

QScriptValue QScriptEngine::newDate(const QDateTime &value)
{
    return newDate(qsreal(value.toMSecsSinceEpoch()));
}

QScriptValue QScriptEngine::newActivationObject() { return newObject(); }

QScriptValue QScriptEngine::newQObject(QObject *object, ValueOwnership ownership,
                                       const QObjectWrapOptions &options)
{
    Q_D(QScriptEngine);
    return d->wrapQObject(QScriptValue(), object, ownership, options);
}

QScriptValue QScriptEngine::newQObject(const QScriptValue &scriptObject, QObject *object,
                                       ValueOwnership ownership, const QObjectWrapOptions &options)
{
    Q_D(QScriptEngine);
    if (scriptObject.isObject()
        && (scriptObject.isArray() || scriptObject.isDate() || scriptObject.isRegExp()
            || scriptObject.isFunction())) {
        qWarning("QScriptEngine::newQObject(): changing class of non-QScriptObject not supported");
        return QScriptValue();
    }
    return d->wrapQObject(scriptObject, object, ownership, options);
}

static QScriptValue qScriptMetaObjectConstructor(QScriptContext *context,
                                                 QScriptEngine *engine, void *argument)
{
    const QMetaObject *metaObject = static_cast<const QMetaObject *>(argument);
    QObject *object = nullptr;
    if (metaObject && metaObject->constructorCount() > 0) {
        QList<QMetaMethod> candidates;
        for (int index = metaObject->constructorCount() - 1; index >= 0; --index)
            candidates.append(metaObject->constructor(index));

        QStringList candidateNames;
        QMetaMethod selected;
        QVector<QVariant> selectedValues;
        QList<QMetaMethod> compatibleCandidates;
        int selectedScore = std::numeric_limits<int>::max();
        bool ambiguous = false;
        bool acceptsArgumentCount = false;
        QScriptEnginePrivate *enginePrivate = QScriptEnginePrivate::get(engine);
        for (const QMetaMethod &method : candidates) {
            candidateNames.append(QString::fromLatin1(method.methodSignature()));
            if (context->argumentCount() < method.parameterCount()
                || method.parameterCount() > 10)
                continue;
            acceptsArgumentCount = true;
            QVector<QVariant> converted;
            converted.reserve(method.parameterCount());
            int score = (context->argumentCount() - method.parameterCount()) * 10;
            bool ok = true;
            for (int index = 0; index < method.parameterCount(); ++index) {
                const QMetaType type = method.parameterMetaType(index);
                if (!type.isValid()) {
                    ok = false;
                    break;
                }
                converted.append(QVariant(type));
                if (!enginePrivate->toVariantType(context->argument(index), type,
                                                   converted.last().data())) {
                    ok = false;
                    break;
                }
                const QScriptValue value = context->argument(index);
                if (type.flags().testFlag(QMetaType::PointerToQObject))
                    score += value.isNull() || value.isUndefined() || value.toQObject() ? 0 : 100;
                else if (type.id() == QMetaType::QString)
                    score += value.isString() ? 0 : 20;
                else if (type.id() == QMetaType::Bool)
                    score += value.isBoolean() ? 0 : 10;
                else if (type.id() == QMetaType::Int || type.id() == QMetaType::UInt
                         || type.id() == QMetaType::Long || type.id() == QMetaType::ULong
                         || type.id() == QMetaType::LongLong
                         || type.id() == QMetaType::ULongLong
                         || type.id() == QMetaType::Short || type.id() == QMetaType::UShort
                         || type.id() == QMetaType::Char || type.id() == QMetaType::SChar
                         || type.id() == QMetaType::UChar || type.id() == QMetaType::Double
                         || type.id() == QMetaType::Float)
                    score += value.isNumber() ? 0 : 20;
                else if (value.isVariant() && value.toVariant().metaType() == type)
                    score += 0;
                else
                    score += 10;
            }
            if (!ok)
                continue;
            compatibleCandidates.append(method);
            if (score < selectedScore) {
                selected = method;
                selectedValues = std::move(converted);
                selectedScore = score;
                ambiguous = false;
            } else if (score == selectedScore
                       && selected.methodSignature() != method.methodSignature()) {
                ambiguous = true;
            }
        }

        auto candidateMessage = [&candidateNames] {
            QString result;
            for (const QString &candidate : candidateNames)
                result += QStringLiteral("\n    ") + candidate;
            return result;
        };
        if (!selected.isValid() || ambiguous) {
            if (!acceptsArgumentCount) {
                return context->throwError(
                    QScriptContext::SyntaxError,
                    QStringLiteral("too few arguments in call to %1(); candidates are%2")
                        .arg(QString::fromLatin1(metaObject->className()), candidateMessage()));
            }
            if (ambiguous) {
                std::sort(compatibleCandidates.begin(), compatibleCandidates.end(),
                          [](const QMetaMethod &left, const QMetaMethod &right) {
                    return QString::fromLatin1(left.methodSignature()).toLower()
                        < QString::fromLatin1(right.methodSignature()).toLower();
                });
                QString message = QStringLiteral(
                    "ambiguous call of overloaded function %1(); candidates were")
                    .arg(QString::fromLatin1(metaObject->className()));
                for (const QMetaMethod &method : compatibleCandidates)
                    message += QStringLiteral("\n    ")
                        + QString::fromLatin1(method.methodSignature());
                return context->throwError(QScriptContext::TypeError, message);
            }
            return context->throwError(
                QScriptContext::TypeError,
                QStringLiteral("incompatible type of argument(s) in call to %1(); candidates were%2")
                    .arg(QString::fromLatin1(metaObject->className()), candidateMessage()));
        }

        const QList<QByteArray> parameterNames = selected.parameterTypes();
        QGenericArgument arguments[10];
        for (int index = 0; index < selected.parameterCount(); ++index)
            arguments[index] = QGenericArgument(parameterNames.at(index).constData(),
                                                selectedValues.at(index).constData());
        object = metaObject->newInstance(arguments[0], arguments[1], arguments[2],
                                          arguments[3], arguments[4], arguments[5],
                                          arguments[6], arguments[7], arguments[8],
                                          arguments[9]);
        if (!object)
            return context->throwError(QScriptContext::TypeError,
                                       QStringLiteral("unable to construct %1")
                                           .arg(QString::fromLatin1(metaObject->className())));
    } else {
        QObject *parent = context->argumentCount() > 0
            ? context->argument(0).toQObject() : nullptr;
        object = new QObject(parent);
    }
    if (context->isCalledAsConstructor())
        return engine->newQObject(context->thisObject(), object,
                                  QScriptEngine::AutoOwnership);
    QScriptValue result = engine->newQObject(object, QScriptEngine::AutoOwnership);
    result.setPrototype(context->callee().property(QStringLiteral("prototype")));
    return result;
}

static QScriptValue qScriptMetaObjectClassName(QScriptContext *, QScriptEngine *, void *argument)
{
    const QMetaObject *metaObject = static_cast<const QMetaObject *>(argument);
    return QScriptValue(metaObject ? metaObject->className() : "");
}

QScriptValue QScriptEngine::newQMetaObject(const QMetaObject *metaObject, const QScriptValue &ctor)
{
    QScriptValue result;
    if (ctor.isFunction()) {
        result = ctor;
    } else if (metaObject && metaObject->constructorCount() > 0) {
        result = newFunction(reinterpret_cast<FunctionWithArgSignature>(
                                 qScriptMetaObjectConstructor),
                             const_cast<QMetaObject *>(metaObject));
    } else {
        result = newObject();
    }
    if (!metaObject)
        return result;
    result.setProperty(QString::fromLatin1(metaObjectProperty),
                       newVariant(QVariant::fromValue(metaObject)),
                       QScriptValue::Undeletable | QScriptValue::SkipInEnumeration);
    result.setProperty(QStringLiteral("className"),
                       newFunction(reinterpret_cast<FunctionWithArgSignature>(
                                       qScriptMetaObjectClassName),
                                   const_cast<QMetaObject *>(metaObject)),
                       QScriptValue::Undeletable | QScriptValue::SkipInEnumeration);
    for (int index = metaObject->enumeratorOffset();
         index < metaObject->enumeratorCount(); ++index) {
        const QMetaEnum enumerator = metaObject->enumerator(index);
        for (int key = 0; key < enumerator.keyCount(); ++key)
            result.setProperty(QString::fromLatin1(enumerator.key(key)),
                               QScriptValue(this, enumerator.value(key)),
                               QScriptValue::ReadOnly | QScriptValue::Undeletable);
    }
    const QMetaObject *qtMetaObject = &Qt::staticMetaObject;
    if (metaObject != qtMetaObject) {
        const QMetaObject *prototypeMetaObject = metaObject->superClass();
        if (!prototypeMetaObject)
            prototypeMetaObject = qtMetaObject;
        if (prototypeMetaObject)
            result.setPrototype(newQMetaObject(prototypeMetaObject));
    }
    return result;
}

QScriptValue QScriptEngine::defaultPrototype(int metaTypeId) const
{
    Q_D(const QScriptEngine);
    return d->state->defaultPrototypes.value(metaTypeId);
}

void QScriptEngine::setDefaultPrototype(int metaTypeId, const QScriptValue &prototype)
{
    Q_D(QScriptEngine);
    if (prototype.isValid())
        d->state->defaultPrototypes.insert(metaTypeId, prototype);
    else
        d->state->defaultPrototypes.remove(metaTypeId);
}

QScriptValue QScriptEngine::create(int typeId, const void *pointer)
{
    Q_D(QScriptEngine);
    if (!pointer)
        return undefinedValue();
    const auto custom = d->state->typeInfos.constFind(typeId);
    if (custom != d->state->typeInfos.cend() && custom->marshal) {
        QScriptValue result = custom->marshal(this, pointer);
        const QScriptValue prototype = d->state->defaultPrototypes.value(typeId);
        if (result.isObject() && prototype.isValid()) {
            result.setPrototype(prototype);
        }
        return result;
    }
    const QMetaType type(typeId);
    if (!type.isValid())
        return undefinedValue();
    switch (type.id()) {
    case QMetaType::QVariant:
        return newVariant(*static_cast<const QVariant *>(pointer));
    case QMetaType::QDateTime:
        return newDate(*static_cast<const QDateTime *>(pointer));
    case QMetaType::QDate:
        return newDate(QDateTime(*static_cast<const QDate *>(pointer), QTime(0, 0),
                                Qt::LocalTime));
    default:
        break;
    }
    if (type.id() == qMetaTypeId<QRegExp>())
        return newRegExp(*static_cast<const QRegExp *>(pointer));
    if (type.flags().testFlag(QMetaType::IsPointer)
        && !*static_cast<void *const *>(pointer))
        return nullValue();
    return d->fromVariant(QVariant(type, pointer));
}

bool QScriptEngine::convert(const QScriptValue &value, int typeId, void *pointer)
{
    Q_D(QScriptEngine);
    return d->toVariantType(value, QMetaType(typeId), pointer);
}

bool QScriptEngine::convertV2(const QScriptValue &value, int typeId, void *pointer)
{
    QScriptValuePrivate *valuePrivate = QScriptValuePrivate::get(value);
    if (!valuePrivate || !pointer)
        return false;
    if (valuePrivate->state && valuePrivate->state->engine)
        return QScriptEnginePrivate::get(valuePrivate->state->engine)
            ->toVariantType(value, QMetaType(typeId), pointer);

    switch (typeId) {
    case QMetaType::Bool: *static_cast<bool *>(pointer) = value.toBool(); return true;
    case QMetaType::Char: *static_cast<char *>(pointer) = char(value.toInt32()); return true;
    case QMetaType::SChar: *static_cast<signed char *>(pointer) = static_cast<signed char>(value.toInt32()); return true;
    case QMetaType::UChar: *static_cast<unsigned char *>(pointer) = uchar(value.toUInt32()); return true;
    case QMetaType::Short: *static_cast<short *>(pointer) = short(value.toInt32()); return true;
    case QMetaType::UShort: *static_cast<ushort *>(pointer) = ushort(value.toUInt32()); return true;
    case QMetaType::Int: *static_cast<int *>(pointer) = value.toInt32(); return true;
    case QMetaType::UInt: *static_cast<uint *>(pointer) = value.toUInt32(); return true;
    case QMetaType::Long: *static_cast<long *>(pointer) = long(value.toInteger()); return true;
    case QMetaType::ULong: *static_cast<ulong *>(pointer) = ulong(value.toInteger()); return true;
    case QMetaType::LongLong: *static_cast<qlonglong *>(pointer) = qlonglong(value.toInteger()); return true;
    case QMetaType::ULongLong: *static_cast<qulonglong *>(pointer) = qulonglong(value.toInteger()); return true;
    case QMetaType::Float: *static_cast<float *>(pointer) = float(value.toNumber()); return true;
    case QMetaType::Double: *static_cast<double *>(pointer) = value.toNumber(); return true;
    case QMetaType::QString:
        *static_cast<QString *>(pointer) = value.isNull() || value.isUndefined()
            ? QString() : value.toString();
        return true;
    case QMetaType::QVariant: *static_cast<QVariant *>(pointer) = value.toVariant(); return true;
    default: return false;
    }
}

void QScriptEngine::registerCustomType(int type, MarshalFunction marshal,
                                       DemarshalFunction demarshal,
                                       const QScriptValue &prototype)
{
    Q_D(QScriptEngine);
    d->state->typeInfos.insert(type, QScriptTypeInfo{marshal, demarshal, prototype});
    if (prototype.isValid())
        setDefaultPrototype(type, prototype);
}

void QScriptEngine::installTranslatorFunctions(const QScriptValue &object)
{
    Q_D(QScriptEngine);
    QScriptValue target = object.isObject() ? object : globalObject();
    const QScriptValue::PropertyFlags hidden = QScriptValue::SkipInEnumeration;

#ifndef QT_NO_TRANSLATION
    target.setProperty(QStringLiteral("qsTranslate"),
                       newFunction(scriptQsTranslate, 5), hidden);
    target.setProperty(QStringLiteral("QT_TRANSLATE_NOOP"),
                       newFunction(scriptQsTranslateNoOp, 2), hidden);
    target.setProperty(QStringLiteral("qsTr"),
                       newFunction(scriptQsTr, 3), hidden);
    target.setProperty(QStringLiteral("QT_TR_NOOP"),
                       newFunction(scriptQsTrNoOp, 1), hidden);
    target.setProperty(QStringLiteral("qsTrId"),
                       newFunction(scriptQsTrId, 2), hidden);
    target.setProperty(QStringLiteral("QT_TRID_NOOP"),
                       newFunction(scriptQsTrIdNoOp, 1), hidden);
#endif

    QScriptValue runtimeGlobal = d->fromBorrowed(d->state->runtimeGlobal);
    QScriptValue stringPrototype = runtimeGlobal.property(QStringLiteral("String"))
        .property(QStringLiteral("prototype"));
    stringPrototype.setProperty(QStringLiteral("arg"),
                                newFunction(scriptStringArg, 1), hidden);
    if (d->state->customGlobalObject)
        JS_SetPrototype(d->state->context, d->state->runtimeGlobal,
                        d->state->globalBuiltins);
}

QScriptValue QScriptEngine::importExtension(const QString &extension)
{
    Q_D(QScriptEngine);
    if (d->state->importedExtensions.contains(extension))
        return undefinedValue();
    QScriptContext *context = currentContext();
    if (!context)
        return newError(this, QScriptContext::UnknownError,
                        QStringLiteral("Unable to import %1: no current context")
                            .arg(extension));

    const QStringList components = extension.split(QLatin1Char('.'));
    QString prefix;
    const QString initFileName = QStringLiteral("__init__.js");
    for (int index = 0; index < components.size(); ++index) {
        if (!prefix.isEmpty())
            prefix += QLatin1Char('.');
        prefix += components.at(index);
        if (d->state->importedExtensions.contains(prefix))
            continue;
        if (d->state->extensionsBeingImported.contains(prefix)) {
            return context->throwError(
                QStringLiteral("recursive import of %1").arg(extension));
        }

        d->state->extensionsBeingImported.insert(prefix);
        QScriptExtensionInterface *interface = nullptr;
        QSharedPointer<QPluginLoader> matchedLoader;
        QString script;
        QString scriptFileName;

        for (QObject *plugin : QPluginLoader::staticInstances()) {
            auto *candidate = qobject_cast<QScriptExtensionInterface *>(plugin);
            if (candidate && candidate->keys().contains(prefix)) {
                interface = candidate;
                break;
            }
        }

        // Extensions may ship their init script as a compiled-in resource
        // (":/qtscriptextension/<prefix>/__init__.js"), typically bundled with
        // a static plugin.  Try that before falling back to the library-path
        // scan.
        if (script.isEmpty()) {
            QString resourcePath = QStringLiteral(":/qtscriptextension");
            for (int component = 0; component <= index; ++component) {
                resourcePath += QLatin1Char('/');
                resourcePath += components.at(component);
            }
            resourcePath += QStringLiteral("/__init__.js");
            QFile resourceFile(resourcePath);
            if (resourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream stream(&resourceFile);
                script = stream.readAll();
                scriptFileName = resourcePath;
            }
        }

        const QStringList libraryPaths = QCoreApplication::libraryPaths();
        for (const QString &libraryPath : libraryPaths) {
            if (interface || !script.isEmpty())
                break;
            const QDir scriptDirectory(QDir(libraryPath).filePath(QStringLiteral("script")));
            if (!scriptDirectory.exists())
                continue;

            const QFileInfoList pluginFiles =
                scriptDirectory.entryInfoList(QDir::Files);
            for (const QFileInfo &pluginFile : pluginFiles) {
                auto loader = QSharedPointer<QPluginLoader>::create(
                    pluginFile.canonicalFilePath());
                auto *candidate = qobject_cast<QScriptExtensionInterface *>(loader->instance());
                if (candidate && candidate->keys().contains(prefix)) {
                    interface = candidate;
                    matchedLoader = loader;
                    break;
                }
            }

            QDir extensionDirectory(scriptDirectory);
            bool exists = true;
            for (int componentIndex = 0;
                 componentIndex <= index && exists; ++componentIndex)
                exists = extensionDirectory.cd(components.at(componentIndex));
            if (exists) {
                const QString candidatePath = extensionDirectory.filePath(initFileName);
                QFile file(candidatePath);
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QTextStream stream(&file);
                    script = stream.readAll();
                    scriptFileName = QFileInfo(candidatePath).canonicalFilePath();
                }
            }
        }

        if (!interface && script.isEmpty()) {
            d->state->extensionsBeingImported.remove(prefix);
            return context->throwError(
                QStringLiteral("Unable to import %1: no such extension").arg(extension));
        }

        QScriptContext *extensionContext = pushContext();
        extensionContext->setThisObject(globalObject());
        extensionContext->activationObject().setProperty(
            QStringLiteral("__extension__"), prefix,
            QScriptValue::ReadOnly | QScriptValue::Undeletable);
        extensionContext->activationObject().setProperty(
            QStringLiteral("__setupPackage__"), newFunction(scriptSetupPackage));
        extensionContext->activationObject().setProperty(
            QStringLiteral("__postInit__"), undefinedValue());

        if (!script.isEmpty()) {
            QScriptValue result = evaluate(script, scriptFileName);
            if (hasUncaughtException()) {
                popContext();
                d->state->extensionsBeingImported.remove(prefix);
                return result;
            }
        }
        if (interface) {
            interface->initialize(prefix, this);
            if (hasUncaughtException()) {
                QScriptValue result = uncaughtException();
                popContext();
                d->state->extensionsBeingImported.remove(prefix);
                return result;
            }
            if (matchedLoader)
                d->state->pluginLoaders.append(matchedLoader);
        }

        QScriptValue postInit = extensionContext->activationObject().property(
            QStringLiteral("__postInit__"));
        if (postInit.isFunction()) {
            postInit.call(globalObject());
            if (hasUncaughtException()) {
                QScriptValue result = uncaughtException();
                popContext();
                d->state->extensionsBeingImported.remove(prefix);
                return result;
            }
        }

        popContext();
        d->state->importedExtensions.append(prefix);
        d->state->extensionsBeingImported.remove(prefix);
    }
    return undefinedValue();
}

QStringList QScriptEngine::availableExtensions() const
{
    QSet<QString> result;
    const QString initFileName = QStringLiteral("__init__.js");
    for (QObject *plugin : QPluginLoader::staticInstances()) {
        if (auto *interface = qobject_cast<QScriptExtensionInterface *>(plugin)) {
            for (const QString &key : interface->keys())
                result.insert(key);
        }
    }
    for (const QString &libraryPath : QCoreApplication::libraryPaths()) {
        const QDir scriptDirectory(QDir(libraryPath).filePath(QStringLiteral("script")));
        if (!scriptDirectory.exists())
            continue;
        for (const QFileInfo &pluginFile : scriptDirectory.entryInfoList(QDir::Files)) {
            QPluginLoader loader(pluginFile.canonicalFilePath());
            if (auto *interface = qobject_cast<QScriptExtensionInterface *>(loader.instance())) {
                for (const QString &key : interface->keys())
                    result.insert(key);
            }
        }

        QList<QFileInfo> directories =
            scriptDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        while (!directories.isEmpty()) {
            const QFileInfo directoryInfo = directories.takeLast();
            const QDir directory(directoryInfo.canonicalFilePath());
            if (directory.exists(initFileName)) {
                const QString relative = QDir(scriptDirectory).relativeFilePath(
                    directory.canonicalPath());
                result.insert(QDir::fromNativeSeparators(relative).split(QLatin1Char('/'))
                                  .join(QLatin1Char('.')));
                directories.append(directory.entryInfoList(
                    QDir::Dirs | QDir::NoDotAndDotDot));
            }
        }
    }
    QStringList extensions = result.values();
    std::sort(extensions.begin(), extensions.end());
    return extensions;
}

QStringList QScriptEngine::importedExtensions() const
{
    Q_D(const QScriptEngine);
    return d->state->importedExtensions;
}

void QScriptEngine::collectGarbage()
{
    Q_D(QScriptEngine);
    JS_RunGC(d->state->runtime);
    d->state->deleteDeferredQObjects();
}

void QScriptEngine::reportAdditionalMemoryCost(int) {}

void QScriptEngine::setProcessEventsInterval(int interval)
{
    Q_D(QScriptEngine);
    d->state->processEventsInterval = interval;
    d->state->processEventsDeadline = 0;
}

int QScriptEngine::processEventsInterval() const
{
    Q_D(const QScriptEngine);
    return d->state->processEventsInterval;
}

void QScriptEngine::setAgent(QScriptEngineAgent *agent)
{
    Q_D(QScriptEngine);
    if (agent && agent->engine() != this) {
        qWarning("QScriptEngine::setAgent(): cannot set agent belonging to different engine");
        return;
    }
    d->state->agent = agent;
}

QScriptEngineAgent *QScriptEngine::agent() const
{
    Q_D(const QScriptEngine);
    return d->state->agent;
}

QScriptString QScriptEngine::toStringHandle(const QString &string)
{
    return QScriptStringPrivate::create(this, string);
}

QScriptValue QScriptEngine::toObject(const QScriptValue &value)
{
    Q_D(QScriptEngine);
    if (value.engine() && value.engine() != this) {
        qWarning("QScriptEngine::toObject() failed: cannot convert a value created in a different engine");
        return QScriptValue();
    }
    if (!value.isValid() || value.isNull() || value.isUndefined())
        return QScriptValue();
    if (value.isObject())
        return value;
    bool ok = false;
    JSValue primitive = d->toQuickJS(value, &ok);
    if (!ok)
        return QScriptValue();
    JSValue object = JS_ToObject(d->state->context, primitive);
    JS_FreeValue(d->state->context, primitive);
    if (JS_IsException(object))
        return d->fromOwned(object);
    return d->fromOwned(object);
}

QScriptValue QScriptEngine::objectById(qint64 id) const
{
    Q_D(const QScriptEngine);
    if (!d->state || !d->state->context || id < 0)
        return QScriptValue();
    const auto it = d->state->objectIds.constFind(id);
    return it == d->state->objectIds.cend()
        ? QScriptValue() : d->fromBorrowed(it.value());
}

QScriptSyntaxCheckResult::QScriptSyntaxCheckResult() = default;
QScriptSyntaxCheckResult::QScriptSyntaxCheckResult(QScriptSyntaxCheckResultPrivate *d) : d_ptr(d) {}
QScriptSyntaxCheckResult::QScriptSyntaxCheckResult(const QScriptSyntaxCheckResult &) = default;
QScriptSyntaxCheckResult::~QScriptSyntaxCheckResult() = default;
QScriptSyntaxCheckResult &QScriptSyntaxCheckResult::operator=(const QScriptSyntaxCheckResult &) = default;
QScriptSyntaxCheckResult::State QScriptSyntaxCheckResult::state() const { return d_ptr ? d_ptr->state : Error; }
int QScriptSyntaxCheckResult::errorLineNumber() const { return d_ptr ? d_ptr->line : -1; }
int QScriptSyntaxCheckResult::errorColumnNumber() const { return d_ptr ? d_ptr->column : -1; }
QString QScriptSyntaxCheckResult::errorMessage() const { return d_ptr ? d_ptr->message : QString(); }

QScriptProgram::QScriptProgram() = default;
QScriptProgram::QScriptProgram(const QString &source, const QString fileName, int firstLine)
    : d_ptr(new QScriptProgramPrivate)
{
    d_ptr->sourceCode = source;
    d_ptr->fileName = fileName;
    d_ptr->firstLineNumber = firstLine;
}
QScriptProgram::QScriptProgram(const QScriptProgram &) = default;
QScriptProgram::~QScriptProgram() = default;
QScriptProgram &QScriptProgram::operator=(const QScriptProgram &) = default;
bool QScriptProgram::isNull() const { return !d_ptr; }
QString QScriptProgram::sourceCode() const { return d_ptr ? d_ptr->sourceCode : QString(); }
QString QScriptProgram::fileName() const { return d_ptr ? d_ptr->fileName : QString(); }
int QScriptProgram::firstLineNumber() const { return d_ptr ? d_ptr->firstLineNumber : 1; }
bool QScriptProgram::operator==(const QScriptProgram &other) const
{
    return sourceCode() == other.sourceCode() && fileName() == other.fileName()
        && firstLineNumber() == other.firstLineNumber();
}
bool QScriptProgram::operator!=(const QScriptProgram &other) const { return !(*this == other); }

QScriptString::QScriptString() = default;
QScriptString::QScriptString(const QScriptString &) = default;
QScriptString::~QScriptString() = default;
QScriptString &QScriptString::operator=(const QScriptString &) = default;
bool QScriptString::isValid() const { return d_ptr && !d_ptr->engine.isNull(); }
bool QScriptString::operator==(const QScriptString &other) const
{
    if (!isValid() || !other.isValid())
        return isValid() == other.isValid();
    return d_ptr->engine == other.d_ptr->engine && d_ptr->string == other.d_ptr->string;
}
bool QScriptString::operator!=(const QScriptString &other) const { return !(*this == other); }
quint32 QScriptString::toArrayIndex(bool *ok) const
{
    const QString value = toString();
    bool converted = !value.isEmpty() && (value == QStringLiteral("0")
                                           || (value.at(0) >= u'1' && value.at(0) <= u'9'));
    for (qsizetype index = 1; converted && index < value.size(); ++index)
        converted = value.at(index) >= u'0' && value.at(index) <= u'9';
    bool parsed = false;
    const quint64 number = converted ? value.toULongLong(&parsed) : 0;
    converted = converted && parsed && number <= 0xfffffffeu;
    if (ok)
        *ok = converted;
    return converted ? quint32(number) : 0xffffffffu;
}
QString QScriptString::toString() const { return isValid() ? d_ptr->string : QString(); }
QScriptString::operator QString() const { return toString(); }
uint qHash(const QScriptString &key) { return qHash(key.toString()); }

QScriptEngineAgent::QScriptEngineAgent(QScriptEngine *engine)
    : d_ptr(new QScriptEngineAgentPrivate(engine))
{
    d_ptr->q_ptr = this;
}
QScriptEngineAgent::QScriptEngineAgent(QScriptEngineAgentPrivate &dd, QScriptEngine *engine)
    : d_ptr(&dd)
{
    d_ptr->q_ptr = this;
    d_ptr->engine = engine;
}
QScriptEngineAgent::~QScriptEngineAgent()
{
    if (d_ptr->engine && d_ptr->engine->agent() == this)
        d_ptr->engine->setAgent(nullptr);
}
void QScriptEngineAgent::scriptLoad(qint64, const QString &, const QString &, int) {}
void QScriptEngineAgent::scriptUnload(qint64) {}
void QScriptEngineAgent::contextPush() {}
void QScriptEngineAgent::contextPop() {}
void QScriptEngineAgent::functionEntry(qint64) {}
void QScriptEngineAgent::functionExit(qint64, const QScriptValue &) {}
void QScriptEngineAgent::positionChange(qint64, int, int) {}
void QScriptEngineAgent::exceptionThrow(qint64, const QScriptValue &, bool) {}
void QScriptEngineAgent::exceptionCatch(qint64, const QScriptValue &) {}
bool QScriptEngineAgent::supportsExtension(Extension) const { return false; }
QVariant QScriptEngineAgent::extension(Extension, const QVariant &) { return QVariant(); }
QScriptEngine *QScriptEngineAgent::engine() const { return d_ptr->engine; }

QT_END_NAMESPACE

#include "moc_qscriptengine.cpp"
