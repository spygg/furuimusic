#pragma once

#include <QDialog>
#include "../core/AppController.h"

class QTabWidget;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QLineEdit;
class QSpinBox;
class QLabel;

namespace Furui {
class ValueSlider;
} // namespace Furui

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    static void applyTheme(const QString &key);

private:
    QWidget *createPlaybackTab();
    QWidget *createSourcesTab();
    QWidget *createNetworkTab();
    QWidget *createAppearanceTab();
    QWidget *createAboutTab();
    void refreshSources();
    void syncFromSettings();
    void connectSignals();

    Furui::AppController *app;

    QComboBox *m_quality;
    QCheckBox *m_vip;
    QCheckBox *m_autoToggle;
    QCheckBox *m_openApi;
    QSpinBox *m_openApiPort;
    QLabel *m_openApiAddress;
    QComboBox *m_searchType;
    Furui::ValueSlider *m_defaultVolume;
    QComboBox *m_playMode;

    QTableWidget *m_sourcesTable;
    QLineEdit *m_httpName;
    QLineEdit *m_httpUrl;
    QLabel *m_localDir;

    QLineEdit *m_downloadDir;
    QLineEdit *m_proxy;
    QSpinBox *m_timeout;
    QLineEdit *m_accountApiBase;

    QComboBox *m_theme;
};
