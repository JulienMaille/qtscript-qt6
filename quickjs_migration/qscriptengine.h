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
    void *jsContext() const { return ctx; }

private:
    void *rt;
    void *ctx;
    bool m_hasException;
    QScriptValue exceptionVal;

    static thread_local void *s_activeContext;
    static unsigned int s_qvariant_class_id;
};

bool Q_SCRIPT_EXPORT qScriptConnect(QObject *sender, const char *signal, const QScriptValue &thisObject, const QScriptValue &callback);

#endif
