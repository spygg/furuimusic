// 主题 QSS 模板：用命名占位符（{accent} 这种）拼装，改一处令牌全站生效。
// 之所以不用 QString::arg 的位置参数，是因为同一令牌在模板里会重复出现几十次，
// 而 arg() 每次只替换一个出现位置，链式 50 多个 arg 必然错位。
#include "Theme.h"

#include <QPair>
#include <QVector>

namespace Furui {
namespace Theme {

namespace {

QString hex(const QColor &c) { return c.name(QColor::HexRgb); }

QString rgba(const QColor &c, int alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(alpha);
}

struct Sub { const char *key; QString value; };

QString render(const QString &tpl, const QVector<Sub> &subs)
{
    QString out = tpl;
    for (const Sub &s : subs)
        out.replace(QLatin1String(s.key), s.value);
    return out;
}

const char *const kTemplate = R"QSS(
QWidget{
  background:transparent;
  color:{text};
  font-family:"Microsoft YaHei UI","PingFang SC","Segoe UI",sans-serif;
  font-size:{fb}px;
}
QMainWindow,QDialog{background:{window};}
QToolTip{
  background:{elev};color:{text};border:1px solid {border};
  border-radius:{rs}px;padding:{p1}px {p2}px;font-size:{fs}px;
}

/* ---- 按钮：默认=次级胶囊，role 属性决定形态 ---- */
QPushButton{
  background:{panel};color:{text};border:1px solid {border};
  border-radius:{rs}px;padding:{p2}px {p4}px;font-size:{fb}px;
}
QPushButton:hover{background:{hover};border-color:{borderStrong};}
QPushButton:pressed{background:{pressed};}
QPushButton:default,QPushButton[role="accent"]{
  background:{accent};color:{accentText};border:none;font-weight:bold;
  border-radius:{rl}px;padding:{p2}px {p4}px;
}
QPushButton[role="accent"]:hover{background:{accentHover};}
QPushButton[role="accent"]:pressed{background:{accentDown};}
QPushButton:disabled{color:{faint};background:{panelAlt};border-color:{border};}
QPushButton[role="icon"]{
  background:transparent;border:none;border-radius:{ri}px;padding:0;
}
QPushButton[role="icon"]:hover{background:{hover};}
QPushButton[role="icon"]:pressed{background:{pressed};}
QPushButton[role="icon"]:checked{background:{accentBg};}
QPushButton[role="icon"]:disabled{background:transparent;}
QPushButton[role="pill"]{
  background:{panel};border:1px solid {border};border-radius:{rl}px;
  padding:{p2}px {p4}px;color:{sub};
}
QPushButton[role="pill"]:hover{background:{hover};color:{text};}
QPushButton[role="pill"]:checked{
  background:{accent};border-color:{accent};color:{accentText};font-weight:bold;
}

/* ---- 输入 ---- */
QLineEdit,QPlainTextEdit,QTextEdit,QSpinBox,QComboBox{
  background:{panel};color:{text};border:1px solid {border};
  border-radius:{rl}px;padding:{p2}px {p3}px;
  selection-background-color:{accent};selection-color:{accentText};
}
/* 数字框一律关掉原生箭头（NoButtons），音量改用滑条：
   QSS 里一旦接管 up/down-button 就画不出箭头，只会留下两个空白方块。 */
QSpinBox:disabled,QComboBox:disabled{color:{faint};background:{panelAlt};}
QLineEdit:focus,QPlainTextEdit:focus,QTextEdit:focus,QSpinBox:focus,QComboBox:focus{
  border:1px solid {accent};background:{elev};
}
QLineEdit[role="search"]{padding-left:{pSearch}px;background:{panel};}
QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:center right;
  width:{p4}px;border:none;}
QComboBox QAbstractItemView{
  background:{elev};border:1px solid {border};border-radius:{rl}px;
  padding:{p1}px;selection-background-color:{accentBg};selection-color:{text};outline:0;
}

/* ---- 列表 / 表格 ---- */
QListView,QTreeView,QTableView{
  background:{panel};color:{text};border:1px solid {border};
  border-radius:{rl}px;outline:0;alternate-background-color:{panelAlt};
}
QListView::item,QTreeView::item,QTableView::item{
  padding:{piItem}px {p3}px;border:none;border-radius:{rs}px;
}
QListView::item:hover,QTreeView::item:hover,QTableView::item:hover{background:{hover};}
QListView::item:selected,QTreeView::item:selected,QTableView::item:selected{
  background:{accentBg};color:{text};
}
QHeaderView{background:transparent;border:none;}
QHeaderView::section{
  background:transparent;color:{sub};border:none;border-bottom:1px solid {border};
  padding:{p2}px {p3}px;font-size:{fs}px;font-weight:bold;
}
QHeaderView::down-arrow,QHeaderView::up-arrow{width:0;height:0;}

/* ---- 侧边导航 ---- */
#nav{background:transparent;border:none;padding:0;outline:0;}
#nav::item{
  border:none;border-left:3px solid transparent;border-radius:{rl}px;
  padding:{piNav}px {p4}px;margin:{p1}px {p2}px;color:{sub};font-size:{fb}px;
}
#nav::item:hover{background:{hover};color:{text};}
#nav::item:selected{
  background:{accentBg};color:{accent};border-left:3px solid {accent};font-weight:bold;
}

