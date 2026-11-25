import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 首次运行许可协议（参照 lx-music-mobile 的 PactModal）：
// 未签署前全屏遮罩盖住整个应用，只有「接受」/「不接受」两个出口。
// 签署状态持久化在 settings.ini 的 [furui] pactAccepted；「不接受」直接退出应用。
Rectangle {
    id: root

    // 由 Main.qml 在 Component.onCompleted 里读一次设置；签署后写回并隐藏
    property bool accepted: false
    property int countdown: 20          // 与 lx-music 一致的 20 秒冷静期
    readonly property bool canAccept: accepted || countdown <= 0

    function init() {
        const v = app.setting("pactAccepted", false)
        // QSettings 写的是 QVariant(bool)，读回是布尔；兼容历史手改的 "true"/"false" 字符串
        accepted = (v === true) || String(v) === "true"
        if (!accepted) {
            visible = true
            forceActiveFocus()   // 抢走焦点：返回键落在这里被吞，不会退后台/关窗口
            countdownTimer.restart()
        }
    }

    function accept() {
        if (!canAccept) return
        const firstTime = !accepted
        accepted = true
        app.setSetting("pactAccepted", true)
        visible = false
        if (firstTime && afterAccept) {
            // 与 lx-music-mobile 相同的提示：完全免费且开源，别花冤枉钱
            afterAccept()
        }
    }

    function reject() {
        app.quitApp()
    }

    // 由 Main.qml 注入：首次签署成功后的提示回调（Toast 定义在 Main.qml，id 不跨文件）
    property var afterAccept: null

    // 协议未决时吞掉 Android 返回键：不允许退后台绕过协议（退后台回来协议还在，
    // 但按返回直接退后台会让用户误以为能跳过；这里统一拦掉）
    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape)
            event.accepted = true
    }

    anchors.fill: parent
    // 深色磨砂底：既压住底下所有内容，又不至于黑成一片死色
    color: theme.dark ? "#F2101013" : "#F2F4F5F7"

    // 吞掉一切点击：协议未签署前不允许穿透到主界面
    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(root.parent ? root.parent.width - 32 : 400, 480)
        // 手机竖屏给足阅读空间：占窗口高度的 ~72%，横屏矮窗钳到可用高度
        height: Math.min(root.parent ? root.parent.height - 32 : 600,
                         root.parent ? Math.max(320, root.parent.height * 0.8) : 600)
        radius: theme.radiusLg
        color: theme.card
        border.color: theme.line
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "许可协议"
                font.pixelSize: theme.fontLg
                font.weight: Font.DemiBold
                color: theme.text
            }

            Flickable {
                id: flick
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: pactText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds

                Text {
                    id: pactText
                    width: flick.width
                    wrapMode: Text.WordWrap
                    color: theme.text
                    font.pixelSize: theme.fontSm
                    textFormat: Text.RichText
                    text: root.agreementHtml
                }

                ScrollIndicator.vertical: ScrollIndicator { }
            }

            Label {
                Layout.fillWidth: true
                visible: !root.accepted
                wrapMode: Text.WordWrap
                text: "若你（使用者）接受以上协议，请点击下面的「接受」按钮签署本协议；若不接受，请点击「不接受」退出并清除本软件的所有数据。"
                font.pixelSize: theme.fontXs
                font.bold: true
                color: theme.subText
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceSm

                Button {
                    Layout.fillWidth: true
                    visible: !root.accepted
                    text: "不接受"
                    onClicked: root.reject()
                }
                Button {
                    Layout.fillWidth: true
                    highlighted: true
                    text: root.accepted ? "关闭"
                          : (root.canAccept ? "接受" : "接受（%1）".arg(root.countdown))
                    enabled: root.accepted || root.canAccept
                    onClicked: root.accept()
                }
            }
        }
    }

    Timer {
        id: countdownTimer
        interval: 1000
        repeat: true
        triggeredOnStart: false
        onTriggered: {
            if (root.countdown > 0) root.countdown--
            // 必须是倒计时 Timer 自己停：之前写成 root.stop()，对话框没有 stop() 方法，
            // 每秒抛一次 TypeError，Cold start 后一直刷到进程结束（Android 上实测每秒一条）。
            if (root.countdown <= 0) countdownTimer.stop()
        }
    }

    // 协议正文：参照 lx-music-mobile 的 PactModal 条款，按本项目改写
    readonly property string agreementHtml: {
        const b = "<b>"
        const e = "</b>"
        const p = "<br/>"
        return ""
            + p + b + "在使用本软件前，你（使用者）需签署本协议才可继续使用！" + e + p
            + "本软件仅用于学习与技术交流，所有材料（包括但不限于音乐数据、歌词、封面图片等）均来自互联网公开渠道搜集整理，本软件不存储、不分发任何版权数据，软件作者（APP 作者）不承担任何后果。" + p
            + b + "一、数据来源" + e + p
            + "1.1 本软件的数据来源是从各官方音乐平台的公开服务器中拉取数据（与未登录状态在官方平台获取的数据相同），经过简单筛选与合并后展示，本软件不对数据的准确性负责。" + p
            + "1.2 本软件本身没有获取音频数据的能力，播放所用在线链接来自第三方返回的地址；链接内容是否正确本软件无法校验，可能出现播放内容与预期不符或无法播放的情况。" + p
            + b + "二、版权数据" + e + p
            + "2.1 使用本软件的过程中可能产生版权数据。本软件不拥有这些数据的所有权。为避免侵权，使用者务必在" + b + "24 小时内" + e + "清除使用过程中产生的版权数据。" + p
            + b + "三、免责声明" + e + p
            + "3.1 由于使用本软件产生的任何性质的直接、间接、特殊、偶然或结果性损害（包括但不限于数据丢失、设备故障、商誉损失等），均由使用者自行负责，软件作者不承担任何责任。" + p
            + b + "四、使用限制" + e + p
            + "4.1 本软件完全免费，仅用作对技术的学习交流。禁止在违反当地法律法规的情况下使用本软件，由此造成的任何违法违规行为由使用者承担，软件作者不承担由此造成的任何责任。" + p
            + b + "五、非商业性质" + e + p
            + "5.1 本软件仅用于对技术可行性的探索及研究，不接受任何商业（包括但不限于广告等）合作及捐赠。" + p
            + b + "六、资源来源" + e + p
            + "6.1 本软件使用的部分资源（包括但不限于字体、图标、图片）来源于互联网搜集，如涉及侵权可联系作者移除。" + p
            + b + "七、版权保护" + e + p
            + "7.1 音乐平台不易，请尊重版权，支持正版。" + p
            + b + "八、接受协议" + e + p
            + "8.1 若你使用了本软件，将代表你接受本协议。" + p
            + "* 若协议更新，恕不另行通知。"
    }
}
