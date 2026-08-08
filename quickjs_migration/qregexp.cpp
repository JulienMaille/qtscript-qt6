#include "qregexp.h"

QRegExp::QRegExp() : m_syntax(RegExp) {}

QRegExp::QRegExp(const QString &pattern, Qt::CaseSensitivity cs, PatternSyntax syntax) : m_syntax(syntax) {
    QString pat = pattern;
    if (syntax == Wildcard) {
        pat = QRegularExpression::wildcardToRegularExpression(pattern);
    } else if (syntax == FixedString) {
        pat = QRegularExpression::escape(pattern);
    }

    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (cs == Qt::CaseInsensitive) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    re = QRegularExpression(pat, options);
}

QRegExp::QRegExp(const QRegExp &other) : re(other.re), m_syntax(other.m_syntax), lastMatch(other.lastMatch) {}

QRegExp &QRegExp::operator=(const QRegExp &other) {
    if (this != &other) {
        re = other.re;
        m_syntax = other.m_syntax;
        lastMatch = other.lastMatch;
    }
    return *this;
}

QRegExp::~QRegExp() {}

bool QRegExp::exactMatch(const QString &str) const {
    QString exactPattern = QStringLiteral("^%1$").arg(re.pattern());
    QRegularExpression exactRe(exactPattern, re.patternOptions());
    lastMatch = exactRe.match(str);
    return lastMatch.hasMatch();
}

int QRegExp::indexIn(const QString &str, int offset) const {
    lastMatch = re.match(str, offset);
    if (lastMatch.hasMatch()) {
        return lastMatch.capturedStart();
    }
    return -1;
}

QString QRegExp::cap(int nth) const {
    if (lastMatch.hasMatch() && nth <= lastMatch.lastCapturedIndex()) {
        return lastMatch.captured(nth);
    }
    return QString();
}