/* ---- 播放条 / 卡片 ---- */
#playBar{background:{panel};border:1px solid {border};border-radius:{rl}px;}
#card{background:{panel};border:1px solid {border};border-radius:{rl}px;}
#coverArt{border:none;}
/* border:none 时 Qt 不画圆角，同色描边才能拿到真正的圆形播放键 */
#playBtn{background:{accent};border:1px solid {accent};border-radius:{pbr}px;color:{accentText};}
#playBtn:hover{background:{accentHover};border-color:{accentHover};}
#playBtn:pressed{background:{accentDown};border-color:{accentDown};}
#playBtn:disabled{background:{track};border-color:{track};}
#tabBar{background:{panel};border:1px solid {border};border-radius:{rl}px;outline:0;padding:{p1}px;}
#tabBar::item{
  border:none;border-top:3px solid transparent;border-radius:{rs}px;
  padding:{piTab}px {p1}px;margin:0 {p1}px;color:{sub};
}
#tabBar::item:hover{background:{hover};color:{text};}
#tabBar::item:selected{
  background:{accentBg};color:{accent};border-top:3px solid {accent};font-weight:bold;
}
#drawer{background:{elev};border:1px solid {border};border-radius:{rl}px;}
#drawerTitle{font-size:{ft}px;font-weight:bold;color:{text};}
#chip{
  background:{accentBg};border:none;border-radius:{rl}px;color:{accent};
  padding:0 {p2}px;font-size:{fs}px;font-weight:bold;
}
#titleLabel{font-size:{ft}px;font-weight:bold;color:{text};}
#subLabel{font-size:{fs}px;color:{sub};}
#hintLabel{font-size:{fs}px;color:{faint};}
#emptyLabel{font-size:{fxl}px;color:{faint};}
#sectionTitle{font-size:{ft}px;font-weight:bold;color:{text};}

/* ---- 其它容器 ---- */
QTabWidget::pane{border:1px solid {border};border-radius:{rl}px;top:-{p2}px;background:{panel};}
QTabBar::tab{
  background:transparent;color:{sub};padding:{p2}px {p4}px;border:none;
  border-bottom:2px solid transparent;
}
QTabBar::tab:hover{color:{text};}
QTabBar::tab:selected{color:{accent};border-bottom:2px solid {accent};font-weight:bold;}
/* documentMode 会在页签之间画一根越过基线的分隔线，看着像残留的滚动按钮 */
QTabBar::separator:horizontal{width:0px;height:0px;}
QGroupBox{
  border:1px solid {border};border-radius:{rl}px;margin-top:{p4}px;
  padding-top:{p2}px;background:{panel};font-weight:bold;
}
QGroupBox::title{
  subcontrol-origin:margin;left:{p3}px;padding:0 {p2}px;
  color:{sub};background:{panel};
}
/* 登录方式卡片没有标题，去掉为标题留的上边距，内边距也收紧一点 */
QGroupBox#card{margin:0;padding:{p3}px;border:1px solid {border};font-weight:normal;}
QCheckBox,QRadioButton{spacing:{p2}px;color:{text};}
QCheckBox::indicator,QRadioButton::indicator{width:{ind}px;height:{ind}px;}
QCheckBox::indicator{
  border:1px solid {borderStrong};border-radius:{rck}px;background:{panel};
}
QCheckBox::indicator:checked{background:{accent};border-color:{accent};}
QRadioButton::indicator{
  border:1px solid {borderStrong};border-radius:{ind}px;background:{panel};
}
QRadioButton::indicator:checked{background:{accent};border-color:{accent};}

/* ---- 滚动条：细、悬浮加深、无箭头 ---- */
QScrollBar:vertical{background:transparent;width:{sb}px;margin:{p1}px;}
QScrollBar::handle:vertical{background:{scroll};border-radius:{scr}px;min-height:{ind}px;}
QScrollBar::handle:vertical:hover{background:{scrollHover};}
QScrollBar:horizontal{background:transparent;height:{sb}px;margin:{p1}px;}
QScrollBar::handle:horizontal{background:{scroll};border-radius:{scr}px;min-width:{ind}px;}
QScrollBar::handle:horizontal:hover{background:{scrollHover};}
QScrollBar::add-line,QScrollBar::sub-line,QScrollBar::add-page,QScrollBar::sub-page{
  background:transparent;border:none;width:0;height:0;
}
/* 设置页的滚动容器保持透明，让 QTabWidget::pane 的底色透出来，不出现两层边框 */
QScrollArea{background:transparent;border:none;}
QScrollArea > QWidget > QWidget{background:transparent;}

