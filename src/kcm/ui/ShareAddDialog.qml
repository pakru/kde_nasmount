/*
 * The KCM's Add wrapper around the shared ShareForm.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything about the form itself lives in ShareForm.qml, which
 * nasmount-dialog embeds verbatim. This file supplies only what is specific
 * to being a modal dialog inside the KCM page: the window chrome, the Add
 * button, and the `kcm.actions` binding. Do not reintroduce form fields here
 * — a field added on one side and not the other makes the two front ends
 * drift apart.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2

QQC2.Dialog {
    id: dialog
    modal: true
    title: "Add network mount"
    standardButtons: QQC2.Dialog.Cancel
    width: Math.min((parent ? parent.width : 640) - 40, 520)

    function openForAdd() {
        form.reset()
        open()
    }

    contentItem: ColumnLayout {
        spacing: 6

        ShareForm {
            id: form
            actions: kcm.actions
            Layout.fillWidth: true
            onSubmitted: dialog.close()
        }

        QQC2.Button {
            text: "Add"
            enabled: form.canSubmit
            Layout.alignment: Qt.AlignRight
            onClicked: form.submit()
        }
    }
}
