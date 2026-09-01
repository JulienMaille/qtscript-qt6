/****************************************************************************
**
** QScriptContextInfo support for the QuickJS-NG backend.
**
****************************************************************************/

#include "qscriptquickjs_p.h"

#include <QtScript/qscriptcontextinfo.h>

#ifndef QT_NO_DATASTREAM
#include <QtCore/qdatastream.h>
#endif

QT_BEGIN_NAMESPACE

class QScriptContextInfoPrivate : public QSharedData
{
    Q_DECLARE_PUBLIC(QScriptContextInfo)

public:
    QScriptContextInfoPrivate() = default;
    explicit QScriptContextInfoPrivate(const QScriptContext *context);
    ~QScriptContextInfoPrivate() = default;

    qint64 scriptId = -1;
    int lineNumber = -1;
    int columnNumber = -1;
    QString fileName;

    QString functionName;
    QScriptContextInfo::FunctionType functionType = QScriptContextInfo::NativeFunction;

    int functionStartLineNumber = -1;
    int functionEndLineNumber = -1;
    int functionMetaIndex = -1;

    QStringList parameterNames;
    QScriptContextInfo *q_ptr = nullptr;
};

QScriptContextInfoPrivate::QScriptContextInfoPrivate(const QScriptContext *context)
{
    if (!context)
        return;

    QScriptContextPrivate *contextPrivate =
        QScriptContextPrivate::get(const_cast<QScriptContext *>(context));
    if (!contextPrivate)
        return;

    scriptId = contextPrivate->scriptId;
    lineNumber = contextPrivate->lineNumber;
    columnNumber = contextPrivate->columnNumber;
    fileName = contextPrivate->fileName;
    functionStartLineNumber = contextPrivate->functionStartLineNumber;
    functionEndLineNumber = contextPrivate->functionEndLineNumber;
    functionType = contextPrivate->functionType;
    functionMetaIndex = contextPrivate->functionMetaIndex;
    parameterNames = contextPrivate->parameterNames;

    const QScriptValue callee = contextPrivate->callee;
    if (!callee.isValid() || !callee.isFunction()) {
        if (functionType == QScriptContextInfo::QtFunction
            || functionType == QScriptContextInfo::QtPropertyFunction)
            functionName = contextPrivate->backtraceName;
        return;
    }

    // Native functions created by this backend are wrapped in a helper whose
    // implementation name is qtscriptFunction.  That helper name is not the
    // user-visible function name and must not leak through QScriptContextInfo.
    const QScriptValue name = callee.property(QStringLiteral("name"));
    if (name.isString() && name.toString() != QStringLiteral("qtscriptFunction"))
        functionName = name.toString();
    if (functionName.isEmpty()
        && (functionType == QScriptContextInfo::QtFunction
            || functionType == QScriptContextInfo::QtPropertyFunction))
        functionName = contextPrivate->backtraceName;
}

QScriptContextInfo::QScriptContextInfo(const QScriptContext *context)
    : d_ptr(context ? new QScriptContextInfoPrivate(context) : nullptr)
{
    if (d_ptr)
        d_ptr->q_ptr = this;
}

QScriptContextInfo::QScriptContextInfo(const QScriptContextInfo &other)
    : d_ptr(other.d_ptr)
{
}

QScriptContextInfo::QScriptContextInfo() = default;
QScriptContextInfo::~QScriptContextInfo() = default;

QScriptContextInfo &QScriptContextInfo::operator=(const QScriptContextInfo &other)
{
    d_ptr = other.d_ptr;
    return *this;
}

bool QScriptContextInfo::isNull() const
{
    Q_D(const QScriptContextInfo);
    return !d;
}

qint64 QScriptContextInfo::scriptId() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->scriptId : -1;
}

QString QScriptContextInfo::fileName() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->fileName : QString();
}

int QScriptContextInfo::lineNumber() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->lineNumber : -1;
}

int QScriptContextInfo::columnNumber() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->columnNumber : -1;
}

QString QScriptContextInfo::functionName() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->functionName : QString();
}

QScriptContextInfo::FunctionType QScriptContextInfo::functionType() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->functionType : NativeFunction;
}

QStringList QScriptContextInfo::functionParameterNames() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->parameterNames : QStringList();
}

int QScriptContextInfo::functionStartLineNumber() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->functionStartLineNumber : -1;
}

int QScriptContextInfo::functionEndLineNumber() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->functionEndLineNumber : -1;
}

int QScriptContextInfo::functionMetaIndex() const
{
    Q_D(const QScriptContextInfo);
    return d ? d->functionMetaIndex : -1;
}

bool QScriptContextInfo::operator==(const QScriptContextInfo &other) const
{
    Q_D(const QScriptContextInfo);
    const QScriptContextInfoPrivate *otherPrivate = other.d_func();
    if (d == otherPrivate)
        return true;
    if (!d || !otherPrivate)
        return false;
    return d->scriptId == otherPrivate->scriptId
        && d->lineNumber == otherPrivate->lineNumber
        && d->columnNumber == otherPrivate->columnNumber
        && d->fileName == otherPrivate->fileName
        && d->functionName == otherPrivate->functionName
        && d->functionType == otherPrivate->functionType
        && d->functionStartLineNumber == otherPrivate->functionStartLineNumber
        && d->functionEndLineNumber == otherPrivate->functionEndLineNumber
        && d->functionMetaIndex == otherPrivate->functionMetaIndex
        && d->parameterNames == otherPrivate->parameterNames;
}

bool QScriptContextInfo::operator!=(const QScriptContextInfo &other) const
{
    return !(*this == other);
}

#ifndef QT_NO_DATASTREAM
QDataStream &operator<<(QDataStream &out, const QScriptContextInfo &info)
{
    out << info.scriptId();
    out << qint32(info.lineNumber());
    out << qint32(info.columnNumber());
    out << quint32(info.functionType());
    out << qint32(info.functionStartLineNumber());
    out << qint32(info.functionEndLineNumber());
    out << qint32(info.functionMetaIndex());
    out << info.fileName();
    out << info.functionName();
    out << info.functionParameterNames();
    return out;
}

QDataStream &operator>>(QDataStream &in, QScriptContextInfo &info)
{
    if (!info.d_ptr)
        info.d_ptr = new QScriptContextInfoPrivate;

    in >> info.d_ptr->scriptId;

    qint32 line = -1;
    in >> line;
    info.d_ptr->lineNumber = line;

    qint32 column = -1;
    in >> column;
    info.d_ptr->columnNumber = column;

    quint32 functionType = quint32(QScriptContextInfo::NativeFunction);
    in >> functionType;
    info.d_ptr->functionType = QScriptContextInfo::FunctionType(functionType);

    qint32 startLine = -1;
    in >> startLine;
    info.d_ptr->functionStartLineNumber = startLine;

    qint32 endLine = -1;
    in >> endLine;
    info.d_ptr->functionEndLineNumber = endLine;

    qint32 metaIndex = -1;
    in >> metaIndex;
    info.d_ptr->functionMetaIndex = metaIndex;

    in >> info.d_ptr->fileName;
    in >> info.d_ptr->functionName;
    in >> info.d_ptr->parameterNames;
    return in;
}
#endif

QT_END_NAMESPACE
