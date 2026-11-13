#pragma once
// 福瑞音乐 - 多平台登录与账号数据同步
#include <QObject>
#include <QTimer>
#include <QVector>

#include <QMap>
#include <functional>

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
    // captcha：酷我等需要图形验证码的平台才用得上，其它平台传空即可
    void loginByPassword(const QString &sourceId, const QString &account, const QString &password,
                         const QString &captcha = QString());
    // 浏览器登录态导入：网易云的密码/扫码被 8821 行为验证码风控挡住时，
    // 用户在浏览器里完成验证后把 Cookie（含 MUSIC_U）粘回来即可登录。
    void loginByCookie(const QString &sourceId, const QString &cookie);
    void sendPhoneCode(const QString &sourceId, const QString &phone, const QString &captcha = QString());
    void loginByPhone(const QString &sourceId, const QString &phone, const QString &code);
    // 酷我：图形验证码（wapi.kuwo.cn 门户同款 /api/common/captcha/getcode）
    void refreshKuwoCaptcha();
    // 酷我：账号密码登录（/api/www/login/loginByKw，账号密码为明文 + 图形验证码）
    void loginKuwoPassword(const QString &account, const QString &password, const QString &captcha);
    // 酷我：手机号短信登录（/api/sms/mobileLoginCode 发码 → /api/www/login/loginByMobile 登录）
    void sendKuwoSms(const QString &phone, const QString &captcha);
    void loginKuwoPhone(const QString &phone, const QString &smsCode);
    // 咪咕：passport.migu.cn 网页端同款密码登录（RSA PKCS#1 v1.5），无需浏览器或外部服务
    void loginMiguPassword(const QString &sourceId, const QString &account, const QString &password);
    // 酷狗：/v9/login_by_pwd（AES-256-CBC + RSA_NO_PADDING + Android 签名），
    // 与扫码并列，不依赖账号 API 服务；命中风控时如实提示用户手动处理
    void loginKugouPassword(const QString &account, const QString &password);
    // 咪咕：门户同款扫码登录（qrcWeb 出码 + qrc_sessionid 轮询），与密码登录并列
    void startMiguQr();
    void pollMiguQr();
    void logout(const QString &sourceId);

    void syncUserLibrary(const QString &sourceId);
    void syncAll();

    QString qrImagePath() const { return m_qrImagePath; }
    QString qrHint() const { return m_qrHint; }
    QString qrStatus() const { return m_qrStatus; }
    QString qrSource() const { return m_qrSource; }
    // 图形验证码图片路径（仅酷我：账号密码与短信登录共用一张码，点图即换）
    QString captchaImagePath() const { return m_captchaImagePath; }
    // 扫码会话是否仍在轮询中：终态（成功/过期/失败）之后为 false，界面据此决定是否重新出码
    bool qrActive() const { return m_polling; }

