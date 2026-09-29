/*
 * The KCM's Add wrapper around the shared ShareForm.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything about the form itself lives in ShareForm.qml, which
 * nasmount-dialog embeds verbatim. This file supplies only what is specific
 * to being a modal dialog inside the KCM page: the window chrome, the Add and
 * Cancel buttons, the wait for the helper, and the `kcm.actions` binding. Do
 * not reintroduce form fields here - a field added on one side and not the
 * other makes the two front ends drift apart.
 *
 * The dialog stays open from Add until the helper answers. Closing it at once
 * meant a refusal (a cancelled or failed authentication prompt, a folder that
 * changed since it was chosen) appeared in the page footer with the dialog
 * gone, and adding again started from an empty form: address, username,
 * password and domain all typed anew. Now success closes it, and a refusal
 * keeps every field and shows the message here.
 *
 * After a Share Browse pick the form asks for a saved login and this dialog
 * routes the question to the KCM host (which runs the lookup child) and the
 * answer back. The form owns every rule about when an answer may be applied;
 * this file only cancels a lookup that is no longer wanted - the share was
 * retyped, a credential field was edited, or the dialog closed.
 *
 * Cancel and Escape stay live while waiting. A KAuth call cannot be taken
 * back and the helper may already have written the units, so closing only
 * stops this dialog listening: the page footer reports the result, exactly as
 * it did before this dialog waited. main.qml keeps its own Add action
 * disabled for as long as `busy` holds, which is what makes the next
 * finished("add") unambiguous.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

QQC2.Dialog {
    id: dialog
    modal: true
    title: "Add network mount"
    width: Math.min((parent ? parent.width : 640) - 40, 520)

    /** True from Add until the add finishes - also after the dialog was
     *  closed meanwhile, because the add itself is still running. */
    property bool busy: false

    function openForAdd() {
        form.reset()
        errorMessage.visible = false
        open()
    }

    function submit() {
        errorMessage.visible = false
        // Before the hand-over: an address or guest-field refusal is
        // reported by finished() synchronously, from inside submit().
        busy = true
        form.submit()
    }

    // A closed dialog keeps no password in its fields until the next Add, and
    // no lookup running for a form nobody is looking at.
    onClosed: {
        kcm.cancelCredentialLookup()
        form.reset()
    }

    Connections {
        target: kcm
        function onCredentialSuggestion(username, domain, password) {
            form.applyCredentialSuggestion(username, domain, password)
        }
    }

    Connections {
        target: kcm.actions
        function onFinished(id, kind, success, message) {
            if (kind !== "add" || !dialog.busy) {
                return
            }
            dialog.busy = false
            // Closed while waiting: nobody is looking at this dialog, and the
            // page reports the outcome.
            if (!dialog.opened) {
                return
            }
            if (success) {
                // The page footer reports the success.
                dialog.close()
            } else {
                errorMessage.text = message
                errorMessage.visible = true
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 6

        Kirigami.InlineMessage {
            id: errorMessage
            type: Kirigami.MessageType.Error
            visible: false
            Layout.fillWidth: true
        }

        RowLayout {
            visible: dialog.busy
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true

            QQC2.BusyIndicator {
                running: dialog.busy
                implicitWidth: Kirigami.Units.iconSizes.smallMedium
                implicitHeight: Kirigami.Units.iconSizes.smallMedium
            }
            QQC2.Label {
                text: "Adding… confirm in the authentication prompt"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        ShareForm {
            id: form
            actions: kcm.actions
            enabled: !dialog.busy
            errorColor: Kirigami.Theme.negativeTextColor
            Layout.fillWidth: true

            onCredentialLookupRequested: (shareInput, user) => kcm.startCredentialLookup(shareInput, user)
            // Stop a lookup that can no longer be applied: the user took the
            // credential fields over, or the address moved on. The form
            // refuses a late answer either way; stopping the child just saves
            // its work and its wallet prompt.
            onCredentialsSealedChanged: {
                if (credentialsSealed) {
                    kcm.cancelCredentialLookup()
                }
            }
            onEffectiveUncChanged: kcm.cancelCredentialLookup()
        }
    }

    footer: QQC2.DialogButtonBox {
        // Cancel stays a standard button, so it keeps the style's own label
        // and icon; Add is ours, since there is no standard "Add".
        standardButtons: QQC2.DialogButtonBox.Cancel

        QQC2.Button {
            text: "Add"
            icon.name: "list-add"
            enabled: form.canSubmit && !dialog.busy
            // ApplyRole, not AcceptRole: the box routes AcceptRole to the
            // dialog's accept(), which always closes it, and this dialog
            // must stay open until the helper has answered.
            QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.ApplyRole
            onClicked: dialog.submit()
        }
    }
}
