#include "PlayerWidgets.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QPushButton>
#include <QLabel>
#include <QEvent>
#include <QSlider>
#include <QHBoxLayout>
#include <QStyle>
#include <QStyleOptionButton>
#include <QLinearGradient>

namespace Furui {

namespace {

QString formatTime(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 total = ms / 1000;
    return QStringLiteral("%1:%2").arg(int(total / 60)).arg(int(total % 60), 2, 10, QLatin1Char('0'));
}

qreal eased(qreal x) { return x * x * (3.0 - 2.0 * x); }

} // namespace

// ---------------- CoverArt ----------------

CoverArt::CoverArt(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void CoverArt::setPixmap(const QPixmap &pm)
{
    if (pm.isNull() && m_pixmap.isNull()) return;
    m_pixmap = pm;
    update();
}

void CoverArt::setCornerRadius(int r)
{
    if (m_radius == r) return;
    m_radius = r;
    update();
}

void CoverArt::setSpin(qreal v)
{
    m_spin = v;
    update();
}

void CoverArt::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect();
    const qreal dpr = r.width() > 0 ? devicePixelRatioF() : 1.0;

    QPainterPath clip;
    clip.addRoundedRect(r, m_radius, m_radius);
    p.setClipPath(clip);

    const Tokens &t = Theme::current();
    if (!m_pixmap.isNull()) {
        // 居中裁成正方形：先按放大后的目标尺寸铺满，再裁掉溢出的一边
        const QSize want(int(r.width() * dpr), int(r.height() * dpr));
        QPixmap art = m_pixmap.scaled(want, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
        const int dx = (art.width() - want.width()) / 2;
        const int dy = (art.height() - want.height()) / 2;
        art = art.copy(dx, dy, want.width(), want.height());
        art.setDevicePixelRatio(dpr);
        p.drawPixmap(0, 0, art);
    } else {
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, t.mask);
        g.setColorAt(1.0, t.panelAlt);
        p.fillPath(clip, g);
        const int ic = qMax(14, int(r.width() * 0.42));
        p.drawPixmap(QPointF((r.width() - ic) / 2.0, (r.height() - ic) / 2.0),
                     Icons::pixmap(Icons::Cover, t.faint, ic, dpr));
    }

    // 1px 内描边，浅色主题下卡片不会和背景糊在一起
    p.setClipping(false);
    p.setPen(QPen(t.key == QLatin1String("dark") ? QColor(255, 255, 255, 22)
                                                 : QColor(0, 0, 0, 14)));
    p.setBrush(Qt::NoBrush);
    p.drawPath(clip);
}

// ---------------- SeekSlider ----------------

SeekSlider::SeekSlider(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(18);
    m_anim = new QPropertyAnimation(this, "hover", this);
    m_anim->setDuration(140);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
}

void SeekSlider::setRange(qint64 positionMs, qint64 durationMs)
{
    m_position = qMax<qint64>(0, positionMs);
    m_duration = qMax<qint64>(0, durationMs);
    if (!m_dragging) update();
}

void SeekSlider::setBuffered(qreal ratio)
{
    m_buffered = qBound(0.0, ratio, 1.0);
    update();
}

void SeekSlider::setEditable(bool on)
{
    m_editable = on;
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void SeekSlider::setHover(qreal v)
{
    if (qFuzzyCompare(m_hover, v)) return;
    m_hover = v;
    update();
}

qreal SeekSlider::ratioAt(const QPoint &pos) const
{
    const qreal pad = 8.0;
    const qreal usable = qMax<qreal>(1.0, width() - pad * 2.0);
    return qBound(0.0, (pos.x() - pad) / usable, 1.0);
}

void SeekSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Tokens &t = Theme::current();
    const qreal h = eased(m_hover);

    qreal ratio = 0.0;
    if (m_dragRatio >= 0.0) ratio = m_dragRatio;
    else if (m_duration > 0) ratio = qBound(0.0, double(m_position) / double(m_duration), 1.0);

    const qreal midY = height() / 2.0;
    const qreal trackH = 4.0 + 2.0 * h;
    const qreal pad = 8.0;
    const QRectF track(pad, midY - trackH / 2.0, qMax<qreal>(1.0, width() - pad * 2.0), trackH);

    p.setPen(Qt::NoPen);
    p.setBrush(t.track);
    p.drawRoundedRect(track, trackH / 2.0, trackH / 2.0);

    if (m_buffered > ratio) {
        p.setBrush(QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 70));
        p.drawRoundedRect(QRectF(track.left(), track.top(),
                                 track.width() * m_buffered, track.height()),
                          trackH / 2.0, trackH / 2.0);
    }

