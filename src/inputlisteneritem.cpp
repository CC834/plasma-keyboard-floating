/*
    SPDX-FileCopyrightText: 2024 Aleix Pol i Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2025 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2026 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "inputlisteneritem.h"
#include "inputmethod_p.h"
#include "inputpanelwindow.h"
#include "logging.h"
#include "plasmakeyboardsettings.h"
#include "wordsuggestions.h"

#include "overlay/longpresstrigger.h"
#include "overlay/overlaycontroller.h"
#include "overlay/prefixquerytrigger.h"
#include "overlay/textexpansiontrigger.h"

#include <QDBusInterface>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QTextFormat>

#include <set>

#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <qpa/qwindowsysteminterface.h>

Q_GLOBAL_STATIC(InputMethod, s_im)

Q_GLOBAL_STATIC_WITH_ARGS(const std::set<int>,
                          IGNORED_KEYS,
                          {
                              Qt::Key_Context1 // Triggered by "special keys" button
                          });
// Keys to always capture for keyboard navigation
QList<Qt::Key> initCapture()
{
    return {
        Qt::Key_Left,
        Qt::Key_Right,
        Qt::Key_Up,
        Qt::Key_Down,
    };
}
Q_GLOBAL_STATIC_WITH_ARGS(const QList<Qt::Key>, KEYBOARD_NAVIGATION_CAPTURE_KEYS, (initCapture()));

// Keys to capture when keyboard navigation is active
Q_GLOBAL_STATIC_WITH_ARGS(const QList<Qt::Key>, KEYBOARD_NAVIGATION_ACTIVE_CAPTURE_KEYS, (initCapture() + QList<Qt::Key>{Qt::Key_Return}));

namespace
{
bool kwinWantsKeyboardForCurrentActivation()
{
    QDBusInterface virtualKeyboard(QStringLiteral("org.kde.KWin"),
                                   QStringLiteral("/VirtualKeyboard"),
                                   QStringLiteral("org.kde.kwin.VirtualKeyboard"),
                                   QDBusConnection::sessionBus());
    const QDBusReply<bool> reply = virtualKeyboard.call(QDBus::Block, QStringLiteral("willShowOnActive"));

    // Showing is the safer fallback on compositors without KWin's helper API.
    return !reply.isValid() || reply.value();
}
}

InputListenerItem::InputListenerItem()
    : m_input(&(*s_im))
    , m_overlayController(new OverlayController(&m_input, this))
{
    auto bus = QDBusConnection::sessionBus();
    if (bus.registerService(QStringLiteral("org.kde.plasma.keyboard.Floating"))) {
        bus.registerObject(QStringLiteral("/Keyboard"), this, QDBusConnection::ExportScriptableInvokables);
    }

    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        auto panel = qobject_cast<InputPanelWindow *>(window);
        if (!panel) {
            return;
        }
        connect(panel, &InputPanelWindow::surfaceAboutToChange, this, [this] {
            m_hideTimer.stop();
            m_manualShowRequested = false;
            m_userDismissed = false;
            m_overlayController->cancelOverlay();
            QGuiApplication::inputMethod()->reset();
            m_suggestionPrefix.clear();
            m_atWordBoundary = false;
            refreshSuggestions();
        });
        connect(panel, &InputPanelWindow::surfaceChanged, this, [this, panel] {
            m_hideTimer.stop();
            m_userDismissed = false;
            refreshSuggestions();
            if (m_input.hasContext() && (panel->lockScreenMode() || kwinWantsKeyboardForCurrentActivation())) {
                activateInputFocus();
                QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
                panel->show();
                QGuiApplication::inputMethod()->show();
            }
        });
    });

    // Grab and listen to physical keyboard input
    m_input.setGrabbing(true);
    connect(&m_input, &InputPlugin::contextChanged, this, &InputListenerItem::syncSuggestionPrefix);
    connect(&m_input, &InputPlugin::surroundingTextChanged, this, &InputListenerItem::syncSuggestionPrefix);
    connect(&m_input, &InputPlugin::contentTypeChanged, this, &InputListenerItem::syncSuggestionPrefix);

    // Register overlay triggers
    m_overlayController->registerTrigger(new LongPressTrigger(m_overlayController));
    m_overlayController->registerTrigger(new PrefixQueryTrigger(m_overlayController));
    m_overlayController->registerTrigger(new TextExpansionTrigger(m_overlayController));

    connect(m_overlayController, &OverlayController::overlayVisibleChanged, this, [this] {
        if (!m_overlayController->overlayVisible() && window() && window()->isVisible()) {
            // The overlay shell takes Qt's local focus while candidates are open.
            // Restore it after the overlay has unmapped, without activating KWin.
            QTimer::singleShot(0, this, [this] {
                if (window()->isVisible() && m_input.hasContext()) {
                    activateInputFocus();
                    QGuiApplication::inputMethod()->show();
                }
            });
        }
    });

    // Text-input contexts can briefly disappear while focus moves between two
    // fields. Avoid unmapping and remapping the Wayland window for that short
    // hand-off, which otherwise looks like visible flicker.
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(180);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] {
        if (!m_input.hasContext()) {
            m_manualShowRequested = false;
            window()->setVisible(false);
            QGuiApplication::inputMethod()->setVisible(false);
        }
    });

    connect(&m_input, &InputPlugin::contextChanged, this, [this] {
        const bool hasContext = m_input.hasContext();

        // Cancel any pending overlay state when the input context changes (focus loss or target swap)
        if (m_overlayController) {
            m_overlayController->cancelOverlay();
        }

        if (hasContext) {
            m_userDismissed = false;
            m_hideTimer.stop();
            activateInputFocus();
            QGuiApplication::inputMethod()->update(Qt::ImQueryAll);

            // In KWin's automatic mode, a mouse-focused field deliberately
            // does not summon the keyboard. Honor that decision even though
            // this floating xdg-shell window is not a compositor input panel.
            if (window()->isVisible() || m_manualShowRequested || kwinWantsKeyboardForCurrentActivation()) {
                window()->setVisible(true);
                QGuiApplication::inputMethod()->show();
            }
        } else {
            m_hideTimer.start();
        }
    });
    connect(&m_input, &InputPlugin::surroundingTextChanged, this, [this] {
        // Notify the overlay controller first so it can cancel the overlay if an
        // external cursor movement is detected (e.g. user tapped elsewhere in the
        // text field while the diacritics overlay was open or the hold timer was
        // still running). The controller uses an internal counter to distinguish
        // its own commit_string echoes from external cursor moves.
        if (m_overlayController) {
            m_overlayController->handleSurroundingTextChanged();
        }

        if (m_input.hasContext()) {
            // A field can first gain focus from a mouse, then be tapped without
            // creating a new input context. Honor the updated touch policy, but
            // don't undo an explicit dismissal on a surrounding-text echo.
            if (!window()->isVisible() && !m_userDismissed && kwinWantsKeyboardForCurrentActivation()) {
                activateInputFocus();
                window()->show();
                QGuiApplication::inputMethod()->show();
            }
            // Update vkbd input method only if the virtual keyboard panel is shown
            if (window()->isExposed()) {
                QGuiApplication::inputMethod()->update(Qt::ImSurroundingText);
            }
        }
    });
    connect(&m_input, &InputPlugin::deactivate, this, [this] {
        m_hideTimer.start();
        QGuiApplication::inputMethod()->reset();
    });
    connect(&m_input, &InputPlugin::resetRequested, this, [] {
        QGuiApplication::inputMethod()->reset();
    });
    connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, this, [this] {
        if (QGuiApplication::inputMethod()->isVisible()) {
            if (m_input.hasContext() && (m_manualShowRequested || kwinWantsKeyboardForCurrentActivation())) {
                m_hideTimer.stop();
                window()->setVisible(true);
            }
        } else if (m_input.hasContext()) {
            // Qt also hides its input method when an accent/emoji window takes
            // local focus. That is not a user dismissal of the floating keyboard.
            if (m_overlayController->overlayVisible() || QGuiApplication::focusWindow() != window()) {
                return;
            }
            // The keyboard's close key hides it without deactivating the text
            // field, so this path must remain immediate.
            m_userDismissed = true;
            m_manualShowRequested = false;
            window()->setVisible(false);
        } else {
            m_hideTimer.start();
        }
    });

    connect(&m_input, &InputPlugin::keyPressed, this, [this](QKeyEvent *keyEvent) {
        // qCDebug(PlasmaKeyboard) << "keyPressed. keycode:" << keyEvent->key() << "text:" << keyEvent->text() << "modifiers:" << keyEvent->modifiers();

        // Delegate to overlay controller for diacritics/emoji/text expansion
        if (m_overlayController && m_overlayController->processKeyPress(keyEvent)) {
            keyEvent->accept();
            return;
        }
        // The window can have isVisible() = true, but not be shown by the compositor if the input panel is suppressed
        if (!window()->isExposed()) {
            return;
        }

        if (PlasmaKeyboardSettings::self()->keyboardNavigationEnabled()) {
            // Keys to capture for keyboard navigation
            const auto keys = m_keyboardNavigationActive ? *KEYBOARD_NAVIGATION_ACTIVE_CAPTURE_KEYS : *KEYBOARD_NAVIGATION_CAPTURE_KEYS;

            // Forward and accept keyboard navigation events
            for (const auto key : keys) {
                if (keyEvent->key() == key) {
                    keyEvent->accept();
                    Q_EMIT keyNavigationPressed(key);
                    break;
                }
            }
        }
    });
    connect(&m_input, &InputPlugin::keyReleased, this, [this](QKeyEvent *keyEvent) {
        // qCDebug(PlasmaKeyboard) << "keyReleased. keycode:" << keyEvent->key() << "text:" << keyEvent->text();

        // Let the overlay controller handle release events (e.g. diacritics, emoji)
        if (m_overlayController && m_overlayController->processKeyRelease(keyEvent)) {
            keyEvent->accept();
            return;
        }
        // The window can have isVisible() = true, but not be shown by the compositor if the input panel is suppressed
        if (!window()->isExposed()) {
            return;
        }

        if (PlasmaKeyboardSettings::self()->keyboardNavigationEnabled()) {
            // Keys to capture for keyboard navigation
            const auto keys = m_keyboardNavigationActive ? *KEYBOARD_NAVIGATION_ACTIVE_CAPTURE_KEYS : *KEYBOARD_NAVIGATION_CAPTURE_KEYS;

            // Forward and accept keyboard navigation events
            for (const auto key : keys) {
                if (keyEvent->key() == key) {
                    keyEvent->accept();
                    Q_EMIT keyNavigationReleased(key);
                    break;
                }
            }
        }
    });

    // Don't hook into the &InputPlugin::receivedCommit signal and call setVisible(true)
    // -> it can cause a race condition as receivedCommit is emitted when the text field loses focus.
    //
    // If the virtual keyboard (vkbd) is manually closed and then the text field loses focus,
    // setVisible(true/false) calls in quick succession may reopen the vkbd in a broken state.
    //
    // Series of events:
    // - vkbd is manually closed (by user), but text field still focused -> setVisible(false)
    // - User unfocuses the text field
    // - receivedCommit() emitted -> setVisible(true)
    // - contextChanged() (m_input.hasContext() = false) -> setVisible(false)
    // - keyboard gets reopened by KWin because the input context gets from setVisible(true) finishes initializing!
    //
    // This happens because of a delay in InputPanelV1Window initialization, which confuses KWin
    // into creating a new input context even when no text field is focused.
    //
    // TODO: Investigate other places this might happen and how to fix it.

    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *quickWindow) {
        if (!quickWindow) {
            return;
        }
        connect(quickWindow, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible) {
                // Run after mapping, when Qt has processed the xdg configure.
                QTimer::singleShot(0, this, &InputListenerItem::activateInputFocus);
            }
        });
    });

    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}

void InputListenerItem::activateInputFocus()
{
    if (!window()) {
        return;
    }
    // Qt Virtual Keyboard dispatches to QGuiApplication::focusObject(). The
    // input-panel shell used to establish this local focus in applyConfigure().
    // An xdg tool window never gets compositor keyboard focus, deliberately.
    // Restore only Qt's in-process focus; do not request Wayland activation.
    forceActiveFocus();
    QWindowSystemInterface::handleFocusWindowChanged(window());
    QWindowSystemInterface::flushWindowSystemEvents();
}

void InputListenerItem::showKeyboard()
{
    m_userDismissed = false;
    m_manualShowRequested = true;
    m_hideTimer.stop();
    // KWin can fall back to key events when the focused application does not
    // implement Wayland text-input (including existing XWayland applications).
    QDBusInterface virtualKeyboard(QStringLiteral("org.kde.KWin"), QStringLiteral("/VirtualKeyboard"),
                                   QStringLiteral("org.kde.kwin.VirtualKeyboard"), QDBusConnection::sessionBus());
    virtualKeyboard.asyncCall(QStringLiteral("forceActivate"));
    if (m_input.hasContext()) {
        activateInputFocus();
        window()->show();
        QGuiApplication::inputMethod()->show();
    }
}

void InputListenerItem::hideKeyboard()
{
    m_userDismissed = true;
    m_manualShowRequested = false;
    m_hideTimer.stop();
    QGuiApplication::inputMethod()->hide();
    if (window()) {
        window()->hide();
    }
}

OverlayController *InputListenerItem::overlayController() const
{
    return m_overlayController;
}

void InputListenerItem::setEngine(QVirtualKeyboardInputEngine * /*engine*/)
{
    // TODO: hook into engine events if necessary?
}

