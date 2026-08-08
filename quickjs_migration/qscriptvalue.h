#ifndef QSCRIPTVALUE_H
#define QSCRIPTVALUE_H

#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>
#include <QtCore/qlist.h>
#include <QtCore/qsharedpointer.h>
#include <QtScript/qtscriptexports.h>

class QScriptEngine;
class QScriptValue;
class QScriptValuePrivate;
typedef QList<QScriptValue> QScriptValueList;

class Q_SCRIPT_EXPORT QScriptValue {
public:
    QScriptValue();
    QScriptValue(bool val);
    QScriptValue(int val);
    QScriptValue(double val);
    QScriptValue(const QString &val);
    QScriptValue(const char *val);

    QScriptValue(const QScriptValue &other);
    QScriptValue &operator=(const QScriptValue &other);
    ~QScriptValue();

    bool isValid() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isObject() const;
    bool isUndefined() const;
    bool isNull() const;
    bool isError() const;

    bool toBool() const;
    double toNumber() const;
    int toInt32() const;
    QString toString() const;
    QVariant toVariant() const;

    QScriptValue call(const QScriptValue &thisObject = QScriptValue(), const QScriptValueList &args = QScriptValueList());

    void setProperty(const QString &name, const QScriptValue &value);
    QScriptValue property(const QString &name) const;

    QSharedPointer<QScriptValuePrivate> d_ptr() const { return d; }

private:
    friend class QScriptEngine;
    // Internal constructor
    QScriptValue(QScriptValuePrivate *pPrivate);

    QSharedPointer<QScriptValuePrivate> d;
};

#endif