    if (ratio > 0.0) {
        QLinearGradient g(track.topLeft(), track.bottomRight());
        g.setColorAt(0.0, t.accent);
        g.setColorAt(1.0, t.accentSoft);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(track.left(), track.top(),
                                 track.width() * ratio, track.height()),
                          trackH / 2.0, trackH / 2.0);
    }

    // 滑块：静止时是个小点，hover/拖动时长大并带光晕
    const qreal kx = track.left() + track.width() * ratio;
    const qreal kr = 3.0 + 4.0 * h;
    if (h > 0.01) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(t.accent.red(), t.accent.green(), t.accent.blue(), int(60 * h)));
        p.drawEllipse(QPointF(kx, midY), kr + 4.0, kr + 4.0);
    }
    p.setBrush(t.accentText);
    p.setPen(QPen(t.accent, 2.0));
    p.drawEllipse(QPointF(kx, midY), kr, kr);
}

void SeekSlider::mousePressEvent(QMouseEvent *event)
{
    if (!m_editable || m_duration <= 0 || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragRatio = ratioAt(event->pos());
    m_anim->stop();
    m_anim->setStartValue(m_hover);
    m_anim->setEndValue(1.0);
    m_anim->start();
    emit hoverPreview(formatTime(qint64(m_dragRatio * m_duration)));
    update();
}

void SeekSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_editable) return;
    if (!m_dragging) {
        if (m_hover < 0.99) {
            m_anim->stop();
            m_anim->setStartValue(m_hover);
            m_anim->setEndValue(1.0);
            m_anim->start();
        }
        emit hoverPreview(formatTime(qint64(ratioAt(event->pos()) * m_duration)));
        return;
    }
    m_dragRatio = ratioAt(event->pos());
    emit hoverPreview(formatTime(qint64(m_dragRatio * m_duration)));
    update();
}

void SeekSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    const qreal r = m_dragRatio;
    m_dragRatio = -1.0;
    if (event->button() == Qt::LeftButton && r >= 0.0 && m_duration > 0)
        emit seekRequested(qint64(r * m_duration));
    update();
}

void SeekSlider::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_dragging) return;
    m_anim->stop();
    m_anim->setStartValue(m_hover);
    m_anim->setEndValue(0.0);
    m_anim->start();
    emit hoverPreview(QString());
}

// ---------------- EmptyOverlay ----------------

EmptyOverlay::EmptyOverlay(QWidget *view, const QString &text)
    : QObject(view), m_view(view)
{
    m_label = new QLabel(text, view);
    m_label->setObjectName(QStringLiteral("emptyLabel"));
    m_label->setAlignment(Qt::AlignCenter);
    m_label->hide();
    view->installEventFilter(this);
}

void EmptyOverlay::setText(const QString &text)
{
    m_label->setText(text);
    reposition();
}

void EmptyOverlay::showFor(int rowCount)
{
    m_label->setVisible(rowCount == 0);
    if (rowCount == 0) reposition();
}

void EmptyOverlay::reposition()
{
    m_label->move((m_view->width() - m_label->sizeHint().width()) / 2,
                  (m_view->height() - m_label->sizeHint().height()) / 2);
    m_label->raise();
}

