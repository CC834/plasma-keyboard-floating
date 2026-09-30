/*
    SPDX-FileCopyrightText: 2024 Aleix Pol i Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2025 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2025 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVirtualKeyboardInputEngine>
#include <qqmlintegration.h>

#include <xkbcommon/xkbcommon.h>

#include "inputplugin.h"

class OverlayController;

class InputListenerItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_CLASSINFO("D-Bus Interface", "org.kde.plasma.keyboard.Floating")

    Q_PROPERTY(QVirtualKeyboardInputEngine *engine WRITE setEngine)
    Q_PROPERTY(bool keyboardNavigationActive MEMBER m_keyboardNavigationActive)
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(QString suggestionLocale READ suggestionLocale WRITE setSuggestionLocale NOTIFY suggestionLocaleChanged)

    /**
     * Controller for overlay popups (diacritics, emoji, text expansion).
     *
     * Exposed to QML for connecting overlay windows.
     */
    Q_PROPERTY(OverlayController *overlayController READ overlayController CONSTANT)

public:
    InputListenerItem();

    Q_SCRIPTABLE Q_INVOKABLE void showKeyboard();
    Q_SCRIPTABLE Q_INVOKABLE void hideKeyboard();

    void setEngine(QVirtualKeyboardInputEngine *engine);
    QStringList suggestions() const
    {
        return m_suggestions;
    }
    QString suggestionLocale() const
    {
        return m_suggestionLocale;
    }
    void setSuggestionLocale(const QString &locale);
    Q_INVOKABLE bool acceptSuggestion(const QString &word);

    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

    /**
     * Get the overlay controller.
     */
    OverlayController *overlayController() const;

Q_SIGNALS:
    void keyNavigationPressed(int key);
    void keyNavigationReleased(int key);
    void suggestionsChanged();
    void suggestionLocaleChanged();

private:
    void activateInputFocus();
    bool wantsKeyboardForCurrentActivation() const;
    bool suggestionsAllowed() const;
    void refreshSuggestions();
    void syncSuggestionPrefix();
    void trackCommittedText(const QString &text);

    InputPlugin m_input;
    OverlayController *m_overlayController = nullptr;
    QTimer m_hideTimer;
    bool m_keyboardNavigationActive = false;
    bool m_userDismissed = false;
    bool m_manualShowRequested = false;
    QString m_suggestionLocale;
    QString m_suggestionPrefix;
    QStringList m_suggestions;
    bool m_atWordBoundary = false;
};
