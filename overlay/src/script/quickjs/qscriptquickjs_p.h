/****************************************************************************
**
** QuickJS-NG backend implementation details for QtScript.
** This file is not part of the public QtScript API.
**
****************************************************************************/

#ifndef QSCRIPTQUICKJS_P_H
#define QSCRIPTQUICKJS_P_H

#include "private/qobject_p.h"

#include <QtCore/qdatetime.h>
#include <QtCore/qhash.h>
#include <QtCore/qpointer.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qshareddata.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qset.h>
#include <QtCore/qvariant.h>
#include <QtCore/qvector.h>

#include <QtScript/qscriptcontext.h>
#include <QtScript/qscriptcontextinfo.h>
#include <QtScript/qscriptengine.h>
#include <QtScript/qscriptengineagent.h>
#include <QtScript/qscriptprogram.h>
#include <QtScript/qscriptstring.h>
#include <QtScript/qscriptvalue.h>

extern "C" {
#include <quickjs.h>
}

QT_BEGIN_NAMESPACE

class QPluginLoader;
class QScriptClass;
class QScriptEngineState;

struct QScriptClassObjectData
{
    QScriptEngineState *state = nullptr;
    QScriptClass *scriptClass = nullptr;
    JSValue originalPrototype = JS_UNDEFINED;
    JSValue boundObject = JS_UNDEFINED;
    bool prototypeBinding = false;
};

struct QScriptTypeInfo
{
    QScriptEngine::MarshalFunction marshal = nullptr;
    QScriptEngine::DemarshalFunction demarshal = nullptr;
    QScriptValue prototype;
};

struct QScriptNativeFunction
{
    QScriptEngine::FunctionSignature function = nullptr;
    QScriptEngine::FunctionWithArgSignature functionWithArg = nullptr;
    void *argument = nullptr;
    int length = 0;
};

struct QScriptSignalConnection
{
    QPointer<QObject> sender;
    int signalIndex = -1;
    qint64 callbackId = -1;
    qint64 receiverId = -1;
    QMetaObject::Connection connection;
};

struct QScriptQObjectWrapper
{
    // This is a non-owning identity entry.  The JSValue is kept only while
    // the corresponding QObject wrapper is reachable from QuickJS.
    JSValue value = JS_UNDEFINED;
    QScriptEngine::ValueOwnership ownership = QScriptEngine::QtOwnership;
    QScriptEngine::QObjectWrapOptions options;
};

class QScriptEngineState
{
public:
    QScriptEngineState();
    ~QScriptEngineState();

    void invalidate();
    void shutdown();
    void deleteDeferredQObjects();

    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    QPointer<QScriptEngine> engine;

    JSClassID variantClassId = 0;
    JSClassID qobjectClassId = 0;
    JSClassID metaObjectClassId = 0;
    JSClassID scriptClassClassId = 0;
    JSAtom cppObjectMarkerAtom = JS_ATOM_NULL;
    JSAtom symbolHasInstanceAtom = JS_ATOM_NULL;

    bool evaluating = false;
    bool abortRequested = false;
    JSValue abortValue = JS_UNDEFINED;
    bool abortValueSet = false;
    bool abortValueIsError = false;
    bool lastNativeReturnInvalid = false;
    bool destroying = false;
    qint64 evaluationDeadline = 0;
    bool hasException = false;
    int exceptionLine = -1;
    QStringList exceptionBacktrace;
    JSValue exception = JS_UNDEFINED;

    int processEventsInterval = -1;
    qint64 processEventsDeadline = 0;
    bool processingEvents = false;
    int nextFunctionId = 1;
    qint64 nextScriptId = 1;
    QHash<int, QScriptNativeFunction> nativeFunctions;
    QHash<int, QScriptTypeInfo> typeInfos;
    QHash<qint64, JSValue> objectIds;
    QHash<int, QScriptValue> defaultPrototypes;
    QHash<QObject *, QList<QScriptQObjectWrapper>> qobjectWrappers;
    QList<QPointer<QObject>> deferredQObjectDeletes;
    QList<QScriptClassObjectData *> scriptClassObjects;
    QList<QScriptSignalConnection> signalConnections;
    QSet<QObject *> signalCleanupSenders;
    QList<QSharedPointer<QPluginLoader>> pluginLoaders;
    QStringList importedExtensions;
    QSet<QString> extensionsBeingImported;

    QScriptContext *currentContext = nullptr;
    QScriptEngineAgent *agent = nullptr;

    JSValue runtimeGlobal = JS_UNDEFINED;
    JSValue logicalGlobal = JS_UNDEFINED;
    JSValue originalGlobalPrototype = JS_UNDEFINED;
    JSValue globalBuiltins = JS_UNDEFINED;
    QStringList mirroredGlobalProperties;
    bool customGlobalObject = false;

    void clearException();
    void clearAbortValue();
    void rememberException(JSValue exceptionValue, int lineNumber = -1);
};

struct QScriptVariantPayload
{
    QVariant value;
};

// Return the native payload carried by a QtScript variant wrapper without
// invoking JavaScript property getters or walking its prototype chain.  The
// result is borrowed from the supplied object (or its own marker property).
QScriptVariantPayload *qscriptVariantPayload(QScriptEngineState *state,
                                              JSValueConst value);

class QScriptValuePrivate : public QSharedData
{
public:
    enum Kind {
        Invalid,
        DetachedUndefined,
        DetachedNull,
        DetachedBoolean,
        DetachedNumber,
        DetachedString,
        QuickJSValue
    };

    QScriptValuePrivate();
    explicit QScriptValuePrivate(Kind kind);
    explicit QScriptValuePrivate(bool value);
    explicit QScriptValuePrivate(qsreal value);
    explicit QScriptValuePrivate(const QString &value);
    QScriptValuePrivate(const QSharedPointer<QScriptEngineState> &state, JSValue value,
                        bool adoptValue);
    ~QScriptValuePrivate();

