/*
 * The KCM's Details view of a saved share: the shared ShareForm in its
 * read-only mode.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mirrors ShareAddDialog.qml on purpose: a saved share is shown in exactly
 * the form that created it — same fields, same order, same words — so what
 * the Details view says can be compared with what was typed. It is a view,
 * not an edit path: the form cannot submit in read-only mode, and there is no
 * Save button here to add one. Changing a share is still Delete then Add.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2

QQC2.Dialog {
    id: dialog
    modal: true
    title: "Network mount details"
    standardButtons: QQC2.Dialog.Close
    width: Math.min((parent ? parent.width : 640) - 40, 520)

    /** `share` carries the row's remoteUrl, mountPoint, authentication,
     *  username, domain and access roles. */
    function openFor(share) {
        form.showDefinition(share)
        open()
    }

    contentItem: ShareForm {
        id: form
        actions: kcm.actions
        readOnly: true
    }
}
