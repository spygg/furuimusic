#include "LyricsWindow.h"

#include <QTextEdit>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QCursor>
#include <QColor>
#include <QFont>
#include "Theme.h"
#include "PlayerWidgets.h"

LyricsWindow::LyricsWindow(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint),
      app(Furui::AppController::instance()),
      m_edit(nullptr),
      m_empty(nullptr),
      m_dragging(false)
{
    setWindowTitle(QStringLiteral("歌词"));
    resize(360, 460);

    QVBoxLayout *lay = new QVBoxLayout(this);
    m_edit = new QTextEdit(this);
    m_edit->setReadOnly(true);
    m_edit->setAlignment(Qt::AlignHCenter);
    lay->addWidget(m_edit, 1);

    QLabel *tip = new QLabel(QStringLiteral("拖动可移动窗口 · 双击关闭"), this);
    tip->setObjectName(QStringLiteral("hintLabel"));
    tip->setAlignment(Qt::AlignHCenter);
    lay->addWidget(tip);

    connect(app, &Furui::AppController::lyricsChanged, this, [this]() {
        if (!app || !m_edit) return;
        refreshLyrics();
    });
    refreshLyrics();
}

void LyricsWindow::refreshLyrics()
{
    if (!app || !m_edit) return;

    const Furui::Tokens &t = Furui::Theme::current();
    const QString text = app->lyricsText();
    if (!m_empty) m_empty = new Furui::EmptyOverlay(m_edit, QStringLiteral("暂无歌词"));

    if (text.isEmpty()) {
        // Qt 富文本不认 padding-top，空态改用浮层，窗口多高都落在正中
        m_edit->clear();
        m_edit->setExtraSelections(QList<QTextEdit::ExtraSelection>());
        m_empty->showFor(0);
        return;
    }
    m_empty->showFor(1);
    m_edit->setPlainText(text);

    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument *doc = m_edit->document();
    const int index = app->currentLyricIndex();

    // 其余行统一压暗，当前行放大 + 主色，视线自然落在唱到的那句
    for (int i = 0; i < doc->blockCount(); ++i) {
        QTextCursor cursor(doc->findBlockByNumber(i));
        cursor.select(QTextCursor::BlockUnderCursor);
        QTextEdit::ExtraSelection sel;
        sel.cursor = cursor;
        if (i == index) {
            QFont f = sel.format.font();
            f.setBold(true);
            f.setPointSizeF(f.pointSizeF() + 2.5);
            sel.format.setFont(f);
            sel.format.setForeground(t.text);
            sel.format.setBackground(QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 30));
        } else {
            sel.format.setForeground(t.faint);
        }
        selections.append(sel);
    }
    m_edit->setExtraSelections(selections);

    if (index >= 0 && index < doc->blockCount()) {
        // 仅移动光标以滚动到当前行，不产生选中态，避免与高亮叠加。
        m_edit->setTextCursor(QTextCursor(doc->findBlockByNumber(index)));
    }
}

void LyricsWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        // 用 QCursor::pos() 取全局坐标，兼容 Qt 5.15 与 Qt 6。
        m_dragOffset = QCursor::pos() - frameGeometry().topLeft();
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void LyricsWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(QCursor::pos() - m_dragOffset);
        event->accept();
    } else {
        QWidget::mouseMoveEvent(event);
    }
}

void LyricsWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    hide();
}
