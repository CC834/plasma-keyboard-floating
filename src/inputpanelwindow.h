/*
    SPDX-FileCopyrightText: 2025 Devin Lin <devin@kde.org>
    SPDX-FileCopyrightText: 2026 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "inputpanelrole.h"

#include <QObject>
#include <QQuickWindow>
#include <qqmlintegration.h>

class InputPanelWindow : public QQuickWindow
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool lockScreenMode READ lockScreenMode NOTIFY lockScreenModeChanged)
    Q_PROPERTY(QRect interactiveRegion READ interactiveRegion WRITE setInteractiveRegion NOTIFY interactiveRegionChanged)

public:
    explicit InputPanelWindow(QWindow *parent = nullptr);

    // Only the main keyboard watches the lock state; overlay windows keep their role.
    void watchLockScreen();
    bool lockScreenMode() const
    {
        return m_lockScreenMode;
    }

    // The interactive part of the keyboard that is reserved on the screen for the input panel.
    // The space outside of it will be overlaid will have input passed to underlying windows by the compositor.
    // This is required for some visual items (ex. popovers) to show outside of the input panel.
    QRect interactiveRegion() const;
    void setInteractiveRegion(QRect interactiveRegion);

    Q_INVOKABLE void showSettings();

    /**
     * Initialize this window as an input panel for the compositor.
     *
     * @param role The intended compositor role for the window.
     * @return True if integration was initialized successfully, otherwise false.
     */
    Q_INVOKABLE bool initInputPanel(InputPanelRole::Role role);

Q_SIGNALS:
    void interactiveRegionChanged();
    void lockScreenModeChanged();
    void surfaceAboutToChange();
    void surfaceChanged();

private Q_SLOTS:
    void setLockScreenMode(bool locked);

private:
    void queryLockScreen();
    QRect m_interactiveRegion;
    QRect m_desktopGeometry;
    bool m_lockScreenMode = false;
    quint64 m_lockStateRevision = 0;
};
