#include <QtCore/QDebug>
#include <QtCore/QMetaType>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QVariant>
#include <QtScript/QRegExp>
#include <QtScript/QScriptEngine>
#include <QtScript/QScriptValue>
#include <QtScriptTools/QScriptEngineDebugger>
#include <QtWidgets/QApplication>
#include <cstdio>

class Probe final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(Mode mode READ mode WRITE setMode)

public:
    enum Mode {
        Disabled = 0,
        Answer = 42
    };
    Q_ENUM(Mode)

    using QObject::QObject;

    int value() const { return m_value; }

    void setValue(int value)
    {
        if (m_value == value)
            return;
        m_value = value;
        emit valueChanged(value);
    }

    Q_INVOKABLE int doubled() const { return m_value * 2; }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode) { m_mode = mode; }
    Q_INVOKABLE Mode echoMode(Mode mode) const { return mode; }

signals:
    void valueChanged(int value);

private:
    int m_value = 0;
    Mode m_mode = Disabled;
};

class Worker final : public QThread
{
    Q_OBJECT

signals:
    void valueReady(int value);

protected:
    void run() override { emit valueReady(84); }
};

static bool check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        qCritical().noquote() << "FAIL:" << message;
    }
    return condition;
}

static QScriptValue nestedEvaluate(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() != 1 || !context->argument(0).isString())
        return context->throwError("nestedEvaluate() expects one script string");
    return engine->evaluate(context->argument(0).toString());
}

static QScriptValue inspectRegexpVariant(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() != 1)
        return context->throwError("inspectRegexpVariant() expects one value");
    const QScriptValue argument = context->argument(0);
    const bool recognizedVariant = argument.isVariant();
    const QVariant value = argument.toVariant();
    const bool isRegexp = recognizedVariant && value.isValid()
        && value.metaType().id() == qMetaTypeId<QRegExp>()
        && value.value<QRegExp>().exactMatch(QStringLiteral("qt6"));
    return QScriptValue(engine, isRegexp);
}

