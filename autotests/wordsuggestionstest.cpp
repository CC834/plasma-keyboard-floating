// SPDX-FileCopyrightText: 2026 CC834
// SPDX-License-Identifier: GPL-3.0-only
#include "../src/wordsuggestions.h"
#include <QtTest>

class WordSuggestionsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void englishAndSwedish()
    {
        QVERIFY(WordSuggestions::complete(QStringLiteral("hell"), QStringLiteral("en_US")).contains(QStringLiteral("hello")));
        QVERIFY(WordSuggestions::complete(QStringLiteral("smö"), QStringLiteral("sv_SE")).contains(QStringLiteral("smör")));
        QCOMPARE(WordSuggestions::complete(QStringLiteral("sj"), QStringLiteral("sv_SE")).first(), QStringLiteral("själv"));
        QCOMPARE(WordSuggestions::complete(QStringLiteral("hel"), QStringLiteral("en_US")).first(), QStringLiteral("help"));
        QCOMPARE(WordSuggestions::complete(QStringLiteral("hel"), QStringLiteral("en_US")).size(), 3);
    }
    void caseAndApostrophes()
    {
        QCOMPARE(WordSuggestions::complete(QStringLiteral("Hel"), QStringLiteral("en_US")).first(), QStringLiteral("Help"));
        QCOMPARE(WordSuggestions::complete(QStringLiteral("HEL"), QStringLiteral("en_US")).first(), QStringLiteral("HELP"));
        QVERIFY(WordSuggestions::complete(QStringLiteral("don’"), QStringLiteral("en_US")).contains(QStringLiteral("don’t")));
        QVERIFY(WordSuggestions::complete(QStringLiteral("Smö"), QStringLiteral("sv_SE")).contains(QStringLiteral("Smör")));
    }
    void cursorAndSelection()
    {
        const QString text = QStringLiteral("Jag äter smö");
        QCOMPARE(WordSuggestions::prefix(text, text.size(), text.size()), QStringLiteral("smö"));
        QVERIFY(WordSuggestions::prefix(text, 5, 5).isEmpty()); // Inside an existing word.
        QVERIFY(WordSuggestions::prefix(text, 12, 9).isEmpty()); // Selected text.
        QVERIFY(WordSuggestions::prefix(QStringLiteral("hello "), 6, 6).isEmpty());
        QVERIFY(WordSuggestions::prefix(text, -1, -1).isEmpty());
    }
    void unsupportedInput()
    {
        QVERIFY(WordSuggestions::complete(QString(), QStringLiteral("sv_SE")).isEmpty());
        QVERIFY(WordSuggestions::complete(QStringLiteral("xyznotaword"), QStringLiteral("en_US")).isEmpty());
        QVERIFY(WordSuggestions::complete(QStringLiteral("hel"), QStringLiteral("it_IT")).isEmpty());
    }
};
QTEST_GUILESS_MAIN(WordSuggestionsTest)
#include "wordsuggestionstest.moc"
