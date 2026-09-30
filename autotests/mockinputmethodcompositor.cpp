// SPDX-FileCopyrightText: 2026 Aleix Pol <aleixpol@kde.org>
// SPDX-License-Identifier: GPL-2.0-or-later

#include <KConfig>
#include <KConfigGroup>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <QPointer>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QSettings>
#include <QtEndian>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandView>
#include <QtWaylandCompositor/QWaylandSurfaceGrabber>
#include <QtTest/QTest>

#include <memory>

#include <QtWaylandCompositor/QWaylandCompositor>
#include <QtWaylandCompositor/QWaylandCompositorExtension>
#include <QtWaylandCompositor/QWaylandCompositorExtensionTemplate>
#include <QtWaylandCompositor/QWaylandOutput>
#include <QtWaylandCompositor/QWaylandOutputMode>
#include <QtWaylandCompositor/QWaylandSurface>
#include <QtWaylandCompositor/QWaylandXdgShell>
#include <private/qxkbcommon_p.h>

#include "mockinputmethodcompositor_config.h"
#include "qwayland-server-input-method-unstable-v1.h"
#include "qwayland-server-wayland.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>

#define PLASMA_KEYBOARD_UNDER_GDB 0

using namespace Qt::StringLiterals;

static int createAnonymousKeymapFile(off_t size)
{
    int fd = -1;
#ifdef MFD_CLOEXEC
    fd = memfd_create("plasma-keyboard-keymap", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, size) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }
#endif
    char name[] = "/tmp/plasma-keyboard-keymap-XXXXXX";
    fd = mkstemp(name);
    if (fd < 0) {
        return -1;
    }
    unlink(name);
    if (ftruncate(fd, size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

class InputMethodKeyboard : public QObject, public QtWaylandServer::wl_keyboard
{
    Q_OBJECT
public:
    InputMethodKeyboard(wl_client *client, uint32_t id, int version)
        : QtWaylandServer::wl_keyboard(client, id, version)
    {
        initializeKeymap();
        m_timer.start();
    }

    void setFocusSurface(wl_resource *surface)
    {
        if (!surface || m_focusSurface == surface) {
            return;
        }
        m_focusSurface = surface;
        send_enter(++m_serial, m_focusSurface, QByteArray());
    }

    void sendKey(uint32_t key, uint32_t state)
    {
        send_key(++m_serial, static_cast<uint32_t>(m_timer.elapsed()), key, state);
    }

    bool keymapped() const
    {
        return m_keymapped;
    }

    /**
     * Sets an updated XKB keymap built from the given layout and variant.
     * Use this to simulate a compositor keymap change (e.g. switching to us/intl
     * so that dead keys produce the correct keysyms in the child process).
     *
     * @param layout XKB layout name (e.g. "us")
     * @param variant XKB variant name (e.g. "intl"), or nullptr for none
     */
    void setKeymap(const char *layout, const char *variant = nullptr)
    {
        QXkbCommon::ScopedXKBContext context(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if (!context) {
            qWarning() << "Failed to create xkb context";
            return;
        }

        xkb_rule_names names = {};
        names.rules = "evdev";
        names.layout = layout;
        names.variant = variant ? variant : "";
        names.options = "compose:menu";

        QXkbCommon::ScopedXKBKeymap keymap(xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!keymap) {
            qWarning() << "Failed to create xkb keymap for layout" << layout << variant;
            return;
        }

        char *mapStr = xkb_keymap_get_as_string(keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
        if (!mapStr) {
            qWarning() << "Failed to get xkb keymap string";
            return;
        }

        const QByteArray mapData(mapStr);
        free(mapStr);

        const int fd = createAnonymousKeymapFile(mapData.size() + 1);
        if (fd < 0) {
            qWarning() << "Failed to create keymap file";
            return;
        }

        if (write(fd, mapData.constData(), mapData.size()) != mapData.size()) {
            close(fd);
            qWarning() << "Failed to write keymap";
            return;
        }
        if (write(fd, "\0", 1) != 1) {
            close(fd);
            qWarning() << "Failed to write keymap terminator";
            return;
        }
        if (lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            qWarning() << "Failed to seek keymap file";
            return;
        }

        send_keymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, mapData.size() + 1);
        close(fd);
    }

Q_SIGNALS:
    void keymapDone();

private:
    void initializeKeymap()
    {
        setKeymap("us");
        m_keymapped = true;
        Q_EMIT keymapDone();
    }

    bool m_keymapped = false;
    uint32_t m_serial = 0;
    QElapsedTimer m_timer;
    wl_resource *m_focusSurface = nullptr;
};

class InputMethodContext : public QObject, public QtWaylandServer::zwp_input_method_context_v1
{
    Q_OBJECT

public:
    explicit InputMethodContext(wl_resource *focusSurface, QObject *parent = nullptr)
        : QObject(parent)
        , m_focusSurface(focusSurface)
    {
    }

    InputMethodKeyboard *keyboard() const
    {
        return m_keyboard.get();
    }

Q_SIGNALS:
    void keyboardGrabbed();
    void commitStringChanged(const QString &commitString);
    void keysymReceived(uint32_t sym, uint32_t state);
    void surroundingDeleted(int index, uint length);

protected:
    void zwp_input_method_context_v1_destroy(Resource *resource) override
    {
        wl_resource_destroy(resource->handle);
    }

    void zwp_input_method_context_v1_commit_string(Resource *resource, uint32_t serial, const QString &text) override
    {
        Q_UNUSED(resource);
        qInfo().noquote() << "commit_string" << serial << text;
        Q_EMIT commitStringChanged(text);
    }

    void zwp_input_method_context_v1_preedit_string(Resource *resource, uint32_t serial, const QString &text, const QString &commit) override
    {
        Q_UNUSED(resource);
        qInfo().noquote() << "preedit_string" << serial << text << commit;
    }

    void zwp_input_method_context_v1_delete_surrounding_text(Resource *resource, int32_t index, uint32_t length) override
    {
        Q_UNUSED(resource);
        qInfo() << "delete_surrounding_text" << index << length;
        Q_EMIT surroundingDeleted(index, length);
    }

    void zwp_input_method_context_v1_keysym(Resource *resource, uint32_t serial, uint32_t time, uint32_t sym, uint32_t state, uint32_t modifiers) override
    {
        Q_UNUSED(resource);
        qInfo() << "keysym" << serial << time << sym << state << modifiers;
        Q_EMIT keysymReceived(sym, state);
    }

    void zwp_input_method_context_v1_grab_keyboard(Resource *resource, uint32_t keyboard) override
    {
        m_keyboard = std::make_unique<InputMethodKeyboard>(resource->client(), keyboard, resource->version());
        m_keyboard->setFocusSurface(m_focusSurface);
        Q_EMIT keyboardGrabbed();
        qInfo() << "input_method_context grab_keyboard";
    }

private:
    std::unique_ptr<InputMethodKeyboard> m_keyboard;
    wl_resource *m_focusSurface = nullptr;
};

class InputPanelSurface : public QObject, public QtWaylandServer::zwp_input_panel_surface_v1
{
    Q_OBJECT

public:
    InputPanelSurface(QWaylandSurface *surface, wl_client *client, uint32_t id, int version, QObject *parent = nullptr)
        : QObject(parent)
        , QtWaylandServer::zwp_input_panel_surface_v1(client, id, version)
        , m_surface(surface)
    {
    }

Q_SIGNALS:
    void toplevelRequested();
    void overlayRequested();

protected:
    void zwp_input_panel_surface_v1_set_toplevel(Resource *resource, wl_resource *output, uint32_t position) override
    {
        Q_UNUSED(resource);
        Q_UNUSED(output);
        Q_UNUSED(position);
        Q_EMIT toplevelRequested();
        qInfo() << "input_panel_surface set_toplevel";
    }

    void zwp_input_panel_surface_v1_set_overlay_panel(Resource *resource) override
    {
        Q_UNUSED(resource);
        Q_EMIT overlayRequested();
        qInfo() << "input_panel_surface set_overlay_panel";
    }

    void zwp_input_panel_surface_v1_destroy_resource(Resource *resource) override
    {
        Q_UNUSED(resource);
        delete this;
    }

private:
    QWaylandSurface *m_surface = nullptr;
};

class InputPanelV1 : public QWaylandCompositorExtensionTemplate<InputPanelV1>, public QtWaylandServer::zwp_input_panel_v1
{
    Q_OBJECT

public:
    explicit InputPanelV1(QWaylandCompositor *compositor)
        : QWaylandCompositorExtensionTemplate<InputPanelV1>(compositor)
    {
    }

    void initialize() override
    {
        QWaylandCompositorExtensionTemplate::initialize();
        auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
        if (!compositor) {
            qWarning() << "No compositor available when initializing input panel";
            return;
        }
        init(compositor->display(), interfaceVersion());
    }

    int overlayPanelCount() const
    {
        return m_overlayPanelCount;
    }

    int toplevelPanelCount() const
    {
        return m_toplevelPanelCount;
    }

    wl_resource *lastSurfaceResource() const
    {
        return m_lastSurfaceResource;
    }

Q_SIGNALS:
    void inputPanelSurfaceCreated();
    void overlayPanelRequested();
    void toplevelPanelRequested();
    void keyboardSurfaceRequested(QWaylandSurface *surface);

protected:
    void zwp_input_panel_v1_get_input_panel_surface(Resource *resource, uint32_t id, wl_resource *surface) override
    {
        auto *wlSurface = QWaylandSurface::fromResource(surface);
        if (!wlSurface) {
            qWarning() << "input_panel_surface requested for unknown surface";
            return;
        }

        m_lastSurfaceResource = surface;
        connect(wlSurface, &QObject::destroyed, this, [this, surface] {
            if (m_lastSurfaceResource == surface)
                m_lastSurfaceResource = nullptr;
        });
        auto *panelSurface = new InputPanelSurface(wlSurface, resource->client(), id, resource->version(), this);
        connect(panelSurface, &InputPanelSurface::toplevelRequested, this, [this, wlSurface] {
            Q_EMIT keyboardSurfaceRequested(wlSurface);
            ++m_toplevelPanelCount;
            Q_EMIT toplevelPanelRequested();
        });
        connect(panelSurface, &InputPanelSurface::overlayRequested, this, [this] {
            ++m_overlayPanelCount;
            Q_EMIT overlayPanelRequested();
        });
        Q_EMIT inputPanelSurfaceCreated();
        qInfo() << "input_panel_surface created";
    }

private:
    int m_overlayPanelCount = 0;
    int m_toplevelPanelCount = 0;
    wl_resource *m_lastSurfaceResource = nullptr;
};

class InputMethodV1 : public QWaylandCompositorExtensionTemplate<InputMethodV1>, public QtWaylandServer::zwp_input_method_v1
{
    Q_OBJECT
public:
    explicit InputMethodV1(QWaylandCompositor *compositor)
        : QWaylandCompositorExtensionTemplate<InputMethodV1>(compositor)
    {
    }

    void setInputPanel(InputPanelV1 *inputPanel)
    {
        m_inputPanel = inputPanel;
    }

    void initialize() override
    {
        QWaylandCompositorExtensionTemplate::initialize();
        auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
        if (!compositor) {
            qWarning() << "No compositor available when initializing input method";
            return;
        }
        init(compositor->display(), interfaceVersion());
    }

    void sendActivate()
    {
        if (m_context) {
            return;
        }

        const wl_resource *focusSurface = m_inputPanel ? m_inputPanel->lastSurfaceResource() : nullptr;
        m_context = std::make_unique<InputMethodContext>(const_cast<wl_resource *>(focusSurface), this);
        for (auto *resource : resourceMap()) {
            auto *contextResource = m_context->add(resource->client(), resource->version());
            send_activate(resource->handle, contextResource->handle);
        }
        qInfo() << "input_method_v1 activated";
        Q_EMIT activated();
    }

    void sendDeactivate()
    {
        if (!m_context) {
            return;
        }

        for (auto *resource : resourceMap()) {
            auto *contextResource = m_context->resourceMap().value(resource->client());
            if (contextResource) {
                send_deactivate(resource->handle, contextResource->handle);
            }
        }
        m_context.reset();
        qInfo() << "input_method_v1 deactivated";
    }

    InputMethodContext *context() const
    {
        return m_context.get();
    }

Q_SIGNALS:
    void activated();

protected:
    void zwp_input_method_v1_bind_resource(Resource *resource) override
    {
        if (m_context) {
            auto *contextResource = m_context->add(resource->client(), resource->version());
            send_activate(resource->handle, contextResource->handle);
            return;
        }

        if (!m_autoActivate || m_activatePending) {
            return;
        }

        m_activatePending = true;
        QTimer::singleShot(m_activateDelayMs, this, [this] {
            m_activatePending = false;
            sendActivate();
        });
    }

private:
    const bool m_autoActivate = true;
    const int m_activateDelayMs = 200;

    bool m_activatePending = false;
    std::unique_ptr<InputMethodContext> m_context;
    InputPanelV1 *m_inputPanel = nullptr;
};

class MockScreenSaver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    bool active = false;
    void setActive(bool value)
    {
        active = value;
        Q_EMIT ActiveChanged(value);
    }
public Q_SLOTS:
    bool GetActive() const
    {
        return active;
    }
Q_SIGNALS:
    void ActiveChanged(bool active);
};

class MockInputMethodCompositorTest : public QObject
{
    Q_OBJECT

public:
    MockInputMethodCompositorTest() = default;

private Q_SLOTS:
    void initTestCase()
    {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY2(bus.registerService(QStringLiteral("org.freedesktop.ScreenSaver")), "Run this test inside dbus-run-session");
        QVERIFY(bus.registerObject(QStringLiteral("/ScreenSaver"), &m_screenSaver, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        // create a temporary folder for test configs
        if (!m_home.isValid() || !qputenv("XDG_CONFIG_HOME", qPrintable(m_home.path()))) {
            qFatal("Couldn't create temporary home folder for the test");
            return;
        }

        {
            KConfig cfg(QStringLiteral("plasmakeyboardrc"));
            KConfigGroup grp(&cfg, QStringLiteral("General"));
            grp.writeEntry(QStringLiteral("enabledLocales"), QStringLiteral("it_IT"));
        }

        m_compositor = std::make_unique<QWaylandCompositor>();
        m_socketPath = m_runtimeDir.path() + QLatin1String("/plasma-keyboard-mock-") + QString::number(QCoreApplication::applicationPid());
        m_compositor->setSocketName(m_socketPath.toUtf8());
        m_compositor->setUseHardwareIntegrationExtension(false);
        m_compositor->create();
        QTRY_VERIFY_WITH_TIMEOUT(m_compositor->isCreated(), 2000);

        m_outputWindow = std::make_unique<QWindow>();
        m_outputWindow->setGeometry(0, 0, 1280, 720);
        m_outputWindow->setTitle(u"Mock Compositor Output"_s);
        m_outputWindow->setVisible(false);

        m_output = std::make_unique<QWaylandOutput>(m_compositor.get(), m_outputWindow.get());
        m_output->setManufacturer(u"Mock"_s);
        m_output->setModel(u"InputMethod"_s);
        m_output->addMode(QWaylandOutputMode(QSize(1280, 720), 60000), true);
        m_output->setCurrentMode(m_output->modes().first());
        m_compositor->setDefaultOutput(m_output.get());

        m_xdgShell = std::make_unique<QWaylandXdgShell>(m_compositor.get());
        m_xdgShell->initialize();
        connect(m_xdgShell.get(), &QWaylandXdgShell::toplevelCreated, this,
                [this](QWaylandXdgToplevel *toplevel, QWaylandXdgSurface *xdgSurface) {
            m_toplevel = toplevel;
            m_surface = xdgSurface->surface();
            m_view = std::make_unique<QWaylandView>();
            m_view->setSurface(m_surface);
            m_view->setOutput(m_output.get());
            // The keyboard must type without ever receiving seat keyboard focus
            // or the xdg Activated state, just like the focusless KWin rule.
            toplevel->sendConfigure(QSize(0, 0), QList<QWaylandXdgToplevel::State>{});
        });
        connect(&m_frameTimer, &QTimer::timeout, this, [this] {
            if (m_view && m_surface) {
                m_view->advance();
                m_surface->frameStarted();
                m_surface->sendFrameCallbacks();
            }
            // Activation must also reach clients before they have a visible surface.
            wl_display_flush_clients(m_compositor->display());
        });
        m_frameTimer.start(16);

        m_inputMethod = std::make_unique<InputMethodV1>(m_compositor.get());
        m_inputMethod->initialize();

        m_inputPanel = std::make_unique<InputPanelV1>(m_compositor.get());
        m_inputPanel->initialize();
        m_inputMethod->setInputPanel(m_inputPanel.get());
        connect(m_inputPanel.get(), &InputPanelV1::keyboardSurfaceRequested, this, [this](QWaylandSurface *surface) {
            m_surface = surface;
            m_view = std::make_unique<QWaylandView>();
            m_view->setSurface(surface);
            m_view->setOutput(m_output.get());
        });

        m_child = std::make_unique<QProcess>();
        connect(m_child.get(), &QProcess::readyReadStandardError, this, [this] {
            QTextStream(stderr) << m_child->readAllStandardError();
        });
        connect(m_child.get(), &QProcess::readyReadStandardOutput, this, [this] {
            QTextStream(stdout) << m_child->readAllStandardOutput();
        });
        connect(m_child.get(), &QProcess::finished, this, [this] {
            qWarning() << "child state:" << m_child->state() << "error:" << m_child->error() << m_child->errorString() << "exitStatus:" << m_child->exitStatus()
                       << "exitCode:" << m_child->exitCode();
        });
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(u"WAYLAND_DISPLAY"_s, m_socketPath);
        env.insert(u"QT_QPA_PLATFORM"_s, u"wayland"_s);
        env.insert(u"QT_QPA_PLATFORMTHEME"_s, u"generic"_s);
        env.insert(u"QT_QUICK_BACKEND"_s, u"software"_s); // Without this plasma-keyboard explodes on alpine for some reason
        m_child->setProcessEnvironment(env);

#if PLASMA_KEYBOARD_UNDER_GDB
        m_child->setProgram(QStringLiteral("gdb"));
        m_child->setArguments({QStringLiteral("-batch"),
                               QStringLiteral("-ex"),
                               QStringLiteral("set pagination off"),
                               QStringLiteral("-ex"),
                               QStringLiteral("run"),
                               QStringLiteral("-ex"),
                               QStringLiteral("bt"),
                               QStringLiteral("-ex"),
                               QStringLiteral("quit"),
                               QStringLiteral("--args"),
                               QStringLiteral(PLASMA_KEYBOARD_BINARY_PATH)});
#else
        m_child->setProgram(QStringLiteral(PLASMA_KEYBOARD_BINARY_PATH));
#endif
        m_child->start();
        QVERIFY2(m_child->waitForStarted(), qPrintable(m_child->errorString()));

        qInfo().noquote().nospace() << "Compositor running on WAYLAND_DISPLAY=" << m_socketPath << " " << PLASMA_KEYBOARD_BINARY_PATH;
        {
            QSignalSpy spy(m_inputMethod.get(), &InputMethodV1::activated);
            QVERIFY(spy.count() || spy.wait());
        }

        QVERIFY(m_inputMethod->context());
        if (!m_inputMethod->context()->keyboard()) {
            QSignalSpy grabSpy(m_inputMethod->context(), &InputMethodContext::keyboardGrabbed);
            QVERIFY(grabSpy.wait());
        }

        auto *keyboard = m_inputMethod->context()->keyboard();
        QVERIFY(keyboard);
        if (!keyboard->keymapped()) {
            QSignalSpy grabSpy(keyboard, &InputMethodKeyboard::keymapDone);
            QVERIFY(grabSpy.wait());
        }
    }

    void sendKey(int key, int interval)
    {
        auto keyboard = m_inputMethod->context()->keyboard();
        Q_ASSERT(keyboard);
        keyboard->sendKey(key, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(interval);
        keyboard->sendKey(key, WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_display_flush_clients(m_compositor->display());
    }

    /**
     * Sets an updated keymap and waits for it to be processed.
     *
     * @param layout XKB layout name (e.g. "us")
     * @param variant XKB variant name (e.g. "intl"), or nullptr for none
     */
    void setKeymap(const char *layout, const char *variant = nullptr)
    {
        auto *keyboard = m_inputMethod->context()->keyboard();
        Q_ASSERT(keyboard);
        keyboard->setKeymap(layout, variant);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(200);
    }

    void testFloatingKeyboardInteractions()
    {
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        QTRY_VERIFY(m_surface->destinationSize().width() > 400);
        QVERIFY(!m_compositor->defaultSeat()->keyboardFocus());
        QVERIFY(!m_toplevel->activated());
        QTest::qWait(300);

        // Save the actual rendered keyboard for visual review.
        if (qEnvironmentVariableIsSet("FLOATING_KEYBOARD_TEST_IMAGE")) {
            QWaylandSurfaceGrabber grabber(m_surface);
            QSignalSpy grabbed(&grabber, &QWaylandSurfaceGrabber::success);
            grabber.grab();
            QTRY_COMPARE(grabbed.count(), 1);
            QVERIFY(qvariant_cast<QImage>(grabbed.first().first()).save(qEnvironmentVariable("FLOATING_KEYBOARD_TEST_IMAGE")));
        }

        auto *seat = m_compositor->defaultSeat();
        auto move = [this, seat](QPointF pos) {
            seat->sendMouseMoveEvent(m_view.get(), pos, pos);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
        };
        auto click = [this, seat, &move](QPointF pos) {
            move(pos);
            seat->sendMousePressEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(60);
        };

        const QSize initialSize = m_surface->destinationSize();
        qInfo() << "Floating keyboard size" << initialSize;
        QSignalSpy commitSpy(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        // Top letter row, first key (q), below the drag handle.
        click(QPointF(initialSize.width() * 0.065, 80 + (initialSize.height() - 116) * 0.125));
        QTRY_COMPARE(commitSpy.count(), 1);
        QCOMPARE(commitSpy.first().first().toString(), QStringLiteral("q"));
        qInfo() << "Pointer typed" << commitSpy.first().first();
        QVERIFY(!m_compositor->defaultSeat()->keyboardFocus());

        // Exercise the actual Wayland touch -> QML resize handler path.
        QPointF grip(initialSize.width() - 16, initialSize.height() - 16);
        seat->sendTouchPointPressed(m_surface, 0, grip);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(50);
        for (int dx : {2, 10, 30, 60, 90}) {
            seat->sendTouchPointMoved(m_surface, 0, grip + QPointF(dx, dx / 3.0));
            seat->sendTouchFrameEvent(m_surface->client());
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
        }
        seat->sendTouchPointReleased(m_surface, 0, grip + QPointF(90, 30));
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTRY_VERIFY(m_surface->destinationSize().width() > initialSize.width() + 50);
        QVERIFY(m_surface->destinationSize().height() > initialSize.height());

        const QSize resized = m_surface->destinationSize();
        move(QPointF(resized.width() - 16, resized.height() - 16));
        seat->sendMousePressEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(50);
        for (int dx : {2, 10, 30, 60}) {
            move(QPointF(resized.width() - 16 - dx, resized.height() - 16 - dx / 3.0));
        }
        seat->sendMouseReleaseEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTRY_VERIFY(m_surface->destinationSize().width() < resized.width() - 30);

        QSignalSpy moveSpy(m_toplevel, &QWaylandXdgToplevel::startMove);
        move(QPointF(160, 16));
        seat->sendMousePressEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        move(QPointF(164, 16));
        seat->sendMouseReleaseEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(moveSpy.count(), 1);

        seat->sendTouchPointPressed(m_surface, 0, QPointF(160, 16));
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        seat->sendTouchPointMoved(m_surface, 0, QPointF(164, 16));
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        seat->sendTouchPointReleased(m_surface, 0, QPointF(164, 16));
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(moveSpy.count(), 2);

        // Dismiss using the header arrow and ensure a surrounding-text echo
        // cannot immediately reopen the panel.
        click(QPointF(m_surface->destinationSize().width() - 16, 16));
        QTRY_VERIFY(!m_surface || !m_surface->hasContent());
        auto *context = m_inputMethod->context();
        for (auto *resource : context->resourceMap()) {
            context->send_surrounding_text(resource->handle, QStringLiteral("q"), 1, 1);
        }
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(300);
        QVERIFY(!m_surface || !m_surface->hasContent());

        // A genuinely new input context should show a usable keyboard again.
        m_inputMethod->sendDeactivate();
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(250);
        m_inputMethod->sendActivate();
        wl_display_flush_clients(m_compositor->display());
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        QTest::qWait(200);
        const QSize reopened = m_surface->destinationSize();
        QSignalSpy touchCommitSpy(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        QPointF qKey(reopened.width() * 0.065, 80 + (reopened.height() - 116) * 0.125);
        seat->sendTouchPointPressed(m_surface, 0, qKey);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        seat->sendTouchPointReleased(m_surface, 0, qKey);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(touchCommitSpy.count(), 1);
        QCOMPARE(touchCommitSpy.first().first().toString(), QStringLiteral("q"));

        // The original bottom-row hide key uses Qt's input-method API.
        click(QPointF(reopened.width() * 0.79, 80 + (reopened.height() - 116) * 0.875));
        QTRY_VERIFY(!m_surface || !m_surface->hasContent());
        m_inputMethod->sendDeactivate();
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(250);
        m_inputMethod->sendActivate();
        wl_display_flush_clients(m_compositor->display());
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        QTest::qWait(200);
    }

    void testNativeResizePresetsAndManualShow()
    {
        auto *seat = m_compositor->defaultSeat();
        auto move = [this, seat](QPointF pos) {
            seat->sendMouseMoveEvent(m_view.get(), pos, pos);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
        };
        auto click = [this, seat, &move](QPointF pos) {
            move(pos);
            seat->sendMousePressEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(80);
        };
        QSignalSpy resizeSpy(m_toplevel, &QWaylandXdgToplevel::startResize);
        move(QPointF(3, 120));
        seat->sendMousePressEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        move(QPointF(6, 120));
        seat->sendMouseReleaseEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(resizeSpy.count(), 1);
        QCOMPARE(qvariant_cast<Qt::Edges>(resizeSpy.first().at(1)), Qt::Edges(Qt::LeftEdge));
        m_toplevel->sendConfigure(QSize(620, 360), QList<QWaylandXdgToplevel::State>{});
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(m_surface->destinationSize(), QSize(620, 360));

        // The size preset is reversible and the new shape is persisted.
        click(QPointF(620 - 90, 18));
        QTRY_COMPARE(m_surface->destinationSize(), QSize(800, 384));
        click(QPointF(800 - 90, 18));
        QTRY_COMPARE(m_surface->destinationSize(), QSize(560, 304));
        QTest::qWait(400);
        QSettings settings(QStringLiteral("kde.org"), QStringLiteral("plasma-keyboard"));
        QTRY_COMPARE_WITH_TIMEOUT(([&settings] { settings.sync(); return settings.value(QStringLiteral("FloatingKeyboard/keyboardWidth")).toInt(); })(), 560, 3000);
        QCOMPARE(settings.value(QStringLiteral("FloatingKeyboard/keyboardHeight")).toInt(), 304);

        click(QPointF(560 - 54, 18));
        QTest::qWait(100);
        KConfig config(QStringLiteral("plasmakeyboardrc"));
        KConfigGroup general(&config, QStringLiteral("General"));
        QVERIFY(general.readEntry(QStringLiteral("soundEnabled"), false));
        click(QPointF(560 - 54, 18));
        config.reparseConfiguration();
        QVERIFY(!general.readEntry(QStringLiteral("soundEnabled"), false));

        click(QPointF(560 - 18, 18));
        QTRY_VERIFY(!m_surface || !m_surface->hasContent());
        QDBusInterface keyboard(QStringLiteral("org.kde.plasma.keyboard.Floating"), QStringLiteral("/Keyboard"),
                                QStringLiteral("org.kde.plasma.keyboard.Floating"));
        QVERIFY(keyboard.isValid());
        keyboard.asyncCall(QStringLiteral("showKeyboard"));
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        QSignalSpy commits(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        click(QPointF(560 * 0.065, 80 + (304 - 116) * 0.125));
        QTRY_COMPARE(commits.count(), 1);
        QCOMPARE(commits.first().first().toString(), QStringLiteral("q"));
        QVERIFY(!seat->keyboardFocus());
    }

    void testKeyClickAudio()
    {
        // Opt in only with an isolated sink monitor; never record a microphone
        // or the user's normal output. The child inherits PULSE_SINK.
        const QString monitor = qEnvironmentVariable("FLOATING_KEYBOARD_AUDIO_MONITOR");
        if (monitor.isEmpty()) {
            QSKIP("Set FLOATING_KEYBOARD_AUDIO_MONITOR and PULSE_SINK to a private test sink");
        }
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        auto *seat = m_compositor->defaultSeat();
        const QSize size = m_surface->destinationSize();
        auto click = [this, seat](QPointF pos) {
            seat->sendMouseMoveEvent(m_view.get(), pos, pos);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMousePressEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(250);
        };
        QProcess capture;
        capture.start(QStringLiteral("parec"), {QStringLiteral("--device=") + monitor,
            QStringLiteral("--format=s16le"), QStringLiteral("--rate=48000"), QStringLiteral("--channels=1"),
            QStringLiteral("--latency-msec=20"), QStringLiteral("--raw")});
        QVERIFY(capture.waitForStarted());
        QTest::qWait(1200);
        QVERIFY(capture.bytesAvailable() > 0);
        capture.readAllStandardOutput();
        const QPointF qKey(size.width() * 0.065, 80 + (size.height() - 116) * 0.125);
        const QPointF speaker(size.width() - 54, 18);
        QSignalSpy commits(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        for (int i = 0; i < 3; ++i) {
            click(qKey);
        }
        QTest::qWait(300);
        const QByteArray muted = capture.readAllStandardOutput();
        click(speaker);
        QTest::qWait(300);
        capture.readAllStandardOutput();
        click(qKey);
        seat->sendTouchPointPressed(m_surface, 0, qKey);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(80);
        seat->sendTouchPointReleased(m_surface, 0, qKey);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(300);
        click(qKey);
        QTest::qWait(300);
        const QByteArray audible = capture.readAllStandardOutput();
        click(speaker);
        capture.terminate();
        capture.waitForFinished();
        auto peak = [](const QByteArray &pcm) {
            int result = 0;
            for (qsizetype i = 0; i + 1 < pcm.size(); i += 2) {
                result = qMax(result, qAbs(int(qFromLittleEndian<qint16>(pcm.constData() + i))));
            }
            return result;
        };
        qInfo() << "Audio PCM peak: muted" << peak(muted) << "enabled" << peak(audible)
                << "captured bytes" << muted.size() << audible.size();
        QVERIFY(muted.size() > 48000);
        QVERIFY(audible.size() > 48000);
        QCOMPARE(peak(muted), 0);
        QVERIFY2(peak(audible) > 200, "Enabling sound must produce actual audio, not just change the setting");
        QTRY_COMPARE(commits.count(), 6);
        QVERIFY(!seat->keyboardFocus());
    }

    void testLongPressShowsOverlayPanel()
    {
        QSignalSpy overlaySpy(m_inputPanel.get(), &InputPanelV1::overlayPanelRequested);

        sendKey(KEY_A, 1200);
        QVERIFY(overlaySpy.count() || overlaySpy.wait());

        QSignalSpy commitStringSpy(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        sendKey(KEY_1, 10);
        QVERIFY(commitStringSpy.count() || commitStringSpy.wait());
        QCOMPARE(commitStringSpy.count(), 1);
        QCOMPARE(commitStringSpy.first().first().toString(), QStringLiteral("à"));
    }

    void testTypingAfterOverlay()
    {
        QVERIFY(m_surface && m_surface->hasContent());
        const QSize size = m_surface->destinationSize();
        auto *seat = m_compositor->defaultSeat();
        QSignalSpy commits(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        seat->sendMouseMoveEvent(m_view.get(), QPointF(size.width() * 0.065, 80 + (size.height() - 116) * 0.125));
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        seat->sendMousePressEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(40);
        seat->sendMouseReleaseEvent(Qt::LeftButton);
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(commits.count(), 1);
        QCOMPARE(commits.first().first().toString(), QStringLiteral("q"));
        QVERIFY(!seat->keyboardFocus());
    }

    /**
     * Test that a Multi_key compose sequence (Multi_key + t + m → ™) is handled
     * locally by the XKB compose state machine in OverlayController. The
     * intermediate keys must be consumed and not forwarded to the compositor;
     * only the composed result should arrive via commit_string.
     */
    void testComposeSequence()
    {
        // Skip if no XKB compose table is available for the current locale.
        // Without one, OverlayController::m_xkbComposeState is null and the
        // compose sequence would not be processed locally.
        {
            QXkbCommon::ScopedXKBContext xkbContext(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
            if (!xkbContext) {
                QSKIP("Could not create XKB context");
            }
            const char *locale = setlocale(LC_CTYPE, nullptr);
            if (!locale || locale[0] == '\0') {
                locale = "C";
            }
            xkb_compose_table *composeTable = xkb_compose_table_new_from_locale(xkbContext.get(), locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
            if (!composeTable) {
                QSKIP("No XKB compose table available for the current locale");
            }
            xkb_compose_table_unref(composeTable);
        }

        QSignalSpy overlaySpy(m_inputPanel.get(), &InputPanelV1::overlayPanelRequested);
        QSignalSpy commitStringSpy(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        QSignalSpy keysymSpy(m_inputMethod->context(), &InputMethodContext::keysymReceived);

        // Send the compose sequence: Multi_key → t → m → ™
        // KEY_COMPOSE (127) maps to Multi_key via the compose:menu keymap option.
        sendKey(KEY_COMPOSE, 10);
        sendKey(KEY_T, 10);
        sendKey(KEY_M, 10);

        // Exactly one commit_string with the composed character should arrive.
        QTRY_COMPARE(commitStringSpy.count(), 1);
        QCOMPARE(commitStringSpy.first().first().toString(), QStringLiteral("™"));

        // The intermediate keys (t, m) must not have been forwarded via keysym.
        // XKB_KEY_t = 0x74, XKB_KEY_m = 0x6d
        for (const QList<QVariant> &args : keysymSpy) {
            const uint32_t sym = args.at(0).toUInt();
            QVERIFY2(sym != 0x74 && sym != 0x6d, "A compose sequence key (t or m) was incorrectly forwarded via keysym");
        }

        // No overlay should have appeared during the compose sequence.
        QTest::qWait(200);
        QVERIFY(overlaySpy.isEmpty());
    }

    /**
     * Test that pressing a dead key (e.g. the grave key `) followed by a character key (e.g. a) results
     * in the correct composed character (e.g. à) being committed, without the dead key
     * being forwarded as a separate keysym or commit_string.
     */
    void testDeadKeySequence()
    {
        // Switch to the US intl with dead keys layout for the test.
        setKeymap("us", "intl");

        QSignalSpy commitStringSpy(m_inputMethod->context(), &InputMethodContext::commitStringChanged);
        QSignalSpy keysymSpy(m_inputMethod->context(), &InputMethodContext::keysymReceived);

        // Send the dead key sequence: ` → a = à
        sendKey(KEY_GRAVE, 10);
        sendKey(KEY_A, 10);

        // Exactly one commit_string with the composed character should arrive.
        QTRY_COMPARE(commitStringSpy.count(), 1);
        QCOMPARE(commitStringSpy.first().first().toString(), QStringLiteral("à"));

        // The dead key (`) must not have been forwarded via keysym.
        // XKB_KEY_grave = 0x60
        for (const QList<QVariant> &args : keysymSpy) {
            const uint32_t sym = args.at(0).toUInt();
            QVERIFY2(sym != 0x60, "The dead key (`) was incorrectly forwarded via keysym");
        }

        // Set the keymap back to the default to avoid affecting other tests.
        setKeymap("us", nullptr);
    }

    void testWordSuggestionsAndSwedishLayout()
    {
        KConfig config(QStringLiteral("plasmakeyboardrc"));
        KConfigGroup general(&config, QStringLiteral("General"));
        general.writeEntry(QStringLiteral("enabledLocales"), QStringList{QStringLiteral("en_US"), QStringLiteral("sv_SE")}, KConfig::Notify);
        config.sync();
        QTest::qWait(500);
        m_toplevel->sendConfigure(QSize(800, 384), QList<QWaylandXdgToplevel::State>{});
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(m_surface->destinationSize(), QSize(800, 384));
        QTest::qWait(200);
        auto *seat = m_compositor->defaultSeat();
        auto *context = m_inputMethod->context();
        auto click = [this, seat](QPointF point) {
            seat->sendMouseMoveEvent(m_view.get(), point, point);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMousePressEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(40);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(150);
        };
        auto surrounding = [this, context](const QString &text, int cursor, int anchor) {
            for (auto *resource : context->resourceMap()) {
                context->send_surrounding_text(resource->handle, text, cursor, anchor);
            }
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(180);
        };
        auto saveImage = [this](const QString &name) {
            if (qEnvironmentVariableIsSet("FLOATING_KEYBOARD_SUGGESTION_IMAGES")) {
                QWaylandSurfaceGrabber grabber(m_surface);
                QSignalSpy grabbed(&grabber, &QWaylandSurfaceGrabber::success);
                grabber.grab();
                QTRY_COMPARE(grabbed.count(), 1);
                QVERIFY(
                    qvariant_cast<QImage>(grabbed.first().first()).save(qEnvironmentVariable("FLOATING_KEYBOARD_SUGGESTION_IMAGES") + QLatin1Char('/') + name));
            }
        };
        surrounding(QString(), 0, 0);
        QSignalSpy commits(context, &InputMethodContext::commitStringChanged);
        QSignalSpy deletions(context, &InputMethodContext::surroundingDeleted);
        // Actually type "hel" through the focusless keyboard, without waiting
        // for application echoes, then tap its highest-frequency completion.
        click(QPointF(0.55 * 800, 80 + 268 * 0.375)); // h
        click(QPointF(0.25 * 800, 80 + 268 * 0.125)); // e
        click(QPointF(0.85 * 800, 80 + 268 * 0.375)); // l
        QTRY_COMPARE(commits.count(), 3);
        QCOMPARE(commits[0][0].toString() + commits[1][0].toString() + commits[2][0].toString(), QStringLiteral("hel"));
        saveImage(QStringLiteral("suggestions-english.png"));
        click(QPointF(184, 58));
        QTRY_COMPARE(commits.count(), 4);
        QCOMPARE(commits.last().first().toString(), QStringLiteral("p "));
        QCOMPARE(deletions.count(), 0);

        // Moving into a word or selecting text must disable suffix completion.
        surrounding(QStringLiteral("helpful"), 3, 3);
        click(QPointF(184, 58));
        QCOMPARE(commits.count(), 4);
        surrounding(QStringLiteral("hel"), 3, 0);
        click(QPointF(184, 58));
        QCOMPARE(commits.count(), 4);
        for (auto *resource : context->resourceMap()) {
            context->send_content_type(resource->handle, 0xc0, 8); // password
        }
        surrounding(QStringLiteral("hel"), 3, 3);
        click(QPointF(184, 58));
        QCOMPARE(commits.count(), 4);
        for (auto *resource : context->resourceMap()) {
            context->send_content_type(resource->handle, 0, 0);
        }
        surrounding(QString(), 0, 0);

        // Switch layout without changing compositor focus; then type å, ä, ö.
        click(QPointF(35, 58));
        QSettings settings(QStringLiteral("kde.org"), QStringLiteral("plasma-keyboard"));
        QTRY_COMPARE_WITH_TIMEOUT(([&settings] {
                                      settings.sync();
                                      return settings.value(QStringLiteral("FloatingKeyboard/keyboardLocale")).toString();
                                  })(),
                                  QStringLiteral("sv_SE"),
                                  3000);
        click(QPointF(0.95 * 800, 80 + 268 * 0.125));
        click(QPointF(0.95 * 800, 80 + 268 * 0.375));
        click(QPointF(0.866 * 800, 80 + 268 * 0.375));
        QTRY_COMPARE(commits.count(), 7);
        QCOMPARE(commits[4][0].toString() + commits[5][0].toString() + commits[6][0].toString(), QStringLiteral("åäö"));
        const QString swedish = QStringLiteral("Jag äter smö");
        surrounding(swedish, swedish.toUtf8().size(), swedish.toUtf8().size());
        saveImage(QStringLiteral("suggestions-swedish.png"));
        // Accept a Swedish completion by touch, preserving the existing UTF-8 text.
        const QPointF suggestion(184, 58);
        seat->sendTouchPointPressed(m_surface, 0, suggestion);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTest::qWait(80);
        seat->sendTouchPointReleased(m_surface, 0, suggestion);
        seat->sendTouchFrameEvent(m_surface->client());
        wl_display_flush_clients(m_compositor->display());
        QTRY_COMPARE(commits.count(), 8);
        QCOMPARE(commits.last().first().toString(), QStringLiteral("r "));
        QCOMPARE(deletions.count(), 0);
        QVERIFY(!seat->keyboardFocus());
        QVERIFY(!m_toplevel->activated());
    }

    void testLockScreenSurfaceLifecycle()
    {
        auto seat = m_compositor->defaultSeat();
        const QSize desktopSize = m_surface->bufferSize();
        for (int cycle = 0; cycle < 3; ++cycle) {
            QPointer<QWaylandSurface> previous = m_surface;
            const int panels = m_inputPanel->toplevelPanelCount();
            if (cycle == 1) {
                m_inputMethod->sendDeactivate();
                QTest::qWait(250);
            }
            m_screenSaver.setActive(true);
            if (cycle == 1) {
                QTest::qWait(100);
                m_inputMethod->sendActivate();
            }
            QTRY_VERIFY(m_inputPanel->toplevelPanelCount() > panels);
            QTRY_VERIFY(m_surface && m_surface != previous && m_surface->hasContent());
            QTRY_VERIFY(!m_toplevel);
            QTRY_VERIFY(!previous);
            QCOMPARE(m_surface->bufferSize(), desktopSize);

            auto context = m_inputMethod->context();
            QVERIFY(context);
            QSignalSpy commits(context, &InputMethodContext::commitStringChanged);
            for (auto resource : context->resourceMap()) {
                context->send_content_type(resource->handle, 0x40 | 0x80, 8); // hidden/sensitive password
                context->send_surrounding_text(resource->handle, QString(), 0, 0);
                context->send_commit_state(resource->handle, 20 + cycle);
            }
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(100);
            const QPointF key(0.055 * desktopSize.width(), 80 + (desktopSize.height() - 116) * 0.125);
            seat->sendTouchPointPressed(m_surface, 0, key);
            seat->sendTouchFrameEvent(m_surface->client());
            seat->sendTouchPointReleased(m_surface, 0, key);
            seat->sendTouchFrameEvent(m_surface->client());
            wl_display_flush_clients(m_compositor->display());
            QTRY_COMPARE(commits.count(), 1);
            QCOMPARE(commits.first().first().toString(), QStringLiteral("q"));
            QVERIFY(!seat->keyboardFocus());

            // No candidates may be accepted on the lock screen, even if a
            // greeter omits the password hint or leaves old surrounding text.
            for (auto resource : context->resourceMap()) {
                context->send_content_type(resource->handle, 0, 0);
                context->send_surrounding_text(resource->handle, QStringLiteral("hel"), 3, 3);
                context->send_commit_state(resource->handle, 30 + cycle);
            }
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(100);
            seat->sendMouseMoveEvent(m_view.get(), QPointF(184, 58));
            seat->sendMousePressEvent(Qt::LeftButton);
            seat->sendMouseReleaseEvent(Qt::LeftButton);
            wl_display_flush_clients(m_compositor->display());
            QTest::qWait(100);
            QCOMPARE(commits.count(), 1);

            previous = m_surface;
            m_screenSaver.setActive(false);
            QTRY_VERIFY(m_toplevel && m_surface && m_surface != previous && m_surface->hasContent());
            QTRY_VERIFY(!previous);
            QCOMPARE(m_surface->bufferSize(), desktopSize);
            QVERIFY(!m_toplevel->activated());
        }
    }

    void testStartupWhileLocked()
    {
        m_inputMethod->sendDeactivate();
        m_child->terminate();
        QVERIFY(m_child->waitForFinished(2000));
        QTRY_VERIFY(!m_surface);
        m_screenSaver.setActive(true);
        const int panels = m_inputPanel->toplevelPanelCount();
        m_child->start();
        QVERIFY(m_child->waitForStarted());
        QTRY_VERIFY(m_inputMethod->context());
        QTRY_VERIFY(m_inputPanel->toplevelPanelCount() > panels);
        QTRY_VERIFY(m_surface && m_surface->hasContent());
        QVERIFY(!m_toplevel);
        m_screenSaver.setActive(false);
        QTRY_VERIFY(m_toplevel && m_surface && m_surface->hasContent());
    }

    void cleanupTestCase()
    {
        m_frameTimer.stop();
        m_view.reset();
        if (m_child) {
            if (m_child->state() != QProcess::NotRunning) {
                m_child->terminate();
                m_child->waitForFinished(2000);
            }
            m_child.reset();
        }
    }

private:
    MockScreenSaver m_screenSaver;
    QPointer<QWaylandSurface> m_surface;
    QPointer<QWaylandXdgToplevel> m_toplevel;
    std::unique_ptr<QWaylandView> m_view;
    QTimer m_frameTimer;
    QTemporaryDir m_runtimeDir;
    QTemporaryDir m_home;
    QString m_socketPath;

    std::unique_ptr<QWaylandCompositor> m_compositor;
    std::unique_ptr<QWindow> m_outputWindow;
    std::unique_ptr<QWaylandOutput> m_output;
    std::unique_ptr<QWaylandXdgShell> m_xdgShell;
    std::unique_ptr<InputMethodV1> m_inputMethod;
    std::unique_ptr<InputPanelV1> m_inputPanel;
    std::unique_ptr<QProcess> m_child;
};

QTEST_MAIN(MockInputMethodCompositorTest)

#include "mockinputmethodcompositor.moc"