QVariant InputListenerItem::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (!m_input.hasContext()) {
        if (query == Qt::ImEnabled) {
            return false;
        }
        return {};
    }

    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImSurroundingText:
        return m_input.surroundingText();
    case Qt::ImHints: {
        const auto imHints = m_input.contentHint();
        Qt::InputMethodHints qtHints;

        // Use our suffix-only completion bar instead of Hunspell's preedit
        // replacement, which interferes with external selection/cursor changes.
        qtHints |= Qt::ImhNoPredictiveText;

        // if (imHints & InputPlugin::content_hint_default) { }
        if (imHints & InputPlugin::content_hint_password) {
            qtHints |= Qt::ImhSensitiveData;
        }
        if ((imHints & InputPlugin::content_hint_auto_capitalization) == 0 || !PlasmaKeyboardSettings::self()->autoCapitalizationEnabled()) {
            qtHints |= Qt::ImhNoAutoUppercase;
        }
        // if (imHints & InputPlugin::content_hint_titlecase) { }
        if (imHints & InputPlugin::content_hint_lowercase) {
            qtHints |= Qt::ImhPreferLowercase;
        }
        if (imHints & InputPlugin::content_hint_uppercase) {
            qtHints |= Qt::ImhPreferUppercase;
        }
        if (imHints & InputPlugin::content_hint_hidden_text) {
            qtHints |= Qt::ImhSensitiveData;
        }
        if (imHints & InputPlugin::content_hint_sensitive_data) {
            qtHints |= Qt::ImhSensitiveData;
        }
        if (imHints & InputPlugin::content_hint_latin) {
            qtHints |= Qt::ImhPreferLatin;
        }
        if (imHints & InputPlugin::content_hint_multiline) {
            qtHints |= Qt::ImhMultiLine;
        }
        const auto imPurpose = m_input.contentPurpose();
        switch (imPurpose) {
        case InputPlugin::content_purpose_normal:
        case InputPlugin::content_purpose_alpha:
        case InputPlugin::content_purpose_name:
            break;
        case InputPlugin::content_purpose_digits:
            qtHints |= Qt::ImhDigitsOnly;
            break;
        case InputPlugin::content_purpose_number:
            qtHints |= Qt::ImhPreferNumbers;
            break;
        case InputPlugin::content_purpose_phone:
            qtHints |= Qt::ImhDialableCharactersOnly;
            break;
        case InputPlugin::content_purpose_url:
            qtHints |= Qt::ImhUrlCharactersOnly;
            break;
        case InputPlugin::content_purpose_email:
            qtHints |= Qt::ImhEmailCharactersOnly;
            break;
        case InputPlugin::content_purpose_password:
            qtHints |= Qt::ImhSensitiveData;
            break;
        case InputPlugin::content_purpose_date:
            qtHints |= Qt::ImhDate;
            break;
        case InputPlugin::content_purpose_time:
            qtHints |= Qt::ImhTime;
            break;
        case InputPlugin::content_purpose_datetime:
            qtHints |= Qt::ImhDate;
            qtHints |= Qt::ImhTime;
            break;
        case InputPlugin::content_purpose_terminal:
            qtHints |= Qt::ImhPreferLatin;
            break;
        }
        return QVariant::fromValue<int>(qtHints);
    }
    case Qt::ImCurrentSelection: {
        // cursorPos and anchorPos are in bytes, we need to convert QString to QByteArray for index operations
        QByteArray surroundingText = m_input.surroundingText().toUtf8();
        int start = qMin(m_input.cursorPos(), m_input.anchorPos());
        int end = qMax(m_input.cursorPos(), m_input.anchorPos());
        return QString::fromUtf8(surroundingText.mid(start, end - start));
    }
    case Qt::ImAnchorPosition: {
        // anchorPos is in bytes, we need to convert QString to QByteArray for index operations
        QByteArray surroundingText = m_input.surroundingText().toUtf8();
        int anchorBytes = qBound(0, int(m_input.anchorPos()), surroundingText.size());
        return QString::fromUtf8(surroundingText.first(anchorBytes)).length();
    }
    case Qt::ImCursorPosition: {
        // cursorPos is in bytes, we need to convert QString to QByteArray for index operations
        QByteArray surroundingText = m_input.surroundingText().toUtf8();
        int cursorBytes = qBound(0, int(m_input.cursorPos()), surroundingText.size());
        return QString::fromUtf8(surroundingText.first(cursorBytes)).length();
    }
    case Qt::ImTextBeforeCursor: {
        // cursorPos is in bytes, we need to convert QString to QByteArray for index operations
        QByteArray surroundingText = m_input.surroundingText().toUtf8();
        int cursorBytes = qBound(0, int(m_input.cursorPos()), surroundingText.size());
        return QString::fromUtf8(surroundingText.first(cursorBytes));
    }
    case Qt::ImTextAfterCursor: {
        // cursorPos is in bytes, we need to convert QString to QByteArray for index operations
        QByteArray surroundingText = m_input.surroundingText().toUtf8();
        return QString::fromUtf8(surroundingText.mid(m_input.cursorPos()));
    }
    case Qt::ImCursorRectangle:
    case Qt::ImFont:
    case Qt::ImMaximumTextLength:
    case Qt::ImPreferredLanguage:
    case Qt::ImPlatformData:
    case Qt::ImAbsolutePosition:
    case Qt::ImEnterKeyType:
    case Qt::ImAnchorRectangle:
    case Qt::ImInputItemClipRectangle:
    case Qt::ImReadOnly:
        // We don't do that
        break;
    default:
        qWarning() << "Unhandled query" << query;
        break;
    }
    return {};
}

