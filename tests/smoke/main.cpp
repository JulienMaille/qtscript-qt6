#include <QtCore/QDebug>
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

static bool check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        qCritical().noquote() << "FAIL:" << message;
    }
    return condition;
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

    engine.globalObject().setProperty(QStringLiteral("observed"), -1);
    const QScriptValue callback = engine.evaluate("(function(value) { observed = value; })");
    ok &= check(qScriptConnect(&probe, SIGNAL(valueChanged(int)), QScriptValue(), callback),
                "signal connection");
    probe.setValue(42);
    ok &= check(engine.globalObject().property(QStringLiteral("observed")).toInt32() == 42,
                "signal delivery");

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
