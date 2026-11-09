#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>
#include <QCheckBox>
#include <QPropertyAnimation>

#include "Theme.h"

class QPushButton;
class QLabel;
class QSlider;
class QEvent;

namespace Furui {

// 圆角封面：无图时用主题渐变 + 音符占位，避免界面里出现写着“封面”两个字的灰块。
class CoverArt : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal spin READ spin WRITE setSpin)
public:
    explicit CoverArt(QWidget *parent = nullptr);

    void setPixmap(const QPixmap &pm);
    void setCornerRadius(int r);
    qreal spin() const { return m_spin; }
    void setSpin(qreal v);

    QSize sizeHint() const override { return QSize(56, 56); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap m_pixmap;
    int m_radius = 12;
    qreal m_spin = 0.0;
};

// 进度条：自定义绘制，hover 时轨道变粗并浮出圆形滑块，拖动即时回显时间点。
class SeekSlider : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal hover READ hover WRITE setHover)
public:
    explicit SeekSlider(QWidget *parent = nullptr);

    void setRange(qint64 positionMs, qint64 durationMs);
    void setBuffered(qreal ratio);
    qint64 duration() const { return m_duration; }
    void setEditable(bool on);

    qreal hover() const { return m_hover; }
    void setHover(qreal v);

    QSize sizeHint() const override { return QSize(200, 18); }

signals:
    void seekRequested(qint64 positionMs);
    void hoverPreview(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    qreal ratioAt(const QPoint &pos) const;

    qint64 m_position = 0;
    qint64 m_duration = 0;
    qreal m_buffered = 0.0;
    qreal m_hover = 0.0;
    qreal m_dragRatio = -1.0;
    bool m_editable = true;
    bool m_dragging = false;
    QPropertyAnimation *m_anim = nullptr;
};

// 空态占位：列表为空时在视图中央显示提示，避免大片空白看着像没加载完。
class EmptyOverlay : public QObject
{
    Q_OBJECT
public:
    explicit EmptyOverlay(QWidget *view, const QString &text);
    void setText(const QString &text);
    void showFor(int rowCount);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();

    QWidget *m_view;
    QLabel *m_label;
};

// 复选框：QSS 只能给指示器填色，勾是 Qt 原生画的、一旦样式接管就消失，
// 所以自己画圆角方框 + 矢量勾，选中态在深浅两套主题下都能看清。
class AccentCheckBox : public QCheckBox
{
    Q_OBJECT
public:
    explicit AccentCheckBox(const QString &text, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};

// 音量/百分比行：滑条 + 右侧数字。设置页里的数字框带箭头会画成两个空白方块，
// 而播放器场景本身也用滑条，统一成滑条既好看又和主界面一致。
class ValueSlider : public QWidget
{
    Q_OBJECT
public:
    explicit ValueSlider(int lo, int hi, QWidget *parent = nullptr);

    int value() const;
    void setValue(int v);
    void setSuffix(const QString &suffix);

signals:
    void valueChanged(int v);

private:
    QSlider *m_slider;
    QLabel *m_label;
    QString m_suffix;
};

// 图标按钮：统一 role="icon" 让 QSS 接管形态，图标由 Icons 矢量生成。
QPushButton *makeIconButton(Icons::Kind kind, const QString &tip, int boxSize,
                           const QColor &color, QWidget *parent, int iconSize = -1);

} // namespace Furui