void InputListenerItem::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Backspace && m_atWordBoundary) {
        m_suggestionPrefix.chop(1);
        refreshSuggestions();
    } else if (event->text().isEmpty() || event->key() == Qt::Key_Return) {
        m_suggestionPrefix.clear();
        m_atWordBoundary = false;
        refreshSuggestions();
    }
    if (IGNORED_KEYS->find(event->key()) != IGNORED_KEYS->end()) {
        return;
    }

    const QList<xkb_keysym_t> keys = QXkbCommon::toKeysym(event);
    for (auto key : keys) {
        // Simulate key press only if it's not textual
        if (event->text().isEmpty() || key == XKB_KEY_Return) { // (return is technically "\n")
            m_input.keysym(QDateTime::currentMSecsSinceEpoch(), key, InputPlugin::Pressed, 0);
        }
    }
}

void InputListenerItem::keyReleaseEvent(QKeyEvent *event)
{
    if (IGNORED_KEYS->find(event->key()) != IGNORED_KEYS->end()) {
        return;
    }

    const QList<xkb_keysym_t> keys = QXkbCommon::toKeysym(event);
    for (auto key : keys) {
        if (event->text().isEmpty() || key == XKB_KEY_Return) { // (return is technically "\n")
            // Simulate the keyboard press for non textual keys
            m_input.keysym(QDateTime::currentMSecsSinceEpoch(), key, InputPlugin::Released, 0);
        } else {
            // If we have text coming as a key event, use it to commit the string
            m_input.commit(event->text());
            trackCommittedText(event->text());
        }
    }
}

