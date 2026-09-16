/*
    SPDX-FileCopyrightText: 2024 Aleix Pol i Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2025 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "config-plasma-keyboard.h"
#include "inputpanelintegration.h"
#include "layoutpathhelper.h"
#include "logging.h"
#include "plasmakeyboardsettings.h"
#include <plasma_keyboard_version.h>

#include <KAboutData>
#include <KConfigWatcher>
#include <KCrash>
#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

int main(int argc, char **argv)
{
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));

    initLayoutsPath();

    QGuiApplication application(argc, argv);

    KLocalizedString::setApplicationDomain("plasma-keyboard");

    KAboutData aboutData(QStringLiteral("plasma-keyboard"),
                         i18n("Plasma Keyboard"),
                         QStringLiteral(PLASMA_KEYBOARD_VERSION_STRING),
                         i18n("An on-screen keyboard for Plasma"),
                         KAboutLicense::GPL,
                         i18n("Copyright 2024, Aleix Pol Gonzalez"));

    aboutData.addAuthor(i18n("Aleix Pol Gonzalez"), i18n("Author"), QStringLiteral("aleixpol@kde.org"));
    aboutData.setOrganizationDomain("kde.org");
    aboutData.setDesktopFileName(QStringLiteral("org.kde.plasma.keyboard"));
    application.setWindowIcon(QIcon::fromTheme(QStringLiteral("input-keyboard-virtual")));
    aboutData.setProgramLogo(application.windowIcon());

    KAboutData::setApplicationData(aboutData);

    KCrash::initialize();

    {
        QCommandLineParser parser;
        aboutData.setupCommandLine(&parser);
        parser.process(application);
        aboutData.processCommandLine(&parser);
    }

    if (!PLASMA_KEYBOARD_SOUNDS_ENABLED) {
        PlasmaKeyboardSettings::self()->setSoundEnabled(false);
    }

    if (!PLASMA_KEYBOARD_VIBRATION_ENABLED) {
        PlasmaKeyboardSettings::self()->setVibrationEnabled(false);
    }

    // A floating keyboard must keep compact key proportions regardless of the
    // docked keyboard setting. This override is intentionally not saved.
    PlasmaKeyboardSettings::self()->setPanelFillScreenWidth(false);

    // Listen to config updates from kcm, and reparse
    auto watcher = KConfigWatcher::create(PlasmaKeyboardSettings::self()->sharedConfig());
    // clang-format off
    QObject::connect(watcher.get(),
        &KConfigWatcher::configChanged,
        &application,
        [](const KConfigGroup &, const QByteArrayList &) {
            PlasmaKeyboardSettings::self()->sharedConfig()->reparseConfiguration();
            PlasmaKeyboardSettings::self()->load();
            // Settings reloads must not enable features omitted from this build.
            if (!PLASMA_KEYBOARD_SOUNDS_ENABLED) {
                PlasmaKeyboardSettings::self()->setSoundEnabled(false);
            }
            if (!PLASMA_KEYBOARD_VIBRATION_ENABLED) {
                PlasmaKeyboardSettings::self()->setVibrationEnabled(false);
            }
            PlasmaKeyboardSettings::self()->setPanelFillScreenWidth(false);
        });
    // clang-format on

    QQmlApplicationEngine view;
    KLocalization::setupLocalizedContext(&view);

    QObject::connect(&view, &QQmlApplicationEngine::objectCreated, &application, [](QObject *object) {
        auto window = qobject_cast<QWindow *>(object);
        if (!window) {
            qCCritical(PlasmaKeyboard) << "The Plasma Keyboard root object is not a window.";
            exit(1);
        }

        // The stock keyboard uses the Wayland input-panel role. KWin always docks
        // that role to the bottom edge and reserves space for it, so moving content
        // inside the surface causes jitter and still resizes the focused window.
        // Leave the main keyboard as a normal, focusless xdg-shell tool window.
        // InputListenerItem controls its visibility when an input-method context is
        // activated, while KWin's system move operation makes it freely draggable.
        window->setVisible(false);
    });
    view.load(QUrl(QStringLiteral("qrc:/qt/qml/org/kde/plasma/keyboard/main.qml")));

    qCDebug(PlasmaKeyboard) << "Starting Plasma Keyboard application";

    return application.exec();
}
