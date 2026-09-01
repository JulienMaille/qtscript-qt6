// Copyright (C) 2026
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QSCRIPT_QREGEXP_H
#define QSCRIPT_QREGEXP_H

#include <QtCore/qcontainerfwd.h>
#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>
#include <QtScript/qtscriptglobal.h>

QT_BEGIN_NAMESPACE

class QDataStream;
class QDebug;
struct QRegExpPrivate;

class Q_SCRIPT_EXPORT QRegExp
{
public:
    enum PatternSyntax {
        RegExp,
        Wildcard,
        FixedString,
        RegExp2,
        WildcardUnix,
        W3CXmlSchema11
    };
    enum CaretMode {
        CaretAtZero,
        CaretAtOffset,
        CaretWontMatch
    };

    QRegExp();
    explicit QRegExp(const QString &pattern,
                     Qt::CaseSensitivity cs = Qt::CaseSensitive,
                     PatternSyntax syntax = RegExp);
    QRegExp(const QRegExp &other);
    QRegExp(QRegExp &&other);
    ~QRegExp();
    QRegExp &operator=(const QRegExp &other);
    QRegExp &operator=(QRegExp &&other) noexcept
    {
        swap(other);
        return *this;
    }
    void swap(QRegExp &other) noexcept { qt_ptr_swap(priv, other.priv); }

    bool operator==(const QRegExp &other) const;
    bool operator!=(const QRegExp &other) const { return !operator==(other); }

    bool isEmpty() const;
    bool isValid() const;
    QString pattern() const;
    void setPattern(const QString &pattern);
    Qt::CaseSensitivity caseSensitivity() const;
    void setCaseSensitivity(Qt::CaseSensitivity sensitivity);
    PatternSyntax patternSyntax() const;
    void setPatternSyntax(PatternSyntax syntax);

    bool isMinimal() const;
    void setMinimal(bool minimal);
    bool exactMatch(const QString &text) const;

    operator QVariant() const;

    int indexIn(const QString &text, int offset = 0,
                CaretMode caretMode = CaretAtZero) const;
    int lastIndexIn(const QString &text, int offset = -1,
                    CaretMode caretMode = CaretAtZero) const;
    int matchedLength() const;
    int captureCount() const;
    QStringList capturedTexts() const;
    QStringList capturedTexts();
    QString cap(int nth = 0) const;
    QString cap(int nth = 0);
    int pos(int nth = 0) const;
    int pos(int nth = 0);
    QString errorString() const;
    QString errorString();

    QString replaceIn(const QString &text, const QString &after) const;
    QString removeIn(const QString &text) const { return replaceIn(text, QString()); }
    bool containedIn(const QString &text) const { return indexIn(text) != -1; }
    int countIn(const QString &text) const;
    QStringList splitString(const QString &text,
                            Qt::SplitBehavior behavior = Qt::KeepEmptyParts) const;
    int indexIn(const QStringList &list, int from) const;
    int lastIndexIn(const QStringList &list, int from) const;
    QStringList replaceIn(const QStringList &list, const QString &after) const;
    QStringList filterList(const QStringList &list) const;

    static QString escape(const QString &text);

private:
    QRegExpPrivate *priv;
};

Q_SCRIPT_EXPORT size_t qHash(const QRegExp &key, size_t seed = 0) noexcept;

#ifndef QT_NO_DATASTREAM
Q_SCRIPT_EXPORT QDataStream &operator<<(QDataStream &out, const QRegExp &regexp);
Q_SCRIPT_EXPORT QDataStream &operator>>(QDataStream &in, QRegExp &regexp);
#endif

#ifndef QT_NO_DEBUG_STREAM
Q_SCRIPT_EXPORT QDebug operator<<(QDebug debug, const QRegExp &regexp);
#endif

QT_END_NAMESPACE

Q_DECLARE_METATYPE(QRegExp)

#endif