signals:
    void loginStateChanged(const QString &sourceId, bool ok, const QString &message);
    // 登录后同步歌单/收藏的结果（成功与否、读到了什么），供登录页逐平台展示
    void librarySynced(const QString &sourceId, bool ok, const QString &message);
    void qrChanged();
    void captchaChanged();
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
    // 补全网易云客户端要求的设备指纹 Cookie（deviceId/os/appver/channel/WNMCID…），
    // 缺了它们服务端会把请求当异常环境，扫码授权阶段直接回 8821
    QString neteaseDeviceCookie();
    // 匿名设备令牌 MUSIC_A：POST /weapi/register/anonimous，成功后缓存到设置里
    void neteaseBootstrap(std::function<void(bool)> done);
    // 网易云 eapi（interface.music.163.com 的加密通道）：官方客户端扫码走这条
    void neteaseEapiPost(const QString &apiPath, const QJsonObject &payload,
                         const HttpClient::Callback &cb);
    // QQ 音乐原生扫码（ptlogin 二维码 + musicu QQLogin 换登录态，无需外部服务）
    void startQqQr();
    void pollQqQr();
    void qqFollowGrantChain(const QString &url, int hop);
    void qqExchange(const QString &code);
    // 酷狗原生扫码（login-user.kugou.com 二维码 + 轮询取 token/userid，无需外部服务）
    void startKgQr();
    void pollKgQr();
    // 酷我：第三方里只有微信能纯 HTTP 走完（开放平台 qrconnect 出码 + 长轮询取 code +
    // i.kuwo.cn 回调换 Cookie）。QQ/微博要内嵌浏览器回调，本工程没链 WebEngine，故未内置。
    void startKuwoWxQr();
    void pollKuwoWx();
    void kuwoWxExchange(const QString &code);
    // 酷我登录结果的统一收口：解析 data.cookies → 落库 → 补资料
    void finishKuwoLogin(const HttpClient::Reply &r, const QString &okMessage);
    // 汽水：登录闭环在汽水服务端（passport/web/* 出码 + check_qrconnect 轮询 + /luna/pc/me 换
    // session），客户端只跑出码 → 轮询 → 拿 sessionid → 换账号；未配置自建账号 API 服务时
    // 直接走官方 endpoint，无需任何外部服务即可「直接扫码」登录。
    // 与 src/tools/qishui_login_flow_test.py 的探针链路保持一致。
    void startQishuiQr();
    void pollQishuiQr();
    void finishQishuiLogin(const QString &sessionId);
    // 汽水：把本轮扫码会话的 Cookie 映射拼成 "name=value; ..." 头（passport_csrf_token / sessionid）
    QString qishuiCookieHeader() const;
    // 汽水扫码轮询统一的应答处理：统一解析状态文案 + 拿 sessionid（Set-Cookie / body 两条链路共用）
    void handleQishuiPollReply(const HttpClient::Reply &r, const QString &sessionId, const QString &st);
    void generateQrImage(const QString &content, const QString &sourceId);
    // data:image/png;base64,... 形态的二维码（汽水 passport 出码返回）→ 直接落盘
    bool saveQrDataUrl(const QString &dataUrl, const QString &sourceId);
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
    // 酷我微信扫码会话：uuid 来自 qrconnect 页面里的 /connect/qrcode/<uuid>，
    // m_kwPollBusy 防止长轮询（最长 30s）和 2s 定时器叠在一起发请求
    QString m_kwUuid;
    bool m_kwPollBusy = false;
    // 酷我图形验证码：token 与图片一一对应，验证码文本要留到短信登录那一步继续用
    QString m_captchaToken;
    QString m_captchaText;
    QString m_captchaImagePath;
    int m_captchaSeq = 0;
    bool m_captchaPending = false;
    // 短信登录会话：tm 由发码接口下发，登录时要原样带回
    QString m_kwTm;
    QString m_kwPhone;
    // 咪咕扫码会话：qrc_sessionid 是轮询凭据，JSESSIONID 必须跟着一起发回门户
    QString m_mgSession;
    QString m_mgCookie;
    // 咪咕扫码成功后拿到的凭据（ticket/uid 等），用于换取门户登录态
    QString m_mgTicket;
    // 汽水扫码会话：base 是自建账号 API 服务地址（/auth 契约）；未配置时登录走官方
    // passport/web/* 直连（m_qsBase 留空即可区分两条链路）。
    QString m_qsBase;
    QString m_qsToken;
    // 汽水扫码会话的 Cookie 映射：passport get_qrcode 会下发 passport_csrf_token，
    // check_qrconnect 确认后下发 sessionid，轮询与登录都要带上同一份。
    QMap<QString, QString> m_qsCookies;
    // 连续收到空回复/无法解析的次数，用于把接口异常显式暴露出来而不是一直"等待扫码"
    int m_qrEmptyStreak = 0;
    // 网易云扫码会话的 Cookie 罐（只保存 name=value）：出码与轮询共用一个会话
    QMap<QString, QString> m_neeCookies;
    // 匿名设备令牌 MUSIC_A（register/anonimous 取得），持久化后每次启动复用同一身份
    QString m_neeMusicA;
    // 本次扫码是否走 eapi：eapi 不可用时退回明文 /api，轮询要跟着用同一套
    bool m_qrEapi = true;
    bool m_neeBootstrapping = false;
    QVector<std::function<void(bool)>> m_neeBootWaiters;
};

} // namespace Furui
