/*
 * The Dolphin service-menu window: "Mount as Network Drive".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thin host around the shared ShareForm, which is the same file the KCM
 * embeds — the two front ends now differ only in chrome and in what each
 * knows up front. Here the share is fixed (it came from the smb:// URL
 * Dolphin was invoked on) and, when that share is already saved, this window
 * offers removal instead of an add form, matching the original service-menu
 * interaction. Editing a saved share remains a KCM job.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2

QQC2.ApplicationWindow {
    id: root

    readonly property bool existing: backend.existingId.length > 0
    property bool busy: false

    /** Opens at this size, never smaller, freely larger. Binding width/height
     *  to the minimums is what allows both: a resize assigns width/height
     *  directly and breaks those bindings, while the minimums are never
     *  assigned and keep enforcing the floor. Two heights because the
     *  already-saved view is a few labels and one button, not the form. */
    readonly property int preferredWidth: 560
    readonly property int preferredHeight: existing ? 260 : 500

    visible: true
    title: "Mount as Network Drive"
    width: preferredWidth
    height: preferredHeight
    minimumWidth: preferredWidth
    minimumHeight: preferredHeight

    // --- credential autofill (plan §5) ---------------------------------------
    // Everything host-specific about the lookup lives here rather than in the
    // shared form: the KCM embeds that same form and has no smb:// URL to
    // look anything up for, so the form only knows how to *receive* a
    // suggestion.
    Connections {
        target: backend
        function onCredentialSuggestion(username, domain, password) {
            form.applyCredentialSuggestion(username, domain, password)
        }
    }

    // The window is the only place that knows the lookup has become pointless:
    // the form seals itself when the user types in a credential field or
    // submits, and the child process should stop at that moment rather than
    // at its 30-second deadline. A suggestion that arrives anyway is refused
    // twice over -- the controller's generation check drops it, and the form
    // would refuse it.
    Connections {
        target: form
        function onCredentialsSealedChanged() {
            if (form.credentialsSealed) {
                backend.cancelCredentialLookup()
            }
        }
    }

    // Started here, not in the backend's constructor: before this point QML
    // has not connected to credentialSuggestion() yet, so a fast answer --
    // a cached credential comes back in milliseconds -- would be delivered to
    // nobody.
    Component.onCompleted: backend.startCredentialLookup()

    // Nothing may outlive the window. Closing while a wallet prompt is open
    // leaves that prompt to KDE, which owns it, but the child we started is
    // stopped here so no lookup is left running against a window that is
    // gone.
    onClosing: backend.cancelCredentialLookup()

    Connections {
        target: backend.actions
        function onFinished(id, kind, success, message) {
            root.busy = false
            if (success) {
                resultDialog.title = kind === "add" ? "Added" : "Removed"
                resultDialog.closeWhenDone = true
            } else {
                resultDialog.title = "Failed"
                resultDialog.closeWhenDone = false
            }
            resultText.text = message
            resultDialog.open()
        }
    }

    QQC2.Dialog {
        id: resultDialog
        property bool closeWhenDone: false
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        standardButtons: QQC2.Dialog.Ok
        // Escape / click-outside must not silently skip the quit-on-success
        // path below -- Popup.close() (which both of those trigger) does not
        // emit accepted(), so OK has to be the only way out of this dialog.
        closePolicy: QQC2.Popup.NoAutoClose
        onAccepted: if (closeWhenDone) { Qt.quit() }

        QQC2.Label {
            id: resultText
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }

    QQC2.Dialog {
        id: confirmRemove
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        title: "Remove this saved share?"
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        onAccepted: {
            root.busy = true
            backend.removeExisting()
        }

        QQC2.Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "If it is currently mounted, it will be unmounted first. The mount point directory "
                + "itself is left in place."
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // --- already-saved share: report and offer removal only -------------
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            font.bold: true
            text: backend.unc
            Layout.fillWidth: true
        }
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            text: "This share is already saved, at " + backend.existingMountPoint
                + " (" + backend.existingStateText + ")."
            Layout.fillWidth: true
        }
        QQC2.Label {
            visible: root.existing
            wrapMode: Text.WordWrap
            opacity: 0.7
            font.italic: true
            text: "To change settings for a saved share, use System Settings → Network Mounts. "
                + "Remove below deletes it entirely."
            Layout.fillWidth: true
        }

        // --- new share: the shared form --------------------------------------
        ShareForm {
            id: form
            visible: !root.existing
            actions: backend.actions
            fixedUnc: backend.unc
            mountPoint: backend.suggestedPath
            username: backend.suggestedUser
            Layout.fillWidth: true
            onSubmitted: root.busy = true
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            QQC2.Button {
                visible: !root.existing
                text: "Mount"
                // The same icon the KCM and the polkit prompt use for this
                // tool (io.github.pakru.nasmount.actions), so the action the
                // user is confirming looks like the thing they invoked.
                icon.name: "drive-network"
                enabled: form.canSubmit && !root.busy
                onClicked: form.submit()
            }
            QQC2.Button {
                visible: root.existing
                text: "Remove"
                enabled: !root.busy
                onClicked: confirmRemove.open()
            }
        }
    }
}
