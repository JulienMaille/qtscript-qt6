#ifndef QSCRIPTENGINE_H
#define QSCRIPTENGINE_H

#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>
#include <QtScript/qtscriptexports.h>
#include "qscriptvalue.h"

class Q_SCRIPT_EXPORT QScriptEngine : public QObject {
    Q_OBJECT
public:
    QScriptEngine();
    virtual ~QScriptEngine();

    QScriptValue evaluate(const QString &program);

    bool hasUncaughtException() const;
    void clearExceptions();

    QScriptValue globalObject() const;

    QScriptValue newVariant(const QVariant &payload);
    QScriptValue newQObject(QObject *object);

    static void *activeContext();
    static unsigned int variantClassId();
    static unsigned int qobjectClassId();
    void *jsContext() const { return ctx; }

private:
    void *rt;
    void *ctx;
    bool m_hasException;
    bool m_qobjectClassRegistered;
    QScriptValue exceptionVal;
};

bool Q_SCRIPT_EXPORT qScriptConnect(QObject *sender, const char *signal, const QScriptValue &thisObject, const QScriptValue &callback);

#endif