void InputListenerItem::inputMethodEvent(QInputMethodEvent *event)
{
    // Don't forward events from Qt VKBD to KWin if the keyboard window isn't open
    // This avoids the possibility of Qt VKBD causing text changes from receiving
    // events (ex. cursor movement) done by its input engine (for features like text prediction).
    if (!window()->isExposed()) {
        return;
    }

    QString commit = event->commitString();
    QString preedit = event->preeditString();
    bool needsReplacement = event->replacementStart() != 0 || event->replacementLength() != 0;

    // Delete characters that are supposed to be replaced
    if (needsReplacement) {
        // The positions we send to need to be in bytes (to support UTF8)
        QString surroundingText = m_input.surroundingText();
        QByteArray surroundingUtf8 = surroundingText.toUtf8();
        int boundedCursorBytes = qBound(0, int(m_input.cursorPos()), surroundingUtf8.size());
        int cursorChars = QString::fromUtf8(surroundingUtf8.first(boundedCursorBytes)).size();
        int startChars = qBound(0, cursorChars + event->replacementStart(), surroundingText.size());
        int endChars = qBound(startChars, startChars + event->replacementLength(), surroundingText.size());

        int startBytes = surroundingText.first(startChars).toUtf8().size();
        int endBytes = surroundingText.first(endChars).toUtf8().size();

        if (endBytes > startBytes) {
            m_input.deleteSurroundingText(startBytes - boundedCursorBytes, endBytes - startBytes);
        }
    }

    // Commit string if there is something to commit, or we did a replacement
    if (needsReplacement || !commit.isEmpty()) {
        m_input.commit(commit);
        if (needsReplacement) {
            m_suggestionPrefix.clear();
        }
        trackCommittedText(commit);
    }

    // Set attributes for style (ex. needed for CJK)
    for (const auto &x : event->attributes()) {
        if (x.type == QInputMethodEvent::TextFormat) {
            int startBytes = preedit.first(qBound(0, x.start, preedit.size())).toUtf8().size();
            int endBytes = preedit.first(qBound(0, x.start + x.length, preedit.size())).toUtf8().size();
            m_input.setPreEditStyle(startBytes, endBytes - startBytes, x.value.value<QTextFormat>().type());
        }
    }

    // Send cursor position (must be before preedit string)
    int preEditCursorPos = preedit.toUtf8().size();
    for (const auto &attribute : event->attributes()) {
        if (attribute.type != QInputMethodEvent::Cursor) {
            continue;
        }

        // Set special attribute preedit cursor position (ex. needed for CJK)
        if (!attribute.value.isValid() || !attribute.value.toBool()) {
            preEditCursorPos = -1;
        } else {
            preEditCursorPos = preedit.first(qBound(0, attribute.start, preedit.size())).toUtf8().size();
        }
        break;
    }
    m_input.setPreEditCursor(preEditCursorPos);

    // Send currently being edited string
    m_input.setPreEditString(preedit);
}

