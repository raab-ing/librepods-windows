import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

Window {
    id: root
    title: "Proximity Keys"
    flags: Qt.Dialog
    modality: Qt.WindowModal

    SystemPalette { id: systemPalette }
    readonly property bool darkMode: systemPalette.window.hslLightness < 0.5
    readonly property color surfaceColor:  darkMode ? "#252525" : "#f5f5f5"
    readonly property color cardColor:     darkMode ? "#303030" : "#ffffff"
    readonly property color borderColor:   darkMode ? "#3d3d3d" : "#d8d8d8"
    readonly property color textPrimary:   darkMode ? "#f2f2f2" : "#1a1a1a"
    readonly property color textSecondary: darkMode ? "#a8a8a8" : "#666666"
    color: surfaceColor

    width: Math.min(Screen.width * 0.8, 340)
    height: Math.min(Screen.height * 0.9, 560)

    property string irk: ""
    property string encKey: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14

        // QR card — light surface kept for scanner contrast
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: width
            radius: 10
            color: "#ffffff"
            border.color: root.borderColor

            Image {
                id: qrCodeImage
                anchors.centerIn: parent
                width: Math.min(parent.width * 0.86, parent.height * 0.86)
                height: width
                fillMode: Image.PreserveAspectFit
                source: "image://qrcode/" + root.encKey + ";" + root.irk

                BusyIndicator {
                    anchors.centerIn: parent
                    running: qrCodeImage.status === Image.Loading
                }

                Label {
                    anchors.centerIn: parent
                    visible: qrCodeImage.status === Image.Error
                    text: "Failed to generate QR code"
                    color: "#1a1a1a"
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: "Scan with the LibrePods Android app to transfer these keys"
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: root.textPrimary
            font.pixelSize: 13
            font.weight: Font.Medium
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: infoText.implicitHeight + 24
            radius: 10
            color: root.cardColor
            border.color: root.borderColor

            ColumnLayout {
                id: infoText
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: root.textSecondary
                    font.pixelSize: 11
                    text: "Your AirPods broadcast a rotating Bluetooth address for privacy. "
                        + "The IRK lets a trusted device recognize that address as yours, and "
                        + "the ENC key decrypts the broadcasts for battery and in-ear state — "
                        + "so status stays live even without an active connection."
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: root.textSecondary
                    font.pixelSize: 11
                    text: "Keep this code private — anyone holding these keys can track your "
                        + "pods' broadcasts."
                }
            }
        }
    }
}