    static QScriptValuePrivate *get(const QScriptValue &value)
    {
        return value.d_ptr.data();
    }

    static QScriptValue toPublic(QScriptValuePrivate *value)
    {
        return QScriptValue(value);
    }

    JSValue materialize(const QSharedPointer<QScriptEngineState> &target, bool *ok = nullptr) const;
    bool belongsTo(const QSharedPointer<QScriptEngineState> &target) const;

    Kind kind = Invalid;
    QSharedPointer<QScriptEngineState> state;
    JSValue value = JS_UNDEFINED;
    bool booleanValue = false;
    qsreal numberValue = 0;
    QString stringValue;
};

class QScriptContextPrivate
{
    Q_DECLARE_PUBLIC(QScriptContext)
public:
    static QScriptContext *create();
    static QScriptContextPrivate *get(QScriptContext *context)
    {
        return context ? context->d_func() : nullptr;
    }

    QScriptContext *q_ptr = nullptr;
    QScriptEngine *engine = nullptr;
    QScriptContext *parent = nullptr;
    QScriptContext::ExecutionState state = QScriptContext::NormalState;
    QScriptContextInfo::FunctionType functionType = QScriptContextInfo::NativeFunction;
    QScriptValue callee;
    QScriptValue thisObject;
    QScriptValue activationObject;
    QScriptValue returnValue;
    QScriptValue thrownValue;
    QScriptValueList arguments;
    QScriptValue argumentsObjectCache;
    QScriptValueList scopes;
    bool calledAsConstructor = false;
    bool userPushed = false;
    bool activationObjectWasSet = false;
    bool hiddenFromBacktrace = false;
    qint64 scriptId = -1;
    int lineNumber = -1;
    int columnNumber = -1;
    QString fileName;
    QString sourceCode;
    int functionStartLineNumber = -1;
    int functionEndLineNumber = -1;
    QString backtraceName;
    int functionMetaIndex = -1;
    QStringList parameterNames;
};

class QScriptEnginePrivate : public QObjectPrivate
{
    Q_DECLARE_PUBLIC(QScriptEngine)
public:
    QScriptEnginePrivate();
    ~QScriptEnginePrivate() override;

    static QScriptEnginePrivate *get(QScriptEngine *engine)
    {
        return engine ? engine->d_func() : nullptr;
    }

    static const QScriptEnginePrivate *get(const QScriptEngine *engine)
    {
        return engine ? engine->d_func() : nullptr;
    }

    QScriptValue fromOwned(JSValue value) const;
    QScriptValue fromBorrowed(JSValueConst value) const;
    JSValue toQuickJS(const QScriptValue &value, bool *ok = nullptr) const;
    QScriptValue fromVariant(const QVariant &value);
    QScriptValue fromVariantAsVariant(const QVariant &value);
    bool toVariantType(const QScriptValue &value, QMetaType type, void *destination,
                       const QMetaObject *contextMetaObject = nullptr) const;

    QScriptValue createNativeFunction(const QScriptNativeFunction &function,
                                      const QScriptValue &prototype = QScriptValue());
    QScriptValue wrapQObject(const QScriptValue &base, QObject *object,
                             QScriptEngine::ValueOwnership ownership,
                             QScriptEngine::QObjectWrapOptions options);
    void populateQObject(JSValueConst wrapper, QObject *object,
                         QScriptEngine::QObjectWrapOptions options);
    void _q_objectDestroyed(QObject *object);
    void syncGlobalObjectToRuntime();
    void syncRuntimeGlobalObject();

    QSharedPointer<QScriptEngineState> state;
};

void qScriptQuickJSRegisterScriptClass(QScriptEngineState *state);
QScriptValue qScriptQuickJSNewScriptClassObject(QScriptEnginePrivate *engine,
                                                QScriptClass *scriptClass,
                                                const QScriptValue &data);
QScriptClass *qScriptQuickJSScriptClass(const QScriptValue &value);
void qScriptQuickJSSetScriptClass(QScriptValue &value, QScriptClass *scriptClass);
bool qScriptQuickJSOwnPropertyShadowsClass(const QScriptValue &value,
                                           const QString &propertyName);

class QScriptProgramPrivate : public QSharedData
{
public:
    QString sourceCode;
    QString fileName;
    int firstLineNumber = 1;
};

class QScriptStringPrivate : public QSharedData
{
public:
    static QScriptString create(QScriptEngine *engine, const QString &string)
    {
        QScriptString result;
        result.d_ptr = new QScriptStringPrivate(engine, string);
        return result;
    }

    explicit QScriptStringPrivate(QScriptEngine *owner, const QString &value)
        : engine(owner), string(value) {}
    QPointer<QScriptEngine> engine;
    QString string;
};

class QScriptEngineAgentPrivate
{
    Q_DECLARE_PUBLIC(QScriptEngineAgent)
public:
    explicit QScriptEngineAgentPrivate(QScriptEngine *scriptEngine = nullptr)
        : engine(scriptEngine)
    {
    }

    QScriptEngineAgent *q_ptr = nullptr;
    QPointer<QScriptEngine> engine;
};

QString qScriptQuickJSString(JSContext *context, JSValueConst value);
QByteArray qScriptQuickJSUtf8(const QString &value);
int qScriptQuickJSPropertyFlags(QScriptValue::PropertyFlags flags);
void qScriptQuickJSRegisterQObjectClass(QScriptEngineState *state);
void qScriptQuickJSInvalidateQObjectMethods(QScriptEngineState *state, QObject *object);

QT_END_NAMESPACE

#endif // QSCRIPTQUICKJS_P_H