bool EmptyOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && event->type() == QEvent::Resize) reposition();
    return QObject::eventFilter(watched, event);
}

// ---------------- AccentCheckBox ----------------

AccentCheckBox::AccentCheckBox(const QString &text, QWidget *parent)
    : QCheckBox(text, parent)
{
    setCursor(Qt::PointingHandCursor);
}

void AccentCheckBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const Tokens &t = Theme::current();
    const qreal k = Theme::pxScale();

    // 位置仍向 style 要，这样 QSS 里的 indicator 尺寸与间距改了，文字也不会错位
    QStyleOptionButton opt;
    initStyleOption(&opt);
    const QRect box = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, this);
    const QRect content = style()->subElementRect(QStyle::SE_CheckBoxContents, &opt, this);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool on = isChecked();
    const QColor edge = !isEnabled() ? t.faint
                        : (on ? t.accent : (underMouse() ? t.subtext : t.border));
    p.setPen(QPen(edge, qMax<qreal>(1.0, k)));
    p.setBrush(!isEnabled() ? t.panelAlt : (on ? t.accent : t.panel));
    const qreal radius = qMax<qreal>(1.0, 4.0 * k);
    p.drawRoundedRect(box, radius, radius);

    if (on) {
        const int tick = qMax(8, qRound(box.height() * 0.74));
        const QPixmap pm = Icons::pixmap(Icons::Check,
                                         isEnabled() ? t.accentText : t.faint,
                                         tick, devicePixelRatioF());
        p.drawPixmap(box.center() - QPoint(tick / 2, tick / 2), pm);
    }

    if (hasFocus()) {
        const int ring = qRound(3 * k);
        p.setPen(QPen(t.accentSoft, qMax<qreal>(1.0, k)));
        p.drawRoundedRect(box.adjusted(-ring, -ring, ring, ring),
                          radius + ring, radius + ring);
    }

    p.setPen(isEnabled() ? t.text : t.faint);
    p.drawText(content, Qt::AlignVCenter | Qt::AlignLeft, text());
}

// ---------------- ValueSlider ----------------

ValueSlider::ValueSlider(int lo, int hi, QWidget *parent)
    : QWidget(parent), m_slider(nullptr), m_label(nullptr)
{
    const qreal k = Theme::pxScale();
    QHBoxLayout *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(qRound(10 * k));

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(lo, hi);
    // 手柄直径 = knob + 2*kmar，行高再小也不能裁掉圆头，否则高 DPI 下会看成两段弧
    m_slider->setMinimumHeight(qRound(20 * k));
    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_label->setMinimumWidth(qRound(46 * k));

    lay->addWidget(m_slider, 1);
    lay->addWidget(m_label);

    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        m_label->setText(QString::number(v) + m_suffix);
        emit valueChanged(v);
    });
    m_label->setText(QString::number(lo) + m_suffix);
}

int ValueSlider::value() const { return m_slider->value(); }

void ValueSlider::setValue(int v) { m_slider->setValue(v); }

void ValueSlider::setSuffix(const QString &suffix)
{
    m_suffix = suffix;
    m_label->setText(QString::number(m_slider->value()) + m_suffix);
}

// ---------------- helpers ----------------

QPushButton *makeIconButton(Icons::Kind kind, const QString &tip, int boxSize,
                           const QColor &color, QWidget *parent, int iconSize)
{
    QPushButton *b = new QPushButton(parent);
    b->setProperty("role", "icon");
    b->setProperty("iconKind", int(kind));
    b->setProperty("iconPx", iconSize > 0 ? iconSize : int(boxSize * 0.56));
    b->setToolTip(tip);
    b->setFixedSize(boxSize, boxSize);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    b->setIcon(Icons::icon(kind, color, iconSize > 0 ? iconSize : int(boxSize * 0.56)));
    if (iconSize > 0) b->setIconSize(QSize(iconSize, iconSize));
    return b;
}

} // namespace Furui
