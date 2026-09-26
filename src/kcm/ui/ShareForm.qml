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
 * would silently make it KCM-only, and the two front ends would drift apart.
 *
 * Validation here is convenience only. UnitSpec re-validates every field in
 * the privileged helper, which is the boundary that actually matters.
 *
 * The share is typed as smb://host/share (//host/share is still accepted);
 * MountActions::addShare() resolves either to the //host/share form the
 * helper and the unit use. When the address names a user, that user fills an
 * empty Username as the address is typed.
 *
 * `readOnly` turns the same form into a view of a saved share — the KCM's
 * Details. It is presentation only, not an edit path: there is no in-place
 * Edit anywhere in this codebase, so a read-only form can never submit, and
 * it is sealed against credential suggestions. The password is never shown:
 * it lives only in the root-owned credential file, and nothing returns it.
 *
 * No Kirigami here: shareform_qml_test loads this exact file in the native
 * package builds, whose containers do not have Kirigami installed.
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

    /** Something after the smb:// or // prefix: the pre-filled prefix alone
     *  is not a share. */
    readonly property bool shareEntered: effectiveUnc.replace(/^(smb:)?\/\//i, "").length > 0

    readonly property bool canSubmit: !form.readOnly && shareEntered && pathField.text.length > 0

    /** Shows a saved share instead of collecting a new one. Set by the host;
     *  fill it with showDefinition(). */
    property bool readOnly: false

    /** "guest" | "credentials" | "" (unknown), for the read-only view's
     *  placeholders only: an orphan share has credentials but no known
     *  username, and an empty Username must not read as guest there. */
    property string savedAuthentication: ""

    readonly property bool savedUsernameUnknown: form.readOnly && form.savedAuthentication !== "guest"
                                                 && userField.text.length === 0

    /** The Username this form last filled in from the address. While
     *  Username still holds exactly that, a corrected address may replace
     *  it; once the user types their own, it is theirs. */
    property string urlFilledUsername: ""

    /** One of the three values UnitValue::accessModeToString() produces. The
     *  helper re-validates it; this is convenience, like every other check
     *  in this file. */
    readonly property string accessMode: readOnlyRadio.checked
        ? "readonly"
        : (executableRadio.checked ? "readwrite-executable" : "readwrite")

    /**
     * True once no suggestion may be applied any more -- the user typed in a
     * credential field, or the form was submitted. A host
     * watches it to stop a lookup it started. Driven by textEdited and not
     * textChanged, which also fires when the host fills a field in.
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
        text: form.actions ? form.actions.displayUrl(form.fixedUnc) : form.fixedUnc
        font.bold: true
        elide: Text.ElideMiddle
        Layout.fillWidth: true
    }

    QQC2.Label {
        visible: form.fixedUnc.length === 0
        text: form.readOnly ? "Share:" : "Share (smb://host/share[/subdir]):"
    }
    QQC2.TextField {
        id: uncField
        objectName: "uncField"
        visible: form.fixedUnc.length === 0
        readOnly: form.readOnly
        Layout.fillWidth: true
        // Fills Username from the address only while it is empty or still
        // holds what the address put there, and never clears it: clearing
        // Username would run the guest handler below and wipe a typed
        // password and domain. An assignment, not an edit, so it does not
        // seal credentials either.
        onTextEdited: {
            const fromAddress = form.actions ? form.actions.userInShareInput(text) : ""
            if (fromAddress.length > 0
                    && (userField.text.length === 0 || userField.text === form.urlFilledUsername)) {
                userField.text = fromAddress
                form.urlFilledUsername = fromAddress
            }
        }
    }

    QQC2.Label { text: "Mount point:" }
    RowLayout {
        Layout.fillWidth: true
        QQC2.TextField {
            id: pathField
            placeholderText: form.readOnly ? "" : "/home/you/ShareName"
            readOnly: form.readOnly
            Layout.fillWidth: true
        }
        QQC2.Button {
            objectName: "browseButton"
            visible: !form.readOnly
            text: "Browse…"
            onClicked: folderDialog.open()
        }
    }

    QQC2.Label { text: form.readOnly ? "Username:" : "Username (leave empty for guest access):" }
    QQC2.TextField {
        id: userField
        // The fields, the access radios and Browse carry objectNames so
        // shareform_qml_test can reach them: QML ids do not exist outside the
        // component.
        objectName: "userField"
        readOnly: form.readOnly
        placeholderText: !form.readOnly ? ""
            : form.savedAuthentication === "guest" ? "None — guest access"
            : form.savedAuthentication === "credentials" ? "Unknown — not in your saved settings"
            : "Unknown"
        Layout.fillWidth: true
        // Guest selection clears the fields it disables below, not just
        // visually hides them -- otherwise stale text
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
        enabled: userField.text.length > 0 || (form.readOnly && form.savedAuthentication !== "guest")
        readOnly: form.readOnly
        placeholderText: !form.readOnly ? ""
            : form.savedAuthentication === "credentials" ? "****"
            : form.savedAuthentication === "guest" ? ""
            : "Unknown"
        Layout.fillWidth: true
        onTextEdited: form.credentialsSealed = true
    }

    QQC2.Label { text: form.readOnly ? "Domain:" : "Domain (optional):" }
    QQC2.TextField {
        id: domainField
        objectName: "domainField"
        enabled: userField.text.length > 0 || form.savedUsernameUnknown
        readOnly: form.readOnly
        placeholderText: form.savedUsernameUnknown ? "Unknown" : ""
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
        objectName: "readOnlyRadio"
        enabled: !form.readOnly
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
        objectName: "readWriteRadio"
        enabled: !form.readOnly
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
        objectName: "executableRadio"
        enabled: !form.readOnly
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
     * Applies one suggestion, or refuses to, returning whether it was applied.
     * The whole tuple or none of it: a cached username beside a typed password
     * is a credential that was never valid anywhere. Username is assigned
     * first and only when non-empty, so the guest-clearing handler above
     * cannot run between the three assignments.
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

    /**
     * Fills the read-only view with a saved share: `values` carries the
     * model's remoteUrl, mountPoint, authentication, username, domain and
     * access. Sealed first, so no credential suggestion can ever land in a
     * view of a saved share. Username is assigned before Domain because the
     * guest handler clears Domain whenever Username becomes empty.
     */
    function showDefinition(values) {
        form.credentialsSealed = true
        form.urlFilledUsername = ""
        form.savedAuthentication = values.authentication ? values.authentication : ""
        uncField.text = values.remoteUrl ? values.remoteUrl : ""
        pathField.text = values.mountPoint ? values.mountPoint : ""
        userField.text = values.username ? values.username : ""
        domainField.text = values.domain ? values.domain : ""
        passwordField.text = ""
        // An unknown access mode checks nothing, rather than showing the
        // read-write default as if it were a fact about the share.
        readOnlyRadio.checked = values.access === "readonly"
        readWriteRadio.checked = values.access === "readwrite"
        executableRadio.checked = values.access === "readwrite-executable"
    }

    function reset() {
        uncField.text = "smb://"
        form.urlFilledUsername = ""
        pathField.text = ""
        userField.text = ""
        domainField.text = ""
        passwordField.text = ""
        // The KCM's Add dialog reuses one form instance, so without this the
        // previous share's access choice silently leaks into the next add.
        readWriteRadio.checked = true
        // An assignment is not an edit, so the seal needs clearing here, or
        // the KCM's reused instance carries one add's state into the next.
        form.credentialsSealed = false
    }

    // One lifecycle, no per-share switches: saving a share means it is armed
    // at boot and mounts on first access -- asking to mount a share *is*
    // asking for it to be there after a reboot.
    //
    // Access is the one exception, and only because it cannot be changed
    // later: there is no in-place Edit anywhere in this codebase, so this
    // submit is the single moment the choice exists. It is recorded in the
    // root-owned unit marker, not here.
    function submit() {
        // A view of a saved share never submits; canSubmit already says so,
        // and this holds even for a caller that skips it.
        if (form.readOnly) {
            return
        }
        // Sealed before the snapshot: a suggestion applied between the two
        // would change the visible fields but not what was submitted.
        form.credentialsSealed = true
        form.actions.addShare(effectiveUnc, pathField.text, userField.text, domainField.text,
                              passwordField.text, form.accessMode)
        form.submitted()
    }
}
