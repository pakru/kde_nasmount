/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kcmutils as KCMUtils

Item {
    id: root
    implicitWidth: 640
    implicitHeight: 480

    KCMUtils.ConfigModule.buttons: KCMUtils.ConfigModule.NoAdditionalButton

    // State is shown only as text (model.stateText), so nothing here tracks
    // Session::DisplayState's ordering. Do not mirror its numeric values in
    // QML: that is a mirror QML cannot check -- it resolves these at
    // runtime, so a reorder in mountmodel.h would silently mismatch rather
    // than fail to build.

    Connections {
        target: kcm.actions
        function onStarted(id, kind) {
            statusLabel.text = ""
            busyIndicator.running = true
        }
        function onFinished(id, kind, success, message) {
            busyIndicator.running = false
            statusLabel.text = message
            kcm.shareModel.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            QQC2.Label {
                text: "Network mounts, mounted on demand — armed at startup, before anyone signs in"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            QQC2.BusyIndicator {
                id: busyIndicator
                running: false
                visible: running
                implicitWidth: 24
                implicitHeight: 24
            }
            QQC2.Button {
                text: "Add…"
                icon.name: "list-add"
                onClicked: addDialog.openForAdd()
            }
        }

        // Global boot-coordinator health, never overriding a specific share's
        // own row state. Every share is boot-armed, so this is relevant
        // whenever any share exists at all.
        QQC2.Pane {
            visible: kcm.shareModel.hasShares || !kcm.shareModel.bootHealthy
            contentItem: RowLayout {
                QQC2.Label {
                    text: kcm.shareModel.bootHealthy ? "✓" : "⚠"
                }
                QQC2.Label {
                    text: "Boot coordinator: " + kcm.shareModel.bootHealthText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
            Layout.fillWidth: true
        }

        QQC2.ScrollView {
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: listView
                model: kcm.shareModel
                spacing: 6

                delegate: QQC2.Pane {
                    id: delegateRoot
                    width: listView.width

                    required property string shareId
                    required property string unc
                    required property string mountPoint
                    required property string stateText
                    required property string detail
                    required property bool hasUnitFiles
                    required property bool hasStoreRecord
                    required property bool drift
                    required property bool canRemoveDefinition
                    required property bool canRemoveLocalRecord
                    required property bool requiresAdministrator
                    required property string access

                    contentItem: RowLayout {
                        spacing: 8

                        ColumnLayout {
                            spacing: 2
                            Layout.fillWidth: true
                            RowLayout {
                                spacing: 6
                                QQC2.Label {
                                    text: delegateRoot.mountPoint
                                    font.bold: true
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                QQC2.Label {
                                    visible: delegateRoot.drift
                                    text: "⚠ drift"
                                    color: palette.text
                                    opacity: 0.8
                                }
                            }
                            QQC2.Label {
                                // The access mode is shown only when it is not
                                // the default. There is no Edit, so this line
                                // is the only way to tell a read-only or
                                // executable share from an ordinary one
                                // without reading its unit file by hand.
                                readonly property string accessText:
                                    delegateRoot.access === "readonly" ? "  ·  read-only"
                                    : delegateRoot.access === "readwrite-executable" ? "  ·  executable files"
                                    : ""
                                text: delegateRoot.unc + "  —  " + delegateRoot.stateText + accessText
                                      + (delegateRoot.detail.length > 0 ? ("  (" + delegateRoot.detail + ")") : "")
                                opacity: 0.7
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        // There are no runtime verbs: a share is armed at
                        // boot and mounts on first access, so there is
                        // nothing to connect, arm, or mount by hand. What
                        // remains is removal.

                        // --- removal, driven directly by the backend's own
                        // actionability booleans -- QML never reproduces the
                        // safety rule behind them. ---------------------------
                        QQC2.Button {
                            visible: delegateRoot.hasStoreRecord && delegateRoot.hasUnitFiles && delegateRoot.canRemoveDefinition
                            text: "Delete"
                            onClicked: deleteConfirm.openFor(delegateRoot.shareId, delegateRoot.mountPoint)
                        }
                        QQC2.Button {
                            visible: !delegateRoot.hasStoreRecord && delegateRoot.hasUnitFiles && delegateRoot.canRemoveDefinition
                            text: "Remove"
                            onClicked: kcm.actions.removeOrphanByPath(delegateRoot.mountPoint)
                        }
                        QQC2.Button {
                            visible: delegateRoot.hasStoreRecord && delegateRoot.canRemoveLocalRecord
                            text: "Remove record"
                            onClicked: kcm.actions.removeOrphanedRecord(delegateRoot.shareId)
                        }

                        // --- administrator-only (Tampered/NotOurs/untrusted-
                        // active): visible, never casually actionable -------
                        QQC2.Label {
                            visible: delegateRoot.requiresAdministrator
                            text: "Requires administrator repair"
                            opacity: 0.7
                            font.italic: true
                        }
                    }
                }

                QQC2.Label {
                    anchors.centerIn: parent
                    visible: listView.count === 0
                    text: "No network mounts yet — click Add… to create one."
                    opacity: 0.6
                }
            }
        }

        QQC2.Label {
            id: statusLabel
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: root.palette.text
            Layout.fillWidth: true
        }
    }

    ShareAddDialog {
        id: addDialog
        parent: root
        anchors.centerIn: parent
    }

    QQC2.Dialog {
        id: deleteConfirm

        property string targetId: ""

        parent: root
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        title: "Remove network mount"
        standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel

        onAccepted: kcm.actions.deleteShare(targetId)

        QQC2.Label {
            id: label
            width: parent.width
            wrapMode: Text.WordWrap
        }

        function openFor(id, mountPoint) {
            targetId = id
            label.text = "Remove the definition for " + mountPoint + "?\n\n"
                       + "If it is currently mounted, it will be disarmed and unmounted first."
            open()
        }
    }
}
