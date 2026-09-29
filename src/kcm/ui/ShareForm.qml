/*
 * ShareForm - the add-a-share form, shared verbatim by both front ends: the
 * KCM wraps it in a QQC2.Dialog, nasmount-dialog wraps it in a window.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file must stay host-agnostic. It never references `kcm` (or any other
 * host context object) - the MountActions instance arrives through the
 * `actions` property, and every other host difference is a property too. That
 * is what lets one definition serve both entry points; a single `kcm.` here
 * would silently make it KCM-only, and the two front ends would drift apart.
 *
 * Validation here is convenience only. UnitSpec re-validates every field in
 * the privileged helper, which is the boundary that actually matters. The
 * form holds no rules of its own: each field's verdict is a string from
 * `actions` (empty means fine), computed by the functions the submit path and
 * the helper use, so a value the form accepts is one the submit accepts.
 * Share and Mount point are checked as they are typed. Mount point has a
 * second, slower verdict - what is on disk there - that arrives later from a
 * worker thread and is applied only if the field still holds the text it was
 * asked about.
 *
 * A problem always gates Add, but it is only *shown* once the field has been
 * revealed: after a pause in typing, on leaving the field, or when a value
 * arrives finished (Browse, a host's pre-filled path). That way the pre-filled
 * "smb://" and an empty Mount point are not scolded on open, and a message
 * goes away the moment the value is fine.
 *
 * The share is typed as smb://host/share (//host/share is still accepted);
 * MountActions::addShare() resolves either to the //host/share form the
 * helper and the unit use. When the address names a user, that user fills an
 * empty Username as the address is typed. Its Browse button opens the platform
 * folder dialog at smb://, and what is picked goes into the field as if typed;
 * a user in the picked URL fills Username by the same rule and never stays in
 * the address.
 *
 * Credentials the host imports (applyCredentialSuggestion) are all-or-nothing
 * and one-shot. Where the share can change - the KCM types it - an imported
 * login is also bound to the share it was found for: a suggestion is refused
 * unless the field still holds the address it was requested for, and an
 * applied one is withdrawn if the address moves to another server or share, so
 * a password found for one machine can never be submitted to another.
 *
 * `readOnly` turns the same form into a view of a saved share - the KCM's
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

    /** Why the Share text cannot be submitted, or empty. Never set for a fixed
     *  share (the host vouches for it) or a read-only view. */
    readonly property string shareProblem: (form.readOnly || form.fixedUnc.length > 0 || !form.actions)
        ? "" : form.actions.shareInputProblem(uncField.text, userField.text)

    /** Why the Mount point text cannot be one, or empty. Lexical, so cheap
     *  enough to re-evaluate on every keystroke. */
    readonly property string mountPointProblem: (form.readOnly || !form.actions)
        ? "" : form.actions.mountPointProblem(pathField.text)

    /** What is wrong with the folder itself - not empty, already mounted,
     *  another mount's unit - as last reported by the worker thread for the
     *  text now in the field. Empty until an answer arrives, and again as soon
     *  as the text changes; while it is empty nothing is known, which is not
     *  a reason to stop the user from adding. */
    property string mountPointFsProblem: ""

    property bool shareRevealed: false
    property bool mountPointRevealed: false

    /** The messages under the fields: a problem, once its field is revealed. */
    readonly property string shareShown: form.shareBrowseError.length > 0 ? form.shareBrowseError
        : form.shareRevealed ? form.shareProblem : ""
    readonly property string mountPointShown: form.mountBrowseError.length > 0 ? form.mountBrowseError
        : !form.mountPointRevealed ? ""
        : form.mountPointProblem.length > 0 ? form.mountPointProblem
        : form.mountPointFsProblem

    /** Add is allowed when nothing is known to be wrong. A check that has not
     *  answered does not block it: the helper still refuses what was missed,
     *  and a walk that never returned must not lock the button. */
    readonly property bool canSubmit: !form.readOnly && !!form.actions
        && form.shareProblem.length === 0
        && form.mountPointProblem.length === 0
        && form.mountPointFsProblem.length === 0

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

    /** Colour of the messages this form shows under a field. A plain
     *  property because the form may not import Kirigami: the host passes its
     *  theme's negative text colour, and the default is Breeze's. */
    property color errorColor: "#da4453"

    /** Why the last mount-point Browse pick was not used, or empty. Cleared
     *  by the next edit or pick, and by reset(). */
    property string mountBrowseError: ""

    /** The same for the Share Browse. */
    property string shareBrowseError: ""

    /** Emitted when a Browse pick has produced a share worth looking a login
     *  up for. The host decides whether it can (the KCM runs the lookup child;
     *  the service menu has its own path) and answers, if at all, through
     *  applyCredentialSuggestion(). `user` is the picked URL's own user, the
     *  only evidence of which account is meant. */
    signal credentialLookupRequested(string shareInput, string user)

    /** The Share text the last lookup was requested for. A suggestion is only
     *  accepted while the field still holds exactly this. */
    property string lookupShare: ""

    /** What an applied suggestion belongs to: the password-service key of the
     *  address it was found for (MountActions::lookupTargetOf()), and whether
     *  the password and the username/domain are still exactly as imported.
     *  Only flags and a key - the imported values themselves are never kept
     *  anywhere but the fields. */
    property string importedTarget: ""
    property bool importedPassword: false
    property bool importedIdentity: false

    /** Emitted after a submit has been handed to `actions`. It says only that
     *  the hand-over happened: the outcome arrives later through the actions
     *  object's finished() signal, and each host decides what to do with it
     *  (the KCM dialog stays open until then, the window shows a result
     *  dialog). */
    signal submitted()

    spacing: 6

    /** The frame that marks a field as having a problem. A child of the field
     *  and not a replacement for its background, so the field keeps the
     *  style's own look. */
    component FieldFrame: Rectangle {
        property string problem: ""
        property color frameColor
        anchors.fill: parent
        color: "transparent"
        border.color: frameColor
        border.width: 1
        radius: 3
        visible: problem.length > 0
    }

    /** The message under a field. */
    component FieldMessage: QQC2.Label {
        property string problem: ""
        visible: problem.length > 0
        text: problem
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    // Slow to complain: a problem is shown once typing pauses. Quick to
    // forgive: the message is bound to the problem, so it goes with it.
    Timer {
        id: shareIdle
        interval: 600
        onTriggered: form.shareRevealed = true
    }
    Timer {
        id: mountPointIdle
        interval: 600
        onTriggered: form.mountPointRevealed = true
    }
    Timer {
        id: fsCheckIdle
        interval: 400
        onTriggered: form.runFsCheck()
    }

    // The worker thread's answer, applied only if it is about the text that
    // is in the field now. Every form on the same `actions` hears every
    // answer, and the KCM keeps a read-only Details form beside the Add one:
    // without the readOnly test, adding a share at the path of a saved one
    // would paint "another mount already uses this folder" onto its Details.
    Connections {
        target: form.actions
        ignoreUnknownSignals: true
        function onMountPointChecked(raw, problem) {
            if (!form.readOnly && raw === pathField.text) {
                form.mountPointFsProblem = problem
            }
        }
    }

    // What is on disk changes while the user is elsewhere - they empty a
    // folder in the file manager and come back - so coming back asks again.
    Connections {
        target: form.Window.window
        ignoreUnknownSignals: true
        function onActiveChanged() {
            if (form.Window.window && form.Window.window.active) {
                form.runFsCheck()
            }
        }
    }

    Component.onCompleted: {
        // A path the host filled in before the form existed (the service menu
        // suggests one) is finished input, not typing in progress.
        if (pathField.text.length > 0) {
            form.mountPointRevealed = true
            form.runFsCheck()
        }
    }

    QtDialogs.FolderDialog {
        id: folderDialog
        onAccepted: form.applyBrowsedMountPoint(selectedFolder)
    }

    // The platform's own folder dialog. Under Plasma it is KDE's, backed by
    // KIO, which lists smb:// out of process and puts up its own
    // authentication prompt for a protected share; this module links none of
    // that. Read-only, because picking a share must not create folders on the
    // server.
    QtDialogs.FolderDialog {
        id: shareDialog
        title: "Choose a network share"
        options: QtDialogs.FolderDialog.ReadOnly
        onAccepted: form.applyBrowsedShare(selectedFolder)
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
    RowLayout {
        visible: form.fixedUnc.length === 0
        Layout.fillWidth: true
        QQC2.TextField {
            id: uncField
            objectName: "uncField"
            readOnly: form.readOnly
            Layout.fillWidth: true
            Accessible.description: form.shareShown
            onTextEdited: {
                form.shareBrowseError = ""
                form.fillUsernameFromAddress(form.actions ? form.actions.userInShareInput(text) : "")
                shareIdle.restart()
            }
            // A login imported for the old address must not follow the field
            // to another share, however the text changed.
            onTextChanged: form.withdrawImportedCredential()
            onEditingFinished: form.shareRevealed = true

            FieldFrame {
                problem: form.shareShown
                frameColor: form.errorColor
            }
        }
        QQC2.Button {
            objectName: "shareBrowseButton"
            visible: !form.readOnly
            text: "Browse…"
            onClicked: {
                // Decided at the click, not bound: the dialog opens where the
                // address in the field points now.
                shareDialog.currentFolder = form.actions.browseStartUrl(uncField.text)
                shareDialog.open()
            }
        }
    }
    FieldMessage {
        objectName: "shareMessage"
        problem: form.shareShown
        color: form.errorColor
    }

    QQC2.Label { text: "Mount point:" }
    RowLayout {
        Layout.fillWidth: true
        QQC2.TextField {
            id: pathField
            objectName: "pathField"
            placeholderText: form.readOnly ? "" : "/home/you/ShareName"
            readOnly: form.readOnly
            Layout.fillWidth: true
            Accessible.description: form.mountPointShown
            // Any change, typed or assigned, makes the last on-disk answer
            // about some other text.
            onTextChanged: form.mountPointFsProblem = ""
            onTextEdited: {
                form.mountBrowseError = ""
                mountPointIdle.restart()
                fsCheckIdle.restart()
            }
            onEditingFinished: {
                form.mountPointRevealed = true
                form.runFsCheck()
            }

            FieldFrame {
                problem: form.mountPointShown
                frameColor: form.errorColor
            }
        }
        QQC2.Button {
            objectName: "browseButton"
            visible: !form.readOnly
            text: "Browse…"
            onClicked: folderDialog.open()
        }
    }
    FieldMessage {
        objectName: "mountPointMessage"
        problem: form.mountPointShown
        color: form.errorColor
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
            : form.savedAuthentication === "guest" ? "None - guest access"
            : form.savedAuthentication === "credentials" ? "Unknown - not in your saved settings"
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
        onTextEdited: {
            form.credentialsSealed = true
            form.importedIdentity = false
        }
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
        onTextEdited: {
            form.credentialsSealed = true
            form.importedPassword = false
        }
    }

    QQC2.Label { text: form.readOnly ? "Domain:" : "Domain (optional):" }
    QQC2.TextField {
        id: domainField
        objectName: "domainField"
        enabled: userField.text.length > 0 || form.savedUsernameUnknown
        readOnly: form.readOnly
        placeholderText: form.savedUsernameUnknown ? "Unknown" : ""
        Layout.fillWidth: true
        onTextEdited: {
            form.credentialsSealed = true
            form.importedIdentity = false
        }
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
        // A fixed share cannot move. An editable one can, between the request
        // and the answer, and the answer is about the address it was asked
        // for: a login found for one server does not belong to another.
        if (form.fixedUnc.length === 0
                && (form.lookupShare.length === 0 || uncField.text !== form.lookupShare)) {
            return false
        }
        userField.text = username
        domainField.text = domain ? domain : ""
        passwordField.text = password ? password : ""
        if (form.fixedUnc.length === 0) {
            form.importedTarget = form.actions.lookupTargetOf(uncField.text)
            form.importedPassword = true
            form.importedIdentity = true
        }
        return true
    }

    /**
     * Takes back an imported login when the Share no longer names the share it
     * was found for (a different server, or a different share on it; a
     * subfolder of the same share keeps it). The password goes if it is still
     * the imported one, and with it the username and domain if those are too.
     * Anything the user typed in the meantime is theirs and stays: only what
     * this form put there is taken back. Flags and a key are all that is kept
     * to decide this - never the values.
     */
    function withdrawImportedCredential() {
        if (form.importedTarget.length === 0 || !form.actions) {
            return
        }
        if (form.actions.lookupTargetOf(uncField.text) === form.importedTarget) {
            return
        }
        // The flags first: clearing the username runs the guest handler, and
        // nothing below should be read back through a half-cleared form.
        const dropPassword = form.importedPassword
        const dropIdentity = form.importedIdentity && form.importedPassword
        form.importedTarget = ""
        form.importedPassword = false
        form.importedIdentity = false
        if (dropPassword) {
            passwordField.text = ""
        }
        if (dropIdentity) {
            userField.text = ""
            domainField.text = ""
        }
    }

    /** Fills Username from the user an address names - typed, or picked in
     *  the Share browser - only while it is empty or still holds what an
     *  address put there, and never clears it: clearing Username would run
     *  the guest handler and wipe a typed password and domain. An assignment,
     *  not an edit, so it does not seal credentials either. */
    function fillUsernameFromAddress(user) {
        if (user.length > 0
                && (userField.text.length === 0 || userField.text === form.urlFilledUsername)) {
            userField.text = user
            form.urlFilledUsername = user
        }
    }

    /**
     * Puts the share picked in the Share browser into the field, as if typed.
     * The user in the picked URL fills Username but stays out of the address.
     * A pick that is not a share on the network leaves the field as it was and
     * says so. A usable pick is finished input: its problem, if any, shows at
     * once, and a login is asked for.
     */
    function applyBrowsedShare(url) {
        const picked = form.actions.browsedShareInput(url)
        form.shareBrowseError = picked.error
        if (picked.error.length > 0) {
            return
        }
        uncField.text = picked.input
        form.fillUsernameFromAddress(picked.user)
        form.shareRevealed = true
        form.requestCredentialLookup(picked.user)
    }

    /** Asks the host for a login for the Share as it stands, unless there is
     *  nothing to ask about or the user has already taken the credential
     *  fields over. */
    function requestCredentialLookup(user) {
        if (form.readOnly || form.fixedUnc.length > 0 || !form.actions
                || form.credentialsSealed || form.shareProblem.length > 0) {
            return
        }
        form.lookupShare = uncField.text
        form.credentialLookupRequested(uncField.text, user)
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

    /**
     * Puts the folder picked in the mount point's Browse dialog into the
     * field. The path comes from the URL through `actions`, never from its
     * string form: that keeps `%` and `#` percent-encoded, so a folder named
     * "100%" would become "100%25" and the helper would create and mount on a
     * different directory than the one picked. A pick that is not a local
     * folder (KDE's dialog can also browse smb://) leaves the field as it was.
     */
    function applyBrowsedMountPoint(url) {
        const path = form.actions.localPathFromUrl(url)
        if (path.length === 0) {
            form.mountBrowseError = "Choose a local folder"
            return
        }
        form.mountBrowseError = ""
        pathField.text = path
        // A pick is finished input, not typing in progress.
        form.mountPointRevealed = true
        form.runFsCheck()
    }

    /**
     * Asks the worker thread what is on disk at the Mount point, unless the
     * text is not even a mount point (its own message covers that) or this is
     * a view of a saved share. The answer comes back through
     * mountPointChecked() and is applied only if the field still holds the
     * text it was asked about.
     */
    function runFsCheck() {
        fsCheckIdle.stop()
        if (form.readOnly || !form.actions || form.mountPointProblem.length > 0) {
            return
        }
        form.actions.checkMountPoint(pathField.text)
    }

    function reset() {
        shareIdle.stop()
        mountPointIdle.stop()
        fsCheckIdle.stop()
        uncField.text = "smb://"
        form.urlFilledUsername = ""
        form.mountBrowseError = ""
        form.shareBrowseError = ""
        form.lookupShare = ""
        form.importedTarget = ""
        form.importedPassword = false
        form.importedIdentity = false
        // Pristine again: the pre-filled prefix and the empty path are not
        // mistakes yet, so nothing is shown until the user has been at them.
        form.shareRevealed = false
        form.mountPointRevealed = false
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
