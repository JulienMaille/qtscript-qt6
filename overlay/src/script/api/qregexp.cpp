// Copyright (C) 2026
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qregexp.h"

#include <QtCore/qdatastream.h>
#include <QtCore/qdebug.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qstringlist.h>

QT_BEGIN_NAMESPACE

struct QRegExpPrivate
{
    QString pattern;
    Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive;
    QRegExp::PatternSyntax syntax = QRegExp::RegExp;
    bool minimal = false;
    mutable QRegularExpressionMatch lastMatch;
    mutable QString error;
};

static QString qregexpWildcardUnixPattern(const QString &wildcard)
{
    QString pattern;
    bool escaping = false;
    for (int i = 0; i < wildcard.size(); ++i) {
        const QChar c = wildcard.at(i);
        if (escaping) {
            pattern += QRegularExpression::escape(QString(c));
            escaping = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaping = true;
            continue;
        }
        if (c == QLatin1Char('*')) {
            pattern += QLatin1String(".*");
        } else if (c == QLatin1Char('?')) {
            pattern += QLatin1Char('.');
        } else if (c == QLatin1Char('[')) {
            const int classStart = i;
            int classEnd = i + 1;
            if (classEnd < wildcard.size() && wildcard.at(classEnd) == QLatin1Char('^'))
                ++classEnd;
            if (classEnd < wildcard.size() && wildcard.at(classEnd) == QLatin1Char(']'))
                ++classEnd;
            while (classEnd < wildcard.size() && wildcard.at(classEnd) != QLatin1Char(']'))
                ++classEnd;
            if (classEnd < wildcard.size()) {
                pattern += wildcard.mid(classStart, classEnd - classStart + 1);
                i = classEnd;
            } else {
                pattern += QRegularExpression::escape(QString(c));
            }
        } else {
            pattern += QRegularExpression::escape(QString(c));
        }
    }
    if (escaping)
        pattern += QRegularExpression::escape(QStringLiteral("\\"));
    return pattern;
}

static QString qregexpPattern(const QRegExpPrivate *d)
{
    switch (d->syntax) {
    case QRegExp::Wildcard: {
        QString pattern = QRegularExpression::wildcardToRegularExpression(
            d->pattern, QRegularExpression::UnanchoredWildcardConversion
                        | QRegularExpression::NonPathWildcardConversion);
        // Qt 6 wraps converted wildcards in a dot-all inline group. The 2011
        // JSC regexp parser cannot parse that syntax, so strip the wrapper and
        // restore its matching behavior through a QRegularExpression option.
        if (pattern.startsWith(QLatin1String("(?s:")) && pattern.endsWith(QLatin1Char(')')))
            pattern = pattern.mid(4, pattern.size() - 5);
        return pattern;
    }
    case QRegExp::WildcardUnix:
        return qregexpWildcardUnixPattern(d->pattern);
    case QRegExp::FixedString:
        return QRegularExpression::escape(d->pattern);
    case QRegExp::RegExp:
    case QRegExp::RegExp2:
    case QRegExp::W3CXmlSchema11:
        // RegExp2 only flipped the default greediness and W3CXmlSchema11
        // added XML Schema extensions in Qt 5; the 2011 JSC parser accepts
        // neither, so both map to plain RegExp.
        return d->pattern;
    }
    return d->pattern;
}

QString qt_regexp_toCanonical(const QString &pattern, QRegExp::PatternSyntax syntax)
{
    QRegExpPrivate d;
    d.pattern = pattern;
    d.syntax = syntax;
    return qregexpPattern(&d);
}

