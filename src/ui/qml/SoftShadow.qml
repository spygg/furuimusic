import QtQuick 2.15

// 三层伪投影：与 PlayerPage 封面投影同款手法，抽出来给所有卡片共用。
// 用几个错位圆角矩形模拟柔和落影——无 GraphicalEffects 依赖，Qt5/Qt6
// 表现完全一致，开销是普通矩形而非实时模糊，网格里整屏铺开也扛得住。
// 用法：与 target 放在同一父级下即可，z 已压到背面：
//   Rectangle { id: card; ... }
//   SoftShadow { target: card }
// 投影带向下偏移（下缘出影多、上缘收进），并跟随 target 的 scale/visible。
Repeater {
    id: shadowRoot

    property Item target: null

    model: 3

    delegate: Rectangle {
        required property int index

        readonly property int m: index + 1

        x: shadowRoot.target ? shadowRoot.target.x - m * 4 : 0
        y: shadowRoot.target ? shadowRoot.target.y + (10 - m * 2) : 0
        width: shadowRoot.target ? shadowRoot.target.width + m * 8 : 0
        height: shadowRoot.target ? shadowRoot.target.height + m * 4 - 6 : 0
        radius: shadowRoot.target && shadowRoot.target.radius > 0
                ? shadowRoot.target.radius + m * 4 : 0
        color: Qt.rgba(theme.shadowTint.r, theme.shadowTint.g, theme.shadowTint.b,
                       theme.dark ? 0.12 : 0.05)
        scale: shadowRoot.target ? shadowRoot.target.scale : 1.0
        visible: shadowRoot.target ? shadowRoot.target.visible : false
        z: -1 - index
    }
}
