#pragma once

#include <QDialog>
#include <QVector>
#include <QStringList>
#include "../core/AppController.h"

class QTabWidget;
class QLabel;
class QPushButton;

class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(QWidget *parent = nullptr);

private:
    struct TabUi {
        QString sourceId;
        QLabel *status;
        QLabel *apiStatus;
        QLabel *qrImage;
        QLabel *qrState;
        QPushButton *qrGet;      // 该页有扫码方式时非空
    };

    QWidget *createTab(const QString &sourceId, const QStringList &methods);
    void refreshStatus();
    void refreshApiStatus();
    void refreshQr();
    void connectSignals();
    void showQrTab(int tabIndex);

    Furui::AppController *app;
    QTabWidget *m_tabs;
    QLabel *m_hint;
    QVector<TabUi> m_tabUis;
};