static QRegularExpression qregexpExpression(const QRegExpPrivate *d,
                                             bool anchored = false,
                                             bool anchorAtStart = false,
                                             const QString &patternOverride = QString())
{
    QString pattern = patternOverride.isNull() ? qregexpPattern(d) : patternOverride;
    if (anchored)
        pattern = QRegularExpression::anchoredPattern(pattern);
    else if (anchorAtStart)
        pattern = QLatin1String("\\A(?:") + pattern + QLatin1Char(')');
    QRegularExpression::PatternOptions options;
    if (d->caseSensitivity == Qt::CaseInsensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    if (d->minimal)
        options |= QRegularExpression::InvertedGreedinessOption;
    if (d->syntax == QRegExp::Wildcard || d->syntax == QRegExp::WildcardUnix)
        options |= QRegularExpression::DotMatchesEverythingOption;
    return QRegularExpression(pattern, options);
}

static void qregexpClearMatch(QRegExpPrivate *d)
{
    d->lastMatch = QRegularExpressionMatch();
}

// True when the pattern contains a ^ anchor outside a character class and
// not escaped. qregexpPattern() escapes ^ for FixedString syntax and never
// produces one for wildcards, so this only fires for the RegExp dialects.
static bool qregexpHasCaretAnchor(const QString &pattern)
{
    bool inClass = false;
    bool escaped = false;
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (escaped) {
            escaped = false;
        } else if (c == QLatin1Char('\\')) {
            escaped = true;
        } else if (c == QLatin1Char('[')) {
            inClass = true;
        } else if (c == QLatin1Char(']')) {
            inClass = false;
        } else if (c == QLatin1Char('^') && !inClass) {
            return true;
        }
    }
    return false;
}

static QString qregexpDisableCaretAnchors(const QString &pattern)
{
    QString result;
    result.reserve(pattern.size());
    bool inClass = false;
    bool escaped = false;
    for (const QChar c : pattern) {
        if (escaped) {
            result += c;
            escaped = false;
        } else if (c == QLatin1Char('\\')) {
            result += c;
            escaped = true;
        } else if (c == QLatin1Char('[')) {
            result += c;
            inClass = true;
        } else if (c == QLatin1Char(']')) {
            result += c;
            inClass = false;
        } else if (c == QLatin1Char('^') && !inClass) {
            // An always-failing assertion removes only this alternative;
            // unanchored alternatives such as ^foo|bar still participate.
            result += QLatin1String("(?!)");
        } else {
            result += c;
        }
    }
    return result;
}

QRegExp::QRegExp()
    : priv(new QRegExpPrivate)
{
}

QRegExp::QRegExp(const QString &pattern, Qt::CaseSensitivity cs, PatternSyntax syntax)
    : priv(new QRegExpPrivate)
{
    priv->pattern = pattern;
    priv->caseSensitivity = cs;
    priv->syntax = syntax;
}

QRegExp::QRegExp(const QRegExp &other)
    : priv(new QRegExpPrivate(*other.priv))
{
}

QRegExp::QRegExp(QRegExp &&other)
    : priv(new QRegExpPrivate)
{
    QRegExpPrivate *empty = priv;
    priv = other.priv;
    other.priv = empty;
}

QRegExp::~QRegExp()
{
    delete priv;
}

QRegExp &QRegExp::operator=(const QRegExp &other)
{
    if (this != &other)
        *priv = *other.priv;
    return *this;
}

bool QRegExp::operator==(const QRegExp &other) const
{
    return priv->pattern == other.priv->pattern
        && priv->caseSensitivity == other.priv->caseSensitivity
        && priv->syntax == other.priv->syntax
        && priv->minimal == other.priv->minimal;
}

bool QRegExp::isEmpty() const
{
    return priv->pattern.isEmpty();
}

bool QRegExp::isValid() const
{
    const QRegularExpression expression = qregexpExpression(priv);
    priv->error = expression.errorString();
    return expression.isValid();
}

QString QRegExp::pattern() const
{
    return priv->pattern;
}

void QRegExp::setPattern(const QString &pattern)
{
    priv->pattern = pattern;
    qregexpClearMatch(priv);
}

Qt::CaseSensitivity QRegExp::caseSensitivity() const
{
    return priv->caseSensitivity;
}

void QRegExp::setCaseSensitivity(Qt::CaseSensitivity sensitivity)
{
    priv->caseSensitivity = sensitivity;
    qregexpClearMatch(priv);
}

QRegExp::PatternSyntax QRegExp::patternSyntax() const
{
    return priv->syntax;
}

void QRegExp::setPatternSyntax(PatternSyntax syntax)
{
    priv->syntax = syntax;
    qregexpClearMatch(priv);
}

