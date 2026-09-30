/*
    SPDX-FileCopyrightText: 2025 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2026 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "inputpanelwindow.h"

#include "inputpanelintegration.h"

#include <KSandbox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDesktopServices>
#include <QProcess>
#include <qnamespace.h>

InputPanelWindow::InputPanelWindow(QWindow *parent)
    : QQuickWindow{parent}
{
    setFlag(Qt::FramelessWindowHint);
}

QRect InputPanelWindow::interactiveRegion() const
{
    return m_interactiveRegion;
}

void InputPanelWindow::setInteractiveRegion(QRect interactiveRegion)
{
    if (interactiveRegion == m_interactiveRegion) {
        return;
    }
    m_interactiveRegion = interactiveRegion;
    Q_EMIT interactiveRegionChanged();

    // Set only a part of the window to be interactive
    setMask(QRegion(m_interactiveRegion));
}

void InputPanelWindow::showSettings()
{
    if (m_lockScreenMode) {
        return;
    }
    if (KSandbox::isInside()) {
        QProcess::startDetached(QStringLiteral("kcmshell6"), {QStringLiteral("kcm_plasmakeyboard")});
    } else {
        QDesktopServices::openUrl(QUrl(QStringLiteral("systemsettings:kcm_plasmakeyboard")));
    }
}

bool InputPanelWindow::initInputPanel(InputPanelRole::Role role)
{
    return initInputPanelIntegration(this, role);
}

void InputPanelWindow::watchLockScreen()
{
    auto bus = QDBusConnection::sessionBus();
    bus.connect(QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("ActiveChanged"),
                this,
                SLOT(setLockScreenMode(bool)));
    auto watcher = new QDBusServiceWatcher(QStringLiteral("org.freedesktop.ScreenSaver"), bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        ++m_lockStateRevision;
        queryLockScreen();
    });
    queryLockScreen();
}

void InputPanelWindow::queryLockScreen()
{
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                  QStringLiteral("/ScreenSaver"),
                                                  QStringLiteral("org.freedesktop.ScreenSaver"),
                                                  QStringLiteral("GetActive"));
    auto call = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    const auto revision = m_lockStateRevision;
    connect(call, &QDBusPendingCallWatcher::finished, this, [this, revision](QDBusPendingCallWatcher *call) {
        const QDBusPendingReply<bool> reply = *call;
        call->deleteLater();
        // An ActiveChanged signal received after this query is more recent.
        if (!reply.isError() && revision == m_lockStateRevision) {
            setLockScreenMode(reply.value());
        }
    });
}

void InputPanelWindow::setLockScreenMode(bool locked)
{
    ++m_lockStateRevision;
    if (m_lockScreenMode == locked) {
        return;
    }
    if (locked) {
        m_desktopGeometry = geometry();
    }
    Q_EMIT surfaceAboutToChange();
    hide();
    // A wl_surface role is immutable. Recreate the native window before assigning
    // the input-panel role, and again to regain the default xdg-shell on unlock.
    destroy();
    m_lockScreenMode = locked;
    Q_EMIT lockScreenModeChanged();
    if (locked) {
        if (!initInputPanel(InputPanelRole::Keyboard)) {
            qWarning() << "Could not create the lock-screen input panel";
            return;
        }
    } else {
        setProperty("plasmaKeyboardInputPanelRole", QVariant());
        setGeometry(m_desktopGeometry);
        create();
    }
    Q_EMIT surfaceChanged();
}

#include "moc_inputpanelwindow.cpp"
