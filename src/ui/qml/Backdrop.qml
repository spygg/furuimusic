// 全窗口封面氛围底 —— 铺满整个窗口（含侧栏与状态栏区域），供外壳与页面共用。
//
// 为什么不用 QtGraphicalEffects 做真模糊：Qt 6 已移除该模块，Qt5Compat 还要额外部署；
// 这里把封面以 96px 低分辨率解码再拉伸铺满，等价于一次很重的模糊，但没有任何逐帧
// 卷积开销，且 Qt 5 / Qt 6 行为一致。
//
// 为什么提到外壳层：以前只有播放页自己画背景，横屏出现侧栏导航后，侧栏那 82dp
// 露的是外壳的纯色底 —— 视觉上左侧一条白板、右侧是封面，中间还有一条 1px 分隔线，
// 看着就像「没铺到屏幕左边」。现在背景归外壳统一负责，页面只负责内容。
import QtQuick 2.15

Item {
    id: root

    // 封面地址；空则退回内置默认封面
    property string source: ""
    property bool dark: false
    // 纱的强度：0 = 不过纱（封面最艳），1 = 完全盖住（等于页面底色）
    property real veil: 0.6

    // 低分辨率解码 = 廉价的强模糊。两张图交替做淡入，换封面时不会闪白。
    Image {
        id: imgOld
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize.width: 96
        sourceSize.height: 96
        smooth: true
        mipmap: true
        opacity: 0
    }
    Image {
        id: imgNew
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        sourceSize.width: 96
        sourceSize.height: 96
        smooth: true
        mipmap: true
    }

    property string currentArt: ""

    function setArt(src) {
        var s = (src && src.length > 0) ? src : "qrc:/qml/art/default_cover.jpg"
        if (s === currentArt)
            return
        if (currentArt.length === 0) {
            currentArt = s
            imgNew.source = s
            return
        }
        imgOld.source = imgNew.source
        imgOld.opacity = 1
        imgNew.source = s
        currentArt = s
        fadeAnim.restart()
    }

    NumberAnimation {
        id: fadeAnim
        target: imgOld
        property: "opacity"
        to: 0
        duration: 520
        easing.type: Easing.InOutQuad
    }

    // 纱：自上而下逐渐加浓，保证顶部标题与底部控件都能压住背景。
    // 用整体 opacity 承载 veil，条纹 alpha 只描述相对形状，调参时只动一个数。
    Rectangle {
        anchors.fill: parent
        opacity: Math.max(0, Math.min(1, root.veil))
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.dark ? Qt.rgba(0.06, 0.06, 0.08, 0.80)
                                 : Qt.rgba(1, 1, 1, 0.74)
            }
            GradientStop {
                position: 0.55
                color: root.dark ? Qt.rgba(0.05, 0.05, 0.07, 0.90)
                                 : Qt.rgba(1, 1, 1, 0.88)
            }
            GradientStop {
                position: 1.0
                color: root.dark ? Qt.rgba(0.04, 0.04, 0.06, 1.0)
                                 : Qt.rgba(1, 1, 1, 1.0)
            }
        }
    }

    Component.onCompleted: setArt(root.source)
    onSourceChanged: setArt(root.source)
}
