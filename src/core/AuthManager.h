#pragma once
// 福瑞音乐 - 多平台登录与账号数据同步
#include <QObject>
#include <QTimer>
#include <QVector>

#include <QMap>

#include "HttpClient.h"
#include "Types.h"

namespace Furui {

class AuthManager : public QObject {
    Q_OBJECT
public:
    static AuthManager *instance();

    QStringList methods(const QString &sourceId) const;
    // 账号 API 配置的如实描述，供登录页展示
    QString apiStatus(const QString &sourceId) const;
    QVector<Account> accounts() const;
    Account account(const QString &sourceId) const;
    void refresh();

    void startQrLogin(const QString &sourceId);
    void cancelQrLogin();
    void loginByCookie(const QString &sourceId, const QString &cookie);
    void loginByPassword(const QString &sourceId, const QString &account, const QString &password);
    void sendPhoneCode(const QString &sourceId, const QString &phone);
    void loginByPhone(const QString &sourceId, const QString &phone, const QString &code);
    void logout(const QString &sourceId);

    void syncUserLibrary(const QString &sourceId);
    void syncAll();

    QString qrImagePath() const { return m_qrImagePath; }
    QString qrHint() const { return m_qrHint; }
    QString qrStatus() const { return m_qrStatus; }
    QString qrSource() const { return m_qrSource; }
    // 扫码会话是否仍在轮询中：终态（成功/过期/失败）之后为 false，界面据此决定是否重新出码
    bool qrActive() const { return m_polling; }

signals:
    void loginStateChanged(const QString &sourceId, bool ok, const QString &message);
    // 登录后同步歌单/收藏的结果（成功与否、读到了什么），供登录页逐平台展示
    void librarySynced(const QString &sourceId, bool ok, const QString &message);
    void qrChanged();
    void toast(const QString &message);
    void libraryUpdated();
    void accountsChanged();

private:
    explicit AuthManager(QObject *parent = nullptr);
    void pollQr();
    // 启动轮询并记录本次会话的硬过期时刻，所有出码路径都必须走这里
    void startQrPolling();
    // 网易云 web 明文接口（/api/*）：二维码登录走这条，weapi 二维码路径会被风控回 8821
    void neteaseWebPost(const QString &path, const QByteArray &form, const HttpClient::Callback &cb);
    // 网易云 weapi（密码登录等仍需加密请求）
    void weapiPost(const QString &path, const QJsonObject &payload, const QString &cookie,
                   const HttpClient::Callback &cb);
    // 扫码会话内的 Cookie（NMTID/MUSIC_U 等），与浏览器行为一致：登录态靠 Cookie 串起来
    void absorbNeteaseCookies(const HttpClient::Reply &r);
    QString neteaseCookieHeader() const;
    // QQ 音乐原生扫码（ptlogin 二维码 + musicu QQLogin 换登录态，无需外部服务）
    void startQqQr();
    void pollQqQr();
    void qqFollowGrantChain(const QString &url, int hop);
    void qqExchange(const QString &code);
    // 酷狗原生扫码（login-user.kugou.com 二维码 + 轮询取 token/userid，无需外部服务）
    void startKgQr();
    void pollKgQr();
    void generateQrImage(const QString &content, const QString &sourceId);
    // 会话超时则收尾（停轮询、置「已过期」）并返回 true，调用方直接 return
    bool qrTimedOut();
    void saveSession(const QString &sourceId, const QString &cookie,
                     const QString &uid, const QString &nickname, const QString &avatar,
                     const QString &message);
    void fetchUserInfo(const QString &sourceId, const QString &cookie, const QString &uid);

    QTimer m_poll;
    QString m_qrSource;
    QString m_qrKey;
    QString m_qrImagePath;
    QString m_qrHint;
    QString m_qrStatus;
    bool m_polling = false;
    // 本次扫码会话的硬过期时刻（epoch 毫秒），0 表示没有会话
    qint64 m_qrDeadline = 0;
    // netease / qq / kugou / service：原生实现按平台分支，service 走配置的账号 API 端点
    QString m_qrMode;
    QString m_qqQrsig;
    QString m_qqLoginSig;
    QString m_kgQrcode;
    // 连续收到空回复/无法解析的次数，用于把接口异常显式暴露出来而不是一直"等待扫码"
    int m_qrEmptyStreak = 0;
    // 网易云扫码会话的 Cookie 罐（只保存 name=value）
    QMap<QString, QString> m_neeCookies;
};

} // namespace Furui