bool InputListenerItem::suggestionsAllowed() const
{
    const auto panel = qobject_cast<InputPanelWindow *>(window());
    if ((panel && panel->lockScreenMode()) || !m_input.hasContext()
        || (m_input.contentHint() & (InputPlugin::content_hint_hidden_text | InputPlugin::content_hint_sensitive_data))) {
        return false;
    }
    const auto purpose = m_input.contentPurpose();
    return purpose == InputPlugin::content_purpose_normal || purpose == InputPlugin::content_purpose_alpha || purpose == InputPlugin::content_purpose_name;
}

void InputListenerItem::syncSuggestionPrefix()
{
    const QByteArray text = m_input.surroundingText().toUtf8();
    const int cursor = QString::fromUtf8(text.first(qMin(qsizetype(m_input.cursorPos()), text.size()))).size();
    const int anchor = QString::fromUtf8(text.first(qMin(qsizetype(m_input.anchorPos()), text.size()))).size();
    const QString unicode = QString::fromUtf8(text);
    m_atWordBoundary = suggestionsAllowed() && cursor == anchor && (cursor == unicode.size() || !WordSuggestions::isWordCharacter(unicode.at(cursor)));
    m_suggestionPrefix = m_atWordBoundary ? WordSuggestions::prefix(unicode, cursor, anchor) : QString();
    refreshSuggestions();
}