static QScriptValue variantMarkerGetter(QScriptContext *, QScriptEngine *engine)
{
    QScriptValue global = engine->globalObject();
    const int calls = global.property(QStringLiteral("nestedMarkerGetterCalls")).toInt32();
    global.setProperty(QStringLiteral("nestedMarkerGetterCalls"), calls + 1);
    // A getter that re-enters the engine is exactly the kind of callback that
    // native argument conversion must not invoke while resolving a wrapper.
    engine->evaluate(QStringLiteral("1 + 1"));
    return engine->newVariant(QVariant::fromValue(QRegExp(QStringLiteral("qt[0-9]+"))));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QScriptEngine engine;
    bool ok = true;

    ok &= check(engine.evaluate("6 * 7").toInt32() == 42, "expression evaluation");

    QScriptValue function = engine.evaluate("(function(a, b) { return a + b; })");
    QScriptValueList arguments;
    arguments << QScriptValue(19) << QScriptValue(23);
    ok &= check(function.call(QScriptValue(), arguments).toInt32() == 42, "function call");

    const QScriptValue exception = engine.evaluate("throw new Error('boom')");
    ok &= check(engine.hasUncaughtException(), "exception state");
    ok &= check(exception.toString().contains("boom"), "exception value");
    engine.clearExceptions();

    const QVariant payload = QStringLiteral("variant-payload");
    ok &= check(engine.newVariant(payload).toVariant().toString() == payload.toString(),
                "QVariant round trip");

    const QRegExp expression(QStringLiteral("qt[0-9]+"));
    const QVariant regexpVariant = QVariant::fromValue(expression);
    const QRegExp roundTrip = engine.newVariant(regexpVariant).toVariant().value<QRegExp>();
    ok &= check(roundTrip.exactMatch(QStringLiteral("qt6")), "QRegExp round trip");
    ok &= check(QRegExp(QStringLiteral("*.js"), Qt::CaseInsensitive,
                        QRegExp::Wildcard).exactMatch(QStringLiteral("ENGINE.JS")),
                "QRegExp wildcard compatibility");
    QRegExp captures(QStringLiteral("(qt)([0-9]+)"));
    ok &= check(captures.indexIn(QStringLiteral("use qt69 here")) == 4
                    && captures.cap(1) == QStringLiteral("qt")
                    && captures.cap(2) == QStringLiteral("69"),
                "QRegExp captures");
    ok &= check(QRegExp(QStringLiteral("a.b"), Qt::CaseSensitive,
                        QRegExp::FixedString).exactMatch(QStringLiteral("a.b")),
                "QRegExp fixed-string compatibility");

    // Nested evaluation must preserve the native payload identity of a
    // marker-backed wrapper.  The second object also verifies that probing a
    // non-wrapper marker never invokes an accessor (which could re-enter the
    // engine while QuickJS is already converting a native argument).
    engine.globalObject().setProperty(QStringLiteral("nestedEvaluate"),
                                      engine.newFunction(nestedEvaluate));
    engine.globalObject().setProperty(QStringLiteral("inspectRegexpVariant"),
                                      engine.newFunction(inspectRegexpVariant));
    const QScriptValue nestedVariant = engine.newVariant(
        engine.newObject(), regexpVariant);
    engine.globalObject().setProperty(QStringLiteral("nestedVariant"), nestedVariant);
    const QScriptValue nestedResult = engine.evaluate(
        QStringLiteral("nestedEvaluate('inspectRegexpVariant(nestedVariant)')"));
    ok &= check(nestedResult.toBoolean() && !engine.hasUncaughtException(),
                "nested evaluation preserves QVariant payloads");
    engine.clearExceptions();

    engine.globalObject().setProperty(QStringLiteral("nestedMarkerGetterCalls"), 0);
    QScriptValue markerTrap = engine.newObject();
    markerTrap.setProperty(QStringLiteral("__qtscript_variant__"),
                           engine.newFunction(variantMarkerGetter),
                           QScriptValue::PropertyGetter | QScriptValue::ReadOnly
                               | QScriptValue::SkipInEnumeration);
    engine.globalObject().setProperty(QStringLiteral("markerTrap"), markerTrap);
    const QScriptValue trapResult = engine.evaluate(
        QStringLiteral("nestedEvaluate('inspectRegexpVariant(markerTrap)')"));
    ok &= check(!trapResult.toBoolean() && !engine.hasUncaughtException()
                    && engine.globalObject().property(
                           QStringLiteral("nestedMarkerGetterCalls")).toInt32() == 0,
                "nested conversion does not invoke marker accessors");
    engine.clearExceptions();
    QRegExp unixWildcard(QStringLiteral("a\\*b"), Qt::CaseSensitive,
                         QRegExp::WildcardUnix);
    ok &= check(unixWildcard.exactMatch(QStringLiteral("a*b"))
                    && !unixWildcard.exactMatch(QStringLiteral("axxb")),
                "QRegExp Unix wildcard escaping");
    QRegExp caretAlternatives(QStringLiteral("^foo|bar"));
    const int caretWontMatch = caretAlternatives.indexIn(
        QStringLiteral("xxbar"), 0, QRegExp::CaretWontMatch);
    const int caretAtOffset = caretAlternatives.indexIn(
        QStringLiteral("xxbar"), 1, QRegExp::CaretAtOffset);
    ok &= check(caretWontMatch == 2 && caretAtOffset == 2,
                "QRegExp caret modes preserve alternatives");
    QRegExp regexp(QStringLiteral("a.*b"));
    ok &= check(regexp.exactMatch(QStringLiteral("aXXbYYb"))
                    && regexp.matchedLength() == 7,
                "QRegExp greedy matching");
    QRegExp regexp2(QStringLiteral("a.*b"), Qt::CaseSensitive, QRegExp::RegExp2);
    ok &= check(regexp2.exactMatch(QStringLiteral("aXXbYYb"))
                    && regexp2.matchedLength() == 7,
                "QRegExp RegExp2 greedy matching");
    regexp2.setMinimal(true);
    ok &= check(!regexp2.exactMatch(QStringLiteral("aXXbYYb"))
                    && regexp2.matchedLength() == 4,
                "QRegExp minimal matching");
    QRegExp matchState(QStringLiteral("qt"));
    matchState.indexIn(QStringLiteral("qt"));
    ok &= check(matchState.indexIn(QStringLiteral("none")) == -1
                    && matchState.matchedLength() == -1,
                "QRegExp failed-match state");

    Probe probe;
    engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));
    ok &= check(engine.evaluate("probe.value = 21; probe.doubled()").toInt32() == 42,
                "QObject property and invokable exposure");
    engine.evaluate("probe.mode = 42");
    ok &= check(!engine.hasUncaughtException() && probe.mode() == Probe::Answer,
                "QObject enum property write");
    ok &= check(engine.evaluate("probe.mode").toInt32() == 42,
                "QObject enum property read");
    const QScriptValue enumResult = engine.evaluate("probe.echoMode(probe.mode)");
    if (enumResult.toInt32() != 42)
        std::fprintf(stderr, "enum invocation result: type=%s value=%d text=%s\n",
                     enumResult.isUndefined() ? "undefined" :
                     enumResult.isError() ? "error" : "other",
                     enumResult.toInt32(), enumResult.toString().toUtf8().constData());
    if (engine.hasUncaughtException())
        qCritical().noquote() << "QObject enum invocation exception:"
                              << engine.uncaughtException().toString();
    ok &= check(enumResult.toInt32() == 42 && !engine.hasUncaughtException(),
                "QObject enum invokable conversion");
    engine.clearExceptions();

    // Inherited assignment (0029): writing a writable QObject property
    // through a prototype-inheriting receiver must invoke the property setter
    // instead of creating a shadowing own data property.
    Probe inheritedProbe;
    engine.globalObject().setProperty(
        QStringLiteral("protoProbe"), engine.newQObject(
            &inheritedProbe, QScriptEngine::QtOwnership));
    const QScriptValue chainResult = engine.evaluate(
        QStringLiteral("chain = {}; chain.__proto__ = protoProbe;"
                       "chain.value = 21; chain.hasOwnProperty('value')"));
    ok &= check(!engine.hasUncaughtException()
                    && inheritedProbe.value() == 21
                    && chainResult.toBoolean() == false,
                "inherited property assignment invokes setter without shadowing");
    ok &= check(engine.evaluate(QStringLiteral("chain.value")).toInt32() == 21,
                "inherited property read through the wrapper chain");
    engine.clearExceptions();

    // Customized wrappers (0030): a prototype the user assigned explicitly to
    // a QObject wrapper must survive a later conversion of the same object
    // instead of being replaced by a registered default prototype.
    Probe reusedProbe;
    QScriptValue customPrototype = engine.newObject();
    customPrototype.setProperty(QStringLiteral("customized"), true);
    QScriptValue customizedWrapper = engine.newQObject(
        &reusedProbe, QScriptEngine::QtOwnership);
    customizedWrapper.setPrototype(customPrototype);
    const QScriptValue reusedAfterConversion =
        qScriptValueFromValue(&engine, static_cast<Probe *>(&reusedProbe));
    ok &= check(reusedAfterConversion.isObject()
                    && reusedAfterConversion.strictlyEquals(customizedWrapper)
                    && reusedAfterConversion.prototype().strictlyEquals(customPrototype),
                "user-assigned wrapper prototype survives conversion");

    // A wrapper created before the metatype registration still gets upgraded
    // (0030): its prototype is the engine-assigned class prototype, which the
    // conversion is allowed to replace with the registered dynamic-class
    // prototype so the returned QObject value is not down-graded.
    Probe upgradableProbe;
    const QScriptValue earlyWrapper = engine.newQObject(
        &upgradableProbe, QScriptEngine::QtOwnership);
    QScriptValue registeredPrototype = engine.newObject();
    registeredPrototype.setProperty(QStringLiteral("dynamicClass"), true);
    engine.setDefaultPrototype(qMetaTypeId<Probe *>(), registeredPrototype);
    const QScriptValue upgraded = qScriptValueFromValue(
        &engine, static_cast<Probe *>(&upgradableProbe));
    ok &= check(upgraded.isObject()
                    && upgraded.strictlyEquals(earlyWrapper)
                    && upgraded.prototype().strictlyEquals(registeredPrototype),
                "registered dynamic-class prototype applies to reused wrapper");
    engine.clearExceptions();

    engine.globalObject().setProperty(QStringLiteral("observed"), -1);
    const QScriptValue callback = engine.evaluate("(function(value) { observed = value; })");
    ok &= check(qScriptConnect(&probe, SIGNAL(valueChanged(int)), QScriptValue(), callback),
                "signal connection");
    probe.setValue(42);
    ok &= check(engine.globalObject().property(QStringLiteral("observed")).toInt32() == 42,
                "signal delivery");

    Worker worker;
    engine.globalObject().setProperty(QStringLiteral("observed"), -1);
    ok &= check(qScriptConnect(&worker, SIGNAL(valueReady(int)), QScriptValue(), callback),
                "cross-thread signal connection");
    worker.start();
    worker.wait();
    for (int attempt = 0; attempt < 100
         && engine.globalObject().property(QStringLiteral("observed")).toInt32() != 84;
         ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    ok &= check(engine.globalObject().property(QStringLiteral("observed")).toInt32() == 84,
                "cross-thread signal delivery");

    QPointer<Probe> ownedProbe = new Probe;
    QScriptValue firstOwned = engine.newQObject(
        ownedProbe.data(), QScriptEngine::ScriptOwnership);
    QScriptValue sameOwned = engine.newQObject(
        ownedProbe.data(), QScriptEngine::ScriptOwnership,
        QScriptEngine::PreferExistingWrapperObject);
    ok &= check(firstOwned.strictlyEquals(sameOwned),
                "PreferExistingWrapperObject identity before GC");
    engine.collectGarbage();
    ok &= check(!ownedProbe.isNull(), "live ScriptOwnership wrapper survives GC");

    QScriptValue secondOwned = engine.newQObject(
        ownedProbe.data(), QScriptEngine::ScriptOwnership,
        QScriptEngine::ExcludeSuperClassProperties);
    engine.collectGarbage();
    ok &= check(!ownedProbe.isNull(), "multiple ScriptOwnership wrappers survive GC");
    firstOwned = QScriptValue();
    sameOwned = QScriptValue();
    engine.collectGarbage();
    ok &= check(!ownedProbe.isNull(), "last ScriptOwnership wrapper controls deletion");
    secondOwned = QScriptValue();
    engine.evaluate("gc()");
    ok &= check(!engine.hasUncaughtException() && ownedProbe.isNull(),
                "ScriptOwnership object is deleted after the last wrapper");

    QScriptEngineDebugger debugger;
    debugger.attachTo(&engine);
    ok &= check(debugger.action(QScriptEngineDebugger::ContinueAction) != nullptr,
                "ScriptTools action creation");
    ok &= check(debugger.widget(QScriptEngineDebugger::ConsoleWidget) != nullptr,
                "ScriptTools widget creation");
    debugger.detach();

    if (ok)
        qInfo().noquote() << "QtScript smoke test passed";
    return ok ? 0 : 1;
}

#include "main.moc"
