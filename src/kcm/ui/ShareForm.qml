/*
 * ShareForm — the add-a-share form, shared verbatim by both front ends: the
 * KCM wraps it in a QQC2.Dialog, nasmount-dialog wraps it in a window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file must stay host-agnostic. It never references `kcm` (or any other
 * host context object) — the MountActions instance arrives through the
 * `actions` property, and every other host difference is a property too. That
 * is what lets one definition serve both entry points; a single `kcm.` here
 * would silently make it KCM-only again, which is exactly how the two forms
 * drifted apart before (the service menu could not create System shares at
 * all, because its separate copy predated the mode selector).
 *
 * Validation here is convenience only. UnitSpec re-validates every field in
 * the privileged helper, which is the boundary that actually matters.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as QtDialogs

ColumnLayout {
    id: form

    /** Session::MountActions, injected by the host. */
    property var actions: null

    /** When non-empty the share is fixed (the service menu already knows it
     *  from the smb:// URL it was invoked on) and is shown as a header rather
     *  than an editable field. Empty means the user types it, as in the KCM. */
    property string fixedUnc: ""

    property alias mountPoint: pathField.text
    property alias username: userField.text

    /** The share this form will actually submit. */
    readonly property string effectiveUnc: fixedUnc.length > 0 ? fixedUnc : uncField.text

    readonly property bool canSubmit: effectiveUnc.length > 2 && pathField.text.length > 0

    /** One of the three values UnitValue::accessModeToString() produces. The
     *  helper re-validates it; this is convenience, like every other check
     *  in this file. */
    readonly property string accessMode: readOnlyRadio.checked
        ? "readonly"
        : (executableRadio.checked ? "readwrite-executable" : "readwrite")

    /**
     * True once the credential fields may no longer be filled in for the
     * user -- because they typed in one of them, or because the form has been
     * submitted (SMB credential autofill plan §5).
     *
     * A host watches this to stop a lookup it started: the moment the user
     * takes the credential into their own hands, an answer still on its way
     * is worthless and must not be applied. It is one property rather than
     * two because both causes mean exactly the same thing here -- no
     * suggestion may be applied from now on -- and a second flag would
     * eventually be checked in only one of the places that matter.
     *
     * Deliberately driven by TextField.textEdited and not by textChanged:
     * textChanged also fires when the *host* fills a field in, so tracking it
     * would make the form's own initialisation look like a user edit and
     * disable the feature for everyone.
     */
    property bool credentialsSealed: false

    /** Emitted after a submit has been handed to `actions`; the host decides
     *  what closing means for it (a dialog closes, a window waits for the
     *  finished() signal so it can report the outcome). */
    signal submitted()

    spacing: 6

    QtDialogs.FolderDialog {
        id: folderDialog
        onAccepted: pathField.text = selectedFolder.toString().replace("file://", "")
    }

    QQC2.Label {
        visible: form.fixedUnc.length > 0
        text: form.fixedUnc
        font.bold: true
        elide: Text.ElideMiddle
        Layout.fillWidth: true
    }

    QQC2.Label {
        visible: form.fixedUnc.length === 0
        text: "Share (//host/share[/subdir]):"
    }
    QQC2.TextField {
        id: uncField
        visible: form.fixedUnc.length === 0
        Layout.fillWidth: true
    }

    QQC2.Label { text: "Mount point:" }
    RowLayout {
        Layout.fillWidth: true
        QQC2.TextField {
            id: pathField
            placeholderText: "/home/you/ShareName"
            Layout.fillWidth: true
        }
        QQC2.Button {
            text: "Browse…"
            onClicked: folderDialog.open()
        }
    }

    QQC2.Label { text: "Username (leave empty for guest access):" }
    QQC2.TextField {
        id: userField
        // The three credential fields carry objectNames so shareform_qml_test
        // can find them in this very file rather than in a C++ imitation of
        // it. QML ids do not exist outside the component; an objectName is
        // the supported way to reach an item from outside.
        objectName: "userField"
        Layout.fillWidth: true
        // Guest selection clears the fields it disables below (design
        // §7.4.4), not just visually hides them -- otherwise stale text
        // left in a disabled field is silently sent as guest-inconsistent
        // input and the save is confusingly rejected.
        onTextChanged: {
            if (text.length === 0) {
                passwordField.text = ""
                domainField.text = ""
            }
        }
        onTextEdited: form.credentialsSealed = true
    }

    QQC2.Label { text: "Password:" }
    QQC2.TextField {
        id: passwordField
        objectName: "passwordField"
        echoMode: TextInput.Password
        enabled: userField.text.length > 0
        Layout.fillWidth: true
        onTextEdited: form.credentialsSealed = true
    }

    QQC2.Label { text: "Domain (optional):" }
    QQC2.TextField {
        id: domainField
        objectName: "domainField"
        enabled: userField.text.length > 0
        Layout.fillWidth: true
        onTextEdited: form.credentialsSealed = true
    }

    QQC2.Label {
        text: "Access:"
        Layout.topMargin: 6
    }
    QQC2.ButtonGroup { id: accessGroup }
    QQC2.RadioButton {
        id: readOnlyRadio
        text: "Read only"
        QQC2.ButtonGroup.group: accessGroup
        // hoverEnabled is required: `hovered` stays false without it, so the
        // tip would never appear (QtQuick.Controls ToolTip, "Delay and
        // Timeout"). The three tips share one label instance -- that is the
        // attached ToolTip's documented behaviour, and it is what keeps only
        // the hovered row's tip on screen.
        hoverEnabled: true
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: 500
        QQC2.ToolTip.timeout: 8000
        QQC2.ToolTip.text: "Read only access to remote files"            
    }
    QQC2.RadioButton {
        id: readWriteRadio
        checked: true
        text: "Read & Write"
        QQC2.ButtonGroup.group: accessGroup
        hoverEnabled: true
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: 500
        QQC2.ToolTip.timeout: 8000
        QQC2.ToolTip.text: "Read and Write access to remote files"            
    }
    QQC2.RadioButton {
        id: executableRadio
        text: "Read & Write & Execute"
        QQC2.ButtonGroup.group: accessGroup
        hoverEnabled: true
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: 500
        QQC2.ToolTip.timeout: 8000
        QQC2.ToolTip.text: "Read, Write and Execution access to remote files\nAllows programs stored on the share to run"            
    }
    QQC2.Label {
        wrapMode: Text.WordWrap
        opacity: 0.6
        font.italic: true
        text: "To change a network mount, remove and add it again in System settings"
        Layout.fillWidth: true
        Layout.topMargin: 6
    }

    /**
     * Fills the credential fields with one suggestion, or refuses to.
     *
     * The whole tuple is applied or none of it is: a username from a cache
     * and a password the user typed are two different accounts' halves, and
     * pairing them would produce a credential that was never valid anywhere.
     * That is why this takes all three fields and checks one seal, rather
     * than offering the host three setters to use as it sees fit.
     *
     * Host-agnostic on purpose (this file must never name a host object): the
     * host decides *when* there is something to apply, this decides whether
     * it still may be.
     *
     * Username is assigned first and only when non-empty, so the guest-
     * clearing handler above -- which wipes password and domain the moment
     * username becomes empty -- cannot run between the three assignments and
     * silently undo two of them.
     *
     * Returns true when the suggestion was applied.
     */
    function applyCredentialSuggestion(username, domain, password) {
        if (form.credentialsSealed || !username || username.length === 0) {
            return false
        }
        userField.text = username
        domainField.text = domain ? domain : ""
        passwordField.text = password ? password : ""
        return true
    }

    function reset() {
        uncField.text = "//"
        pathField.text = ""
        userField.text = ""
        domainField.text = ""
        passwordField.text = ""
        // The KCM's Add dialog reuses one form instance, so without this the
        // previous share's access choice silently leaks into the next add.
        readWriteRadio.checked = true
        // Clearing the fields above is an assignment, not an edit, so the
        // seal has to be cleared explicitly -- and it has to be cleared, or
        // the reused instance would carry one add's edit state into the next.
        form.credentialsSealed = false
    }

    // One lifecycle, no per-share switches: saving a share means it is armed
    // at boot and mounts on first access. There is deliberately no "arm at
    // sign-in" or mode choice to make -- asking to mount a share *is* asking
    // for it to be there after a reboot.
    //
    // Access is the one exception, and only because it cannot be changed
    // later: there is no in-place Edit anywhere in this codebase, so this
    // submit is the single moment the choice exists. It is recorded in the
    // root-owned unit marker, not here.
    function submit() {
        // Seal before the snapshot, not after: addShare() hands the four
        // credential values off to a worker thread, and a suggestion applied
        // between the two would change the fields the user is looking at
        // without changing what was submitted.
        form.credentialsSealed = true
        form.actions.addShare(effectiveUnc, pathField.text, userField.text, domainField.text,
                              passwordField.text, form.accessMode)
        form.submitted()
    }
}