bool QRegExp::isMinimal() const
{
    return priv->minimal;
}

void QRegExp::setMinimal(bool minimal)
{
    priv->minimal = minimal;
    qregexpClearMatch(priv);
}

bool QRegExp::exactMatch(const QString &text) const
{
    const QRegularExpression expression = qregexpExpression(priv, false, true);
    priv->error = expression.errorString();
    if (!expression.isValid()) {
        qregexpClearMatch(priv);
        return false;
    }
    priv->lastMatch = expression.match(text);
    if (priv->lastMatch.hasMatch())
        return priv->lastMatch.capturedStart() == 0
            && priv->lastMatch.capturedEnd() == text.size();
    // Qt 5 reports a zero-length partial match for a valid regexp that does
    // not match at the start, rather than reporting an invalid match.
    priv->lastMatch = QRegularExpression(QStringLiteral("\\A")).match(text);
    return false;
}

QRegExp::operator QVariant() const
{
    return QVariant::fromValue(*this);
}

int QRegExp::indexIn(const QString &text, int offset, CaretMode caretMode) const
{
    if (offset < 0)
        offset += text.size();
    if (offset < 0 || offset > text.size()) {
        qregexpClearMatch(priv);
        return -1;
    }
    const QString pattern = qregexpPattern(priv);
    const bool hasCaret = qregexpHasCaretAnchor(pattern);
    const QString patternOverride = caretMode == CaretWontMatch && hasCaret
        ? qregexpDisableCaretAnchors(pattern) : QString();
    const QRegularExpression expression = qregexpExpression(priv, false, false,
                                                              patternOverride);
    priv->error = expression.errorString();
    priv->lastMatch = expression.match(text, offset);
    if (caretMode == CaretAtOffset && hasCaret) {
        // AnchorAtOffsetMatchOption anchors the whole expression.  Try that
        // candidate separately so unanchored alternatives may still match
        // later in the string.
        const QRegularExpressionMatch anchored = expression.match(
            text, offset, QRegularExpression::NormalMatch,
            QRegularExpression::AnchorAtOffsetMatchOption);
        if (anchored.hasMatch()
            && (!priv->lastMatch.hasMatch()
                || anchored.capturedStart() < priv->lastMatch.capturedStart())) {
            priv->lastMatch = anchored;
        }
    }
    return priv->lastMatch.hasMatch() ? int(priv->lastMatch.capturedStart()) : -1;
}

