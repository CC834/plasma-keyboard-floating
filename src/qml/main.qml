/*
    SPDX-FileCopyrightText: 2024 Aleix Pol i Gonzalez <aleixpol@kde.org>
    SPDX-FileCopyrightText: 2026 Kristen McWilliam <kristen@kde.org>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

import QtQuick
import QtQuick.VirtualKeyboard
import QtQuick.VirtualKeyboard.Settings
import QtCore as QtCore
import QtQuick.Controls as QQC2
import QtMultimedia
import QtQuick.Layouts

import org.kde.plasma.keyboard
import org.kde.plasma.keyboard.lib as PlasmaKeyboard

import org.kde.kirigami as Kirigami

InputPanelWindow {
    id: root
    readonly property real screenMargin: Kirigami.Units.gridUnit
    readonly property real availableScreenWidth: Screen.width > 0 ? Screen.width : 1280
    readonly property real availableScreenHeight: Screen.height > 0 ? Screen.height : 800
    minimumWidth: Math.min(440, availableScreenWidth - screenMargin * 2)
    maximumWidth: Math.max(minimumWidth, availableScreenWidth - screenMargin * 2)
    minimumHeight: Math.min(264, availableScreenHeight - screenMargin * 2)
    maximumHeight: Math.max(minimumHeight, Math.min(600, availableScreenHeight - screenMargin * 2))
    width: 772
    height: 368

    function fitToScreen() {
        if (!visible) {
            return;
        }
        width = Math.round(Math.max(minimumWidth, Math.min(maximumWidth, width)));
        height = Math.round(Math.max(minimumHeight, Math.min(maximumHeight, height)));
    }

    function resizeKeyboard(w, h) {
        width = Math.round(Math.max(minimumWidth, Math.min(maximumWidth, w)));
        height = Math.round(Math.max(minimumHeight, Math.min(maximumHeight, h)));
    }

    onMaximumWidthChanged: Qt.callLater(fitToScreen)
    onMaximumHeightChanged: Qt.callLater(fitToScreen)
    onWidthChanged: saveSizeTimer.restart()
    onHeightChanged: saveSizeTimer.restart()

    Timer {
        id: saveSizeTimer
        interval: 300
        onTriggered: {
            if (root.visible && !root.lockScreenMode) {
                floatingKeyboardState.keyboardWidth = root.width;
                floatingKeyboardState.keyboardHeight = root.height;
            }
        }
    }

    color: "transparent"

    // A tool window stays out of the task manager. Not accepting focus is
    // essential: tapping a key must not take focus from the text field that
    // owns the input-method context.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus

    QtCore.Settings {
        id: floatingKeyboardState
        category: "FloatingKeyboard"
        property real keyboardScale: 0.9 // Migrate the previous proportional size.
        property real keyboardWidth: 0
        property real keyboardHeight: 0
        property string keyboardLocale: ""
        property bool hasSuggestionBar: false
    }

    Component.onCompleted: {
        const scale = Number.isFinite(floatingKeyboardState.keyboardScale) ? floatingKeyboardState.keyboardScale : 0.9;
        const savedWidth = floatingKeyboardState.keyboardWidth;
        const savedHeight = floatingKeyboardState.keyboardHeight;
        width = Number.isFinite(savedWidth) && savedWidth > 0 ? savedWidth : 840 * scale + panelWrapper.sidePadding * 2;
        height = Number.isFinite(savedHeight) && savedHeight > 0 ? savedHeight : 280 * scale + panelWrapper.topPadding + panelWrapper.bottomPadding;
        if (!floatingKeyboardState.hasSuggestionBar) {
            height += suggestionBar.height;
            floatingKeyboardState.hasSuggestionBar = true;
        }
    }

    onVisibleChanged: {
        if (visible) {
            Qt.callLater(root.fitToScreen);
        }
        if (!visible) {
            // Reset keyboard navigation when hidden
            // Note: keyboard property is internal Qt API
            if (inputPanel.keyboard.navigationModeActive) {
                inputPanel.keyboard.navigationModeActive = false;
            }

            // Close language dialog
            languageDialog.close();
        }
    }

    InputListenerItem {
        id: thing
        focus: true
        engine: inputPanel.InputContext.inputEngine
        suggestionLocale: VirtualKeyboardSettings.locale

        keyboardNavigationActive: inputPanel.keyboard.navigationModeActive

        onKeyNavigationPressed: (key) => {
            // HACK: invoke the Qt VirtualKeyboard keyboard navigation feature ourselves
            // See https://github.com/qt/qtvirtualkeyboard/blob/6d810ac41df96f1ad984f56e17f16860bec2abbf/src/virtualkeyboard/qvirtualkeyboardinputcontext_p.h#L110
            inputPanel.InputContext.priv.navigationKeyPressed(key, false);
        }
        onKeyNavigationReleased: (key) => {
            // HACK: invoke the Qt VirtualKeyboard keyboard navigation feature ourselves
            inputPanel.InputContext.priv.navigationKeyReleased(key, false);
        }
    }

    // Some distributions build Qt Virtual Keyboard without vkb_sound_effects.
    // Play feedback directly through Multimedia so the switch works there too.
    SoundEffect {
        id: keyClick
        source: "qrc:/sounds/keyboard_tick2_quiet.wav"
        volume: 0.7
        muted: !PlasmaKeyboardSettings.soundEnabled
        readonly property bool keyPressed: inputPanel.keyboard.activeKey ? inputPanel.keyboard.activeKey.pressed : false
        onKeyPressedChanged: {
            if (keyPressed && PlasmaKeyboardSettings.soundEnabled) {
                keyClick.play();
            }
        }
        onMutedChanged: if (muted) stop()
    }

    // Avoid duplicate feedback on Qt builds that do include their own player.
    Binding {
        target: inputPanel.keyboard.soundEffect
        property: "enabled"
        value: false
    }

    Connections {
        target: inputPanel.InputContext.inputEngine
        function onVirtualKeyClicked(key, text, modifiers, isAutoRepeat) {
            if (isAutoRepeat && inputPanel.keyboard.activeKey && PlasmaKeyboardSettings.soundEnabled) {
                keyClick.play();
            }
        }
    }

    // Unified overlay system for diacritics, emoji, text expansion, etc.
    OverlayWindow {
        id: overlayWindow
        controller: thing.overlayController
        onCandidateSelected: (index) => thing.overlayController.commitCandidate(index)
    }

    interactiveRegion: Qt.rect(0, 0, width, height)

    Kirigami.ShadowedRectangle {
        id: panelWrapper
        anchors.fill: parent

        LanguagePopup {
            id: languageDialog
            style: inputPanel.keyboard.style
            keyboardPanel: inputPanel

            onShowSettings: root.showSettings()
        }

        color: PlasmaKeyboard.BreezeConstants.keyboardBackgroundColor

        // Provide shadow and radius when the keyboard is detached from edges
        corners {
            bottomLeftRadius: Kirigami.Units.cornerRadius
            bottomRightRadius: Kirigami.Units.cornerRadius
            topLeftRadius: Kirigami.Units.cornerRadius
            topRightRadius: Kirigami.Units.cornerRadius
        }
        shadow {
            size: 16
            color: Qt.rgba(0, 0, 0, 0.3)
        }

        x: 0
        y: 0

        // Leave a comfortable touch target above the keys for dragging.
        readonly property real sidePadding: Kirigami.Units.largeSpacing
        readonly property real topPadding: Kirigami.Units.gridUnit * 2
        readonly property real bottomPadding: Kirigami.Units.gridUnit * 2

        Item {
            id: dragArea
            enabled: !root.lockScreenMode
            visible: !root.lockScreenMode
            z: 2
            anchors {
                top: parent.top
                left: parent.left
                right: sizeButton.left
            }
            height: panelWrapper.topPadding

            Rectangle {
                anchors.centerIn: parent
                width: Kirigami.Units.gridUnit * 2
                height: Math.max(4, Kirigami.Units.smallSpacing / 2)
                radius: height / 2
                color: Kirigami.Theme.disabledTextColor
                opacity: 0.75
            }

            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.SizeAllCursor
                dragThreshold: 0

                // Pass the current mouse or touch serial to KWin so it moves
                // the whole Wayland window, including across output boundaries.
                onActiveChanged: {
                    if (active) {
                        root.startSystemMove();
                    }
                }
            }

        }

        QQC2.ToolButton {
            id: sizeButton
            enabled: !root.lockScreenMode
            visible: !root.lockScreenMode
            anchors.top: parent.top
            anchors.right: soundButton.left
            width: panelWrapper.topPadding
            height: panelWrapper.topPadding
            focusPolicy: Qt.NoFocus
            icon.name: root.width < 650 ? "view-fullscreen" : "view-restore"
            text: root.width < 650 ? i18n("Wide keyboard") : i18n("Compact keyboard")
            display: QQC2.AbstractButton.IconOnly
            onClicked: root.width < 650 ? root.resizeKeyboard(800, 384) : root.resizeKeyboard(560, 304)
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
        }

        QQC2.ToolButton {
            id: soundButton
            anchors.top: parent.top
            anchors.right: hideButton.left
            width: panelWrapper.topPadding
            height: panelWrapper.topPadding
            focusPolicy: Qt.NoFocus
            icon.name: PlasmaKeyboardSettings.soundEnabled ? "audio-volume-medium" : "audio-volume-muted"
            text: PlasmaKeyboardSettings.soundEnabled ? i18n("Mute key clicks") : i18n("Enable key clicks")
            display: QQC2.AbstractButton.IconOnly
            onClicked: {
                PlasmaKeyboardSettings.soundEnabled = !PlasmaKeyboardSettings.soundEnabled;
                PlasmaKeyboardSettings.save();
            }
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
        }

        QQC2.ToolButton {
            id: hideButton
            anchors.top: parent.top
            anchors.right: parent.right
            width: panelWrapper.topPadding
            height: panelWrapper.topPadding
            focusPolicy: Qt.NoFocus
            icon.name: "arrow-down"
            text: i18n("Hide keyboard")
            display: QQC2.AbstractButton.IconOnly
            onClicked: thing.hideKeyboard()
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
        }

        Item {
            id: resizeGrip
            enabled: !root.lockScreenMode
            visible: !root.lockScreenMode
            z: 3
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: Kirigami.Units.gridUnit * 2
            height: Kirigami.Units.gridUnit * 2

            // Three diagonal strokes make the touch-resize affordance visible.
            Repeater {
                model: 3

                Rectangle {
                    required property int index
                    width: Kirigami.Units.gridUnit * (0.45 + index * 0.25)
                    height: Math.max(2, Kirigami.Units.smallSpacing / 3)
                    radius: height / 2
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.8
                    rotation: -45
                    anchors.right: parent.right
                    anchors.rightMargin: Kirigami.Units.smallSpacing + index * Kirigami.Units.smallSpacing
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Kirigami.Units.smallSpacing + index * Kirigami.Units.smallSpacing
                }
            }

            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.SizeFDiagCursor
                dragThreshold: 0
                property real initialWidth: 0
                property real initialHeight: 0

                onActiveChanged: {
                    if (active) {
                        initialWidth = root.width;
                        initialHeight = root.height;
                    } else {
                        saveSizeTimer.restart();
                    }
                }
                onTranslationChanged: {
                    if (active) {
                        root.resizeKeyboard(initialWidth + activeTranslation.x, initialHeight + activeTranslation.y);
                    }
                }
            }

        }

        // Compositor-managed resizing also handles moving the left/top origin.
        Repeater {
            model: [Qt.LeftEdge, Qt.RightEdge, Qt.TopEdge, Qt.BottomEdge,
                Qt.TopEdge | Qt.LeftEdge, Qt.TopEdge | Qt.RightEdge, Qt.BottomEdge | Qt.LeftEdge]
            delegate: Item {
                enabled: !root.lockScreenMode
                visible: !root.lockScreenMode
                required property int modelData
                readonly property bool leftEdge: (modelData & Qt.LeftEdge) !== 0
                readonly property bool rightEdge: (modelData & Qt.RightEdge) !== 0
                readonly property bool topEdge: (modelData & Qt.TopEdge) !== 0
                readonly property bool bottomEdge: (modelData & Qt.BottomEdge) !== 0
                readonly property bool corner: (leftEdge || rightEdge) && (topEdge || bottomEdge)
                z: 5
                width: leftEdge || rightEdge ? (corner ? 16 : 8) : parent.width - 32
                height: topEdge || bottomEdge ? (corner ? 16 : 8) : parent.height - 32
                x: leftEdge ? 0 : rightEdge ? parent.width - width : 16
                y: topEdge ? 0 : bottomEdge ? parent.height - height : 16
                DragHandler {
                    target: null
                    dragThreshold: 0
                    acceptedButtons: Qt.LeftButton
                    cursorShape: parent.corner ? (parent.leftEdge === parent.topEdge ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                        : (parent.leftEdge || parent.rightEdge ? Qt.SizeHorCursor : Qt.SizeVerCursor)
                    onActiveChanged: if (active) root.startSystemResize(parent.modelData)
                }
            }
        }

        RowLayout {
            id: suggestionBar
            x: panelWrapper.sidePadding
            y: panelWrapper.topPadding
            width: parent.width - panelWrapper.sidePadding * 2
            height: 44
            spacing: 0

            QQC2.ToolButton {
                objectName: "keyboardLanguageButton"
                text: VirtualKeyboardSettings.locale.slice(0, 2).toUpperCase()
                font.pixelSize: 14
                Layout.preferredWidth: 54
                Layout.fillHeight: true
                focusPolicy: Qt.NoFocus
                enabled: VirtualKeyboardSettings.activeLocales.length > 1
                Accessible.name: i18n("Switch keyboard language")
                onClicked: inputPanel.switchLanguage()
                QQC2.ToolTip.text: Qt.locale(VirtualKeyboardSettings.locale).nativeLanguageName
                QQC2.ToolTip.visible: hovered
            }

            Repeater {
                model: 3
                delegate: QQC2.Button {
                    required property int index
                    property string pressedWord: ""
                    objectName: "wordSuggestion" + index
                    text: thing.suggestions[index] || ""
                    font.pixelSize: Math.max(14, Math.min(18, root.width / 42))
                    enabled: text.length > 0
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    focusPolicy: Qt.NoFocus
                    flat: true
                    onPressed: pressedWord = text
                    onClicked: {
                        if (thing.acceptSuggestion(pressedWord) && PlasmaKeyboardSettings.soundEnabled) {
                            keyClick.play();
                        }
                    }
                    background: Rectangle {
                        color: parent.down ? Kirigami.Theme.highlightColor : "transparent"
                        radius: Kirigami.Units.smallSpacing
                        Rectangle {
                            width: 1
                            height: parent.height * 0.5
                            anchors.verticalCenter: parent.verticalCenter
                            color: Kirigami.Theme.disabledTextColor
                            opacity: 0.25
                        }
                    }
                }
            }
        }

        InputPanel {
            id: inputPanel
            property bool localesInitialized: false
            readonly property real availableWidth: panelWrapper.width - panelWrapper.sidePadding * 2
            readonly property real availableHeight: Math.max(1, panelWrapper.height - panelWrapper.topPadding - suggestionBar.height - panelWrapper.bottomPadding)
            readonly property real layoutAspect: Math.max(2.2, Math.min(4.2, availableWidth / availableHeight))
            width: Math.min(availableWidth, availableHeight * layoutAspect)
            x: (panelWrapper.width - width) / 2
            y: panelWrapper.topPadding + suggestionBar.height + (availableHeight - height) / 2

            Binding {
                target: inputPanel.keyboard.style
                property: "keyboardDesignWidth"
                value: inputPanel.keyboard.style ? inputPanel.keyboard.style.keyboardDesignHeight * inputPanel.layoutAspect : 2100
                when: inputPanel.keyboard.style !== null
            }

            focusPolicy: Qt.NoFocus
            externalLanguageSwitchEnabled: true
            onExternalLanguageSwitch: (localeList, currentIndex) => {
                if (VirtualKeyboardSettings.activeLocales.length === 2) {
                    switchLanguage();
                } else {
                    languageDialog.show(inputPanel.keyboard.activeKey, localeList, currentIndex);
                }
            }

            function switchLanguage() {
                const locales = VirtualKeyboardSettings.activeLocales;
                const index = locales.indexOf(VirtualKeyboardSettings.locale);
                VirtualKeyboardSettings.locale = locales[(index + 1) % locales.length];
            }

            function updateLocales() {
                if (PlasmaKeyboardSettings.enabledLocales.length === 0) {
                    VirtualKeyboardSettings.activeLocales = ["en_US", "sv_SE"];
                } else {
                    VirtualKeyboardSettings.activeLocales = PlasmaKeyboardSettings.enabledLocales;
                }
                if (VirtualKeyboardSettings.activeLocales.indexOf(VirtualKeyboardSettings.locale) < 0) {
                    VirtualKeyboardSettings.locale = VirtualKeyboardSettings.activeLocales[0];
                }
            }

            Connections {
                target: VirtualKeyboardSettings
                function onAvailableLocalesChanged() {
                    inputPanel.updateLocales();
                }
                function onLocaleChanged() {
                    if (inputPanel.localesInitialized) {
                        floatingKeyboardState.keyboardLocale = VirtualKeyboardSettings.locale;
                    }
                }
            }

            Connections {
                target: PlasmaKeyboardSettings
                function onEnabledLocalesChanged() {
                    inputPanel.updateLocales();
                }
            }

            Component.onCompleted: {
                VirtualKeyboardSettings.styleName = "Breeze";
                const savedLocale = floatingKeyboardState.keyboardLocale;
                inputPanel.updateLocales();
                const locales = VirtualKeyboardSettings.activeLocales;
                VirtualKeyboardSettings.locale = locales.indexOf(savedLocale) >= 0 ? savedLocale : locales[0];
                localesInitialized = true;
            }
        }
    }
}