void InputListenerItem::trackCommittedText(const QString &text)
{
    if (m_atWordBoundary && suggestionsAllowed()) {
        const QString combined = m_suggestionPrefix + text;
        m_suggestionPrefix = WordSuggestions::prefix(combined, combined.size(), combined.size());
    }
    refreshSuggestions();
}

void InputListenerItem::refreshSuggestions()
{
    const QStringList words = suggestionsAllowed() && m_atWordBoundary ? WordSuggestions::complete(m_suggestionPrefix, m_suggestionLocale) : QStringList();
    if (words != m_suggestions) {
        m_suggestions = words;
        Q_EMIT suggestionsChanged();
    }
}

void InputListenerItem::setSuggestionLocale(const QString &locale)
{
    if (locale != m_suggestionLocale) {
        m_suggestionLocale = locale;
        refreshSuggestions();
        Q_EMIT suggestionLocaleChanged();
    }
}

bool InputListenerItem::acceptSuggestion(const QString &word)
{
    if (!suggestionsAllowed() || !m_atWordBoundary || m_suggestionPrefix.isEmpty() || !m_suggestions.contains(word)) {
        return false;
    }
    // Append only the missing suffix. Never delete or rewrite surrounding text.
    const QString suffix = word.mid(m_suggestionPrefix.size()) + QLatin1Char(' ');
    m_input.commit(suffix);
    trackCommittedText(suffix);
    return true;
}

#include "moc_inputlisteneritem.cpp"
