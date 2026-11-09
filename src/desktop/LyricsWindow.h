#pragma once

#include <QWidget>
#include <QPoint>
#include "../core/AppController.h"

class QTextEdit;
class QMouseEvent;

namespace Furui {
class EmptyOverlay;
}

// 无边框、置顶、可拖动的歌词窗口；双击隐藏，跟随 lyricsChanged 刷新。
class LyricsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit LyricsWindow(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void refreshLyrics();

    Furui::AppController *app;
    QTextEdit *m_edit;
    Furui::EmptyOverlay *m_empty;
    QPoint m_dragOffset;
    bool m_dragging;
};
