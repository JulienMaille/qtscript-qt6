#ifndef QREGEXP_H
#define QREGEXP_H

#include <QtCore/qstring.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qmetatype.h>
#include <QtScript/qtscriptglobal.h>

class Q_SCRIPT_EXPORT QRegExp {
public:
    enum PatternSyntax {
        RegExp,
        Wildcard,
        FixedString
    };

    QRegExp();
    QRegExp(const QString &pattern, Qt::CaseSensitivity cs = Qt::CaseSensitive, PatternSyntax syntax = RegExp);
    QRegExp(const QRegExp &other);
    QRegExp &operator=(const QRegExp &other);
    ~QRegExp();

    bool exactMatch(const QString &str) const;
    int indexIn(const QString &str, int offset = 0) const;
    QString cap(int nth = 0) const;

private:
    QRegularExpression re;
    PatternSyntax m_syntax;
    mutable QRegularExpressionMatch lastMatch;
};

Q_DECLARE_METATYPE(QRegExp)

#endif
