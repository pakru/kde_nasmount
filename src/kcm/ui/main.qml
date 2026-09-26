/*
 * The KCM page: the list of network mounts, laid out as a table like the
 * other System Settings modules (Autostart is the model), with Add in the
 * page header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each row is a mount point over its smb:// address, then fixed-width access
 * and status columns, then Open, Details and Remove. A third line appears
 * only when the row has a problem to explain. Rows are not clickable; only
 * the buttons act.
 *
 * Every decision about a row arrives from the model as a finished value —
 * its state text and severity, its access text (blank when unknown), which
 * one removal it offers and why none when it offers none. This file never
 * recombines raw facts into a rule of its own, and never mirrors
 * Session::DisplayState's numeric values: QML resolves those at runtime, so a
 * reorder in mountmodel.h would silently mismatch rather than fail to build.
 *
 * There are no runtime verbs: a share is armed at boot and mounts on first
 * access, so there is nothing to connect, arm or mount by hand. Open shows
 * the mount point in the file manager, which mounts it by looking.
 */

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCMUtils

KCMUtils.ScrollViewKCM {
    id: root

    implicitWidth: Kirigami.Units.gridUnit * 44
    implicitHeight: Kirigami.Units.gridUnit * 26

    KCMUtils.ConfigModule.buttons: KCMUtils.ConfigModule.NoAdditionalButton

    actions: [
        Kirigami.Action {
            text: "Add…"
            icon.name: "list-add"
            onTriggered: addDialog.openForAdd()
        }
    ]

    Connections {
        target: kcm.actions
        function onStarted(id, kind) {
            statusMessage.visible = false
            busyIndicator.running = true
        }
        function onFinished(id, kind, success, message) {
            busyIndicator.running = false
            root.showStatus(success, message)
            kcm.shareModel.refresh()
        }
    }

    Connections {
        target: kcm
        function onOpenFailed(message) {
            root.showStatus(false, message)
        }
    }

    function showStatus(success, message) {
        if (message.length === 0) {
            statusMessage.visible = false
            return
        }
        statusMessage.type = success ? Kirigami.MessageType.Positive : Kirigami.MessageType.Error
        statusMessage.text = message
        statusMessage.visible = true
    }

    // Global boot-coordinator health, never overriding a specific share's
    // own row state. Shown only when something is wrong: a healthy boot
    // coordinator is the normal case and needs no line of its own, while an
    // unhealthy one explains why shares may not be armed after a reboot.
    header: RowLayout {
        visible: !kcm.shareModel.bootHealthy
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            source: "dialog-warning"
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
        }
        QQC2.Label {
            text: "nasmount-boot: " + kcm.shareModel.bootHealthText
            color: Kirigami.Theme.neutralTextColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    view: ListView {
        id: listView
        model: kcm.shareModel
        clip: true

        // Measured once for the whole list, so every row's access and status
        // columns line up — that is what makes the list read as a table.
        readonly property real accessColumnWidth: accessMetrics.advanceWidth + Kirigami.Units.largeSpacing
        readonly property real statusColumnWidth:
            statusMetrics.advanceWidth + Kirigami.Units.iconSizes.small + Kirigami.Units.largeSpacing

        TextMetrics {
            id: accessMetrics
            text: "Read & Write & Execute"
        }
        TextMetrics {
            id: statusMetrics
            text: "Missing credentials"
        }

        // Rows arrive with every managed share first and foreign mounts last
        // (see MountModel's source 4), so each section is one block.
        section.property: "section"
        section.delegate: Kirigami.ListSectionHeader {
            required property string section
            width: ListView.view.width
            text: section === "foreign" ? "Mounted by other tools" : "Network mounts"
        }

        delegate: QQC2.Control {
            id: row

            required property string shareId
            required property string mountPoint
            required property string remoteUrl
            required property string stateText
            required property string severity
            required property string detail
            required property string access
            required property string accessText
            required property string authentication
            required property string username
            required property string domain
            required property string removal
            required property string removalBlockedReason

            readonly property color severityColor: severity === "error" ? Kirigami.Theme.negativeTextColor
                                                  : severity === "warning" ? Kirigami.Theme.neutralTextColor
                                                  : Kirigami.Theme.textColor

            width: ListView.view.width
            topPadding: Kirigami.Units.smallSpacing * 2
            bottomPadding: Kirigami.Units.smallSpacing * 2
            leftPadding: Kirigami.Units.largeSpacing
            rightPadding: Kirigami.Units.largeSpacing
            // No hover or pressed painting: rows are not clickable.
            background: null

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: "folder-network"
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                    Layout.alignment: Qt.AlignTop
                }

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true

                    QQC2.Label {
                        text: row.mountPoint
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        text: row.remoteUrl
                        elide: Text.ElideMiddle
                        opacity: 0.7
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        visible: row.detail.length > 0
                        text: row.detail
                        color: row.severityColor
                        wrapMode: Text.WordWrap
                        font: Kirigami.Theme.smallFont
                        Layout.fillWidth: true
                    }
                }

                QQC2.Label {
                    text: row.accessText
                    opacity: 0.7
                    elide: Text.ElideRight
                    Layout.preferredWidth: listView.accessColumnWidth
                    Layout.maximumWidth: listView.accessColumnWidth
                }

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    Layout.preferredWidth: listView.statusColumnWidth
                    Layout.maximumWidth: listView.statusColumnWidth

                    Kirigami.Icon {
                        visible: row.severity !== "normal"
                        source: row.severity === "error" ? "dialog-error" : "dialog-warning"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: Kirigami.Units.iconSizes.small
                    }
                    QQC2.Label {
                        text: row.stateText
                        color: row.severityColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                QQC2.ToolButton {
                    icon.name: "document-open-folder"
                    text: "Open in file manager"
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: kcm.openMountPoint(row.mountPoint)
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: text
                }

                QQC2.ToolButton {
                    icon.name: "document-properties"
                    text: "Details"
                    display: QQC2.AbstractButton.IconOnly
                    onClicked: detailsDialog.openFor({
                        remoteUrl: row.remoteUrl,
                        mountPoint: row.mountPoint,
                        authentication: row.authentication,
                        username: row.username,
                        domain: row.domain,
                        access: row.access
                    })
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: text
                }

                // Always present, so the buttons line up down the list;
                // disabled when the row offers no removal. A disabled control
                // receives no hover, so the tooltip hangs off this wrapper,
                // whose HoverHandler still sees the pointer.
                Item {
                    implicitWidth: removeButton.implicitWidth
                    implicitHeight: removeButton.implicitHeight

                    HoverHandler {
                        id: removeHover
                    }

                    QQC2.ToolButton {
                        id: removeButton
                        anchors.fill: parent
                        icon.name: "edit-delete-remove"
                        text: row.removal === "removeOrphan" ? "Remove the orphan definition"
                            : row.removal === "removeRecord" ? "Remove the local record"
                            : "Remove"
                        display: QQC2.AbstractButton.IconOnly
                        enabled: row.removal.length > 0
                        onClicked: removeConfirm.openFor(row.removal, row.shareId, row.mountPoint)
                    }

                    QQC2.ToolTip.visible: removeHover.hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    QQC2.ToolTip.text: row.removal.length > 0 ? removeButton.text : row.removalBlockedReason
                }
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: listView.count === 0
            text: "No network mounts yet"
            explanation: "Click Add… to create one."
        }
    }

    footer: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        QQC2.BusyIndicator {
            id: busyIndicator
            running: false
            visible: running
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            Layout.alignment: Qt.AlignHCenter
        }
        Kirigami.InlineMessage {
            id: statusMessage
            visible: false
            showCloseButton: true
            Layout.fillWidth: true
        }
    }

    ShareAddDialog {
        id: addDialog
        parent: root
        anchors.centerIn: parent
    }

    ShareDetailsDialog {
        id: detailsDialog
        parent: root
        anchors.centerIn: parent
    }

    // One confirmation for all three removals. Remove record is local and
    // needs no password, which is exactly why it must not stay a one-click
    // action behind the same red X as the other two.
    QQC2.Dialog {
        id: removeConfirm

        property string kind: ""
        property string targetId: ""
        property string targetMountPoint: ""

        parent: root
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        title: kind === "removeRecord" ? "Remove saved settings" : "Remove network mount"
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel

        onAccepted: {
            if (kind === "delete") {
                kcm.actions.deleteShare(targetId)
            } else if (kind === "removeOrphan") {
                kcm.actions.removeOrphanByPath(targetMountPoint)
            } else if (kind === "removeRecord") {
                kcm.actions.removeOrphanedRecord(targetId)
            }
        }

        QQC2.Label {
            id: confirmLabel
            width: parent.width
            wrapMode: Text.WordWrap
        }

        function openFor(removalKind, id, mountPoint) {
            kind = removalKind
            targetId = id
            targetMountPoint = mountPoint
            if (removalKind === "delete") {
                confirmLabel.text = "Remove the network mount at " + mountPoint + "?\n\n"
                                  + "If it is currently mounted, it will be unmounted first."
            } else if (removalKind === "removeOrphan") {
                confirmLabel.text = "Remove the definition at " + mountPoint + "?\n\n"
                                  + "You have no saved settings for it, so adding it again means "
                                  + "entering everything anew."
            } else {
                confirmLabel.text = "Remove your saved settings for " + mountPoint + "?\n\n"
                                  + "No mount definition is touched and nothing is unmounted."
            }
            open()
        }
    }
}
