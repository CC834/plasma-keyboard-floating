// SPDX-FileCopyrightText: 2026 CC834
// SPDX-License-Identifier: GPL-3.0-only
#include "wordsuggestions.h"

#include <QFile>

namespace
{
QStringList loadWords(const QString &language)
{
    QFile file(QStringLiteral(":/suggestions/") + language + QStringLiteral(".txt"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}
}

bool WordSuggestions::isWordCharacter(QChar character)
{
    return character.isLetter() || character == QLatin1Char('\'') || character == QChar(0x2019);
}

QString WordSuggestions::prefix(const QString &text, int cursor, int anchor)
{
    if (cursor != anchor || cursor < 0 || cursor > text.size() || (cursor < text.size() && isWordCharacter(text.at(cursor)))) {
        return {};
    }
    int start = cursor;
    while (start > 0 && isWordCharacter(text.at(start - 1))) {
        --start;
    }
    const QString result = text.mid(start, cursor - start);
    return result.size() <= 64 ? result : QString();
}

QStringList WordSuggestions::complete(const QString &prefix, const QString &locale)
{
    if (prefix.isEmpty() || prefix.size() > 64 || !prefix.front().isLetter()) {
        return {};
    }
    // Frequency order is retained in the bundled word lists. No typed text is saved.
    static const QStringList english = loadWords(QStringLiteral("en"));
    static const QStringList swedish = loadWords(QStringLiteral("sv"));
    const auto language = locale.left(2);
    if (language != QLatin1String("en") && language != QLatin1String("sv")) {
        return {};
    }
    const auto &words = language == QLatin1String("sv") ? swedish : english;
    const QString match = QString(prefix).replace(QChar(0x2019), QLatin1Char('\''));
    const bool allCaps = prefix.size() > 1 && prefix == prefix.toUpper();
    QStringList result;
    for (const QString &word : words) {
        if (word.startsWith(match, Qt::CaseInsensitive)) {
            // Keep exactly what was typed, including case and curly apostrophes.
            QString suffix = word.mid(prefix.size());
            if (allCaps) {
                suffix = suffix.toUpper();
            }
            result.append(prefix + suffix);
            if (result.size() == 3) {
                break;
            }
        }
    }
    return result;
}
