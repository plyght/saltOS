import QtQuick 2.15
import calamares.slideshow 1.0

Presentation {
    id: presentation

    function onActivate() {
        presentation.startAutoAdvance(15000)
    }

    function onLeave() {
        presentation.stopAutoAdvance()
    }

    Slide {
        anchors.fill: parent

        Image {
            id: logoSlide
            source: "logo.png"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 60
            width: 160
            fillMode: Image.PreserveAspectFit
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: logoSlide.bottom
            anchors.topMargin: 40
            horizontalAlignment: Text.AlignHCenter
            color: "#f2f4f7"
            font.pixelSize: 22
            text: "Welcome to saltOS\nA fast, current, rollbackable Unix-like system."
        }
    }

    Slide {
        anchors.fill: parent

        Text {
            anchors.centerIn: parent
            horizontalAlignment: Text.AlignHCenter
            color: "#f2f4f7"
            font.pixelSize: 20
            text: "Every update creates a rollback point.\nIf an upgrade breaks the machine, run salt rollback and reboot."
        }
    }

    Slide {
        anchors.fill: parent

        Text {
            anchors.centerIn: parent
            horizontalAlignment: Text.AlignHCenter
            color: "#f2f4f7"
            font.pixelSize: 20
            text: "Curated, signed packages from a small official repository.\nNo untrusted user repository by default."
        }
    }

    Slide {
        anchors.fill: parent

        Text {
            anchors.centerIn: parent
            horizontalAlignment: Text.AlignHCenter
            color: "#f2f4f7"
            font.pixelSize: 20
            text: "A lightweight LXQt desktop with Helium.\nInstalling now. This will only take a few minutes."
        }
    }
}