/* ---- 进度 / 音量（播放条里的进度条是自定义绘制，这里只兜底原生 QSlider） ---- */
QSlider::groove:horizontal{height:{grv}px;border-radius:{scr}px;background:{track};}
QSlider::sub-page:horizontal{height:{grv}px;border-radius:{scr}px;background:{accent};}
QSlider::handle:horizontal{
  width:{knob}px;height:{knob}px;margin:-{kmar}px 0;border-radius:{knobR}px;
  background:{accentText};border:{kbd}px solid {accent};
}
QSlider::handle:horizontal:hover{background:{accent};}
QSlider::groove:vertical{width:{grv}px;border-radius:{scr}px;background:{track};}
QSlider::handle:vertical{
  width:{knob}px;height:{knob}px;margin:0 -{kmar}px;border-radius:{knobR}px;
  background:{accentText};border:{kbd}px solid {accent};
}
QProgressBar{background:{track};border:none;border-radius:{scr}px;
  color:{sub};font-size:{fs}px;}
QProgressBar::chunk{background:{accent};border-radius:{scr}px;}

QMenu{background:{elev};color:{text};border:1px solid {border};
  border-radius:{rl}px;padding:{p1}px;}
QMenu::item{padding:{p2}px {p5}px;border-radius:{rs}px;}
QMenu::item:selected{background:{accentBg};color:{accent};}
QMenu::item:disabled{color:{faint};}
QMenu::separator{height:1px;background:{border};margin:{p1}px {p2}px;}

QStatusBar{background:{panel};border-top:1px solid {border};color:{sub};font-size:{fs}px;}
QStatusBar::item{border:none;}

QDockWidget{color:{sub};font-size:{fs}px;font-weight:bold;}
QDockWidget::title{background:transparent;padding:{p2}px {p3}px;}
QDockWidget::close-button,QDockWidget::float-button{border:none;background:transparent;}
)QSS";

} // namespace

QString styleSheet(const Tokens &t, qreal fontScale)
{
    const auto px = [fontScale](int v) { return qMax(1, qRound(v * fontScale)); };
    const QString window = QStringLiteral("qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                                         "stop:0 %1, stop:1 %2)")
                               .arg(hex(t.windowTop), hex(t.windowBottom));

    const QVector<Sub> subs = {
        {"{window}",        window},
        {"{panel}",         hex(t.panel)},
        {"{panelAlt}",      hex(t.panelAlt)},
        {"{elev}",          hex(t.panel.lighter(t.key == QLatin1String("dark") ? 118 : 100))},
        {"{text}",          hex(t.text)},
        {"{sub}",           hex(t.subtext)},
        {"{faint}",         hex(t.faint)},
        {"{accent}",        hex(t.accent)},
        {"{accentText}",    hex(t.accentText)},
        {"{accentHover}",   hex(t.accent.lighter(112))},
        {"{accentDown}",    hex(t.accent.darker(120))},
        {"{accentBg}",      rgba(t.accent, t.key == QLatin1String("dark") ? 52 : 26)},
        {"{border}",        hex(t.border)},
        {"{borderStrong}",  hex(t.border.darker(112))},
        {"{hover}",         hex(t.hover)},
        {"{pressed}",       hex(t.pressed)},
        {"{track}",         hex(t.track)},
        {"{scroll}",        rgba(t.faint, 110)},
        {"{scrollHover}",   rgba(t.faint, 190)},
        {"{r}",             QString::number(px(t.radius))},
        {"{rs}",            QString::number(px(t.radius - 5))},
        {"{rl}",            QString::number(px(t.radius + 5))},
        {"{ri}",            QString::number(px(11))},
        {"{fb}",            QString::number(px(t.fontBase))},
        {"{fs}",            QString::number(px(t.fontSmall))},
        {"{ft}",            QString::number(px(t.fontTitle))},
        {"{fxl}",           QString::number(px(t.fontTitle + 4))},
        {"{p1}",            QString::number(px(3))},
        {"{p2}",            QString::number(px(6))},
        {"{p3}",            QString::number(px(10))},
        {"{p4}",            QString::number(px(14))},
        {"{p5}",            QString::number(px(26))},
        {"{pSearch}",       QString::number(px(32))},
        {"{piItem}",        QString::number(px(8))},
        {"{piTab}",         QString::number(px(6))},
        {"{pbr}",           QString::number(px(22))},
        {"{piNav}",         QString::number(px(11))},
        {"{ind}",           QString::number(px(16))},
        {"{rck}",           QString::number(px(4))},
        {"{sb}",            QString::number(px(10))},
        {"{scb}",           QString::number(px(10))},
        {"{scr}",           QString::number(px(5))},
        {"{grv}",           QString::number(px(5))},
        {"{knob}",          QString::number(px(14))},
        {"{knobR}",         QString::number(px(7))},
        {"{kmar}",          QString::number(px(5))},
        {"{kbd}",           QString::number(px(2))},
    };
    return render(QString::fromUtf8(kTemplate), subs);
}

} // namespace Theme
} // namespace Furui
