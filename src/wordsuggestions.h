// SPDX-FileCopyrightText: 2026 CC834
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QStringList>

class WordSuggestions
{
public:
    static bool isWordCharacter(QChar character);
    static QString prefix(const QString &text, int cursor, int anchor);
    static QStringList complete(const QString &prefix, const QString &locale);
};