int QRegExp::lastIndexIn(const QString &text, int offset, CaretMode caretMode) const
{
    if (offset < 0)
        offset += text.size();
    if (offset < 0 || offset > text.size()) {
        qregexpClearMatch(priv);
        return -1;
    }
    const QString pattern = qregexpPattern(priv);
    const bool hasCaret = qregexpHasCaretAnchor(pattern);
    const QString patternOverride = caretMode == CaretWontMatch && hasCaret
        ? qregexpDisableCaretAnchors(pattern) : QString();
    const QRegularExpression expression = qregexpExpression(priv, false, false,
                                                              patternOverride);
    priv->error = expression.errorString();
    QRegularExpressionMatch selected;
    if (caretMode == CaretAtOffset && hasCaret) {
        selected = expression.match(text, offset,
                                    QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
        if (!selected.hasMatch() || selected.capturedStart() != offset)
            selected = QRegularExpressionMatch();
    }
    // Qt 5 tried each position and returned the highest one whose match
    // starts there; a plain globalMatch scan would miss overlapping matches
    // (e.g. "a+" in "aaa").  The caret-at-offset candidate above is the
    // only position where that mode changes the meaning of ^.
    if (!selected.hasMatch()) {
        for (int pos = offset; pos >= 0; --pos) {
            const QRegularExpressionMatch candidate =
                expression.match(text, pos);
            if (candidate.hasMatch() && candidate.capturedStart() == pos) {
                selected = candidate;
                break;
            }
        }
    }
    priv->lastMatch = selected;
    return selected.hasMatch() ? int(selected.capturedStart()) : -1;
}

int QRegExp::matchedLength() const
{
    return priv->lastMatch.hasMatch() ? int(priv->lastMatch.capturedLength()) : -1;
}

int QRegExp::captureCount() const
{
    return qregexpExpression(priv).captureCount();
}

QStringList QRegExp::capturedTexts() const
{
    return priv->lastMatch.capturedTexts();
}

QStringList QRegExp::capturedTexts()
{
    return static_cast<const QRegExp *>(this)->capturedTexts();
}

QString QRegExp::cap(int nth) const
{
    return priv->lastMatch.captured(nth);
}

QString QRegExp::cap(int nth)
{
    return static_cast<const QRegExp *>(this)->cap(nth);
}

int QRegExp::pos(int nth) const
{
    return priv->lastMatch.hasCaptured(nth)
        ? int(priv->lastMatch.capturedStart(nth)) : -1;
}

int QRegExp::pos(int nth)
{
    return static_cast<const QRegExp *>(this)->pos(nth);
}

QString QRegExp::errorString() const
{
    const QRegularExpression expression = qregexpExpression(priv);
    return expression.isValid() ? QStringLiteral("no error occurred")
                                : expression.errorString();
}

QString QRegExp::errorString()
{
    return static_cast<const QRegExp *>(this)->errorString();
}

QString QRegExp::replaceIn(const QString &text, const QString &after) const
{
    QString result = text;
    result.replace(qregexpExpression(priv), after);
    return result;
}

int QRegExp::countIn(const QString &text) const
{
    int count = 0;
    QRegularExpressionMatchIterator iterator = qregexpExpression(priv).globalMatch(text);
    while (iterator.hasNext()) {
        iterator.next();
        ++count;
    }
    return count;
}

QStringList QRegExp::splitString(const QString &text, Qt::SplitBehavior behavior) const
{
    return text.split(qregexpExpression(priv), behavior);
}

int QRegExp::indexIn(const QStringList &list, int from) const
{
    const int start = from < 0 ? qMax(0, list.size() + from) : from;
    for (int i = start; i < list.size(); ++i) {
        if (indexIn(list.at(i)) != -1)
            return i;
    }
    return -1;
}

int QRegExp::lastIndexIn(const QStringList &list, int from) const
{
    int start = from < 0 ? list.size() + from : from;
    start = qMin(start, list.size() - 1);
    for (int i = start; i >= 0; --i) {
        if (indexIn(list.at(i)) != -1)
            return i;
    }
    return -1;
}

QStringList QRegExp::replaceIn(const QStringList &list, const QString &after) const
{
    QStringList result = list;
    for (QString &text : result)
        text = replaceIn(text, after);
    return result;
}

QStringList QRegExp::filterList(const QStringList &list) const
{
    QStringList result;
    for (const QString &text : list) {
        if (indexIn(text) != -1)
            result.append(text);
    }
    return result;
}

QString QRegExp::escape(const QString &text)
{
    return QRegularExpression::escape(text);
}

size_t qHash(const QRegExp &key, size_t seed) noexcept
{
    seed = qHash(key.pattern(), seed);
    seed ^= size_t(key.caseSensitivity()) << 1;
    seed ^= size_t(key.patternSyntax()) << 4;
    seed ^= size_t(key.isMinimal()) << 8;
    return seed;
}

#ifndef QT_NO_DATASTREAM
QDataStream &operator<<(QDataStream &out, const QRegExp &regexp)
{
    out << regexp.pattern()
        << qint32(regexp.caseSensitivity())
        << qint32(regexp.patternSyntax())
        << regexp.isMinimal();
    return out;
}

QDataStream &operator>>(QDataStream &in, QRegExp &regexp)
{
    QString pattern;
    qint32 sensitivity;
    qint32 syntax;
    bool minimal;
    in >> pattern >> sensitivity >> syntax >> minimal;
    regexp = QRegExp(pattern, Qt::CaseSensitivity(sensitivity),
                     QRegExp::PatternSyntax(syntax));
    regexp.setMinimal(minimal);
    return in;
}
#endif

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug debug, const QRegExp &regexp)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "QRegExp(" << regexp.pattern() << ')';
    return debug;
}
#endif

QT_END_NAMESPACE
