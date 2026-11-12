#pragma once
// 福瑞音乐 - QQ 音乐登录与会话协议工具（应用内直连，不经过任何第三方服务）
//
// 依据 y.qq.com 现行前端实现（portal/wx_redirect.html 与 ptlogin c_login_2.js）：
//   ptqrtoken = hash33(qrsig) & 0x7fffffff          —— 扫码轮询的校验参数
//   g_tk      = hash33(qqmusic_key|qm_keyst|p_skey|skey) & 0x7fffffff
//   ptqrlogin 响应是 JS 回调 ptuiCB('码','x','跳转地址','2052','昵称','x')
//   扫码确认后拿到的 code 交给 musicu.fcg 的 QQConnectLogin.LoginServer/QQLogin 换成
//   {musicid, musickey, openid, access_token, nick, logo}，即本机的登录态
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace Furui {
namespace Qq {

// ptlogin 与 g_tk 共用的 33 进制哈希
quint32 hash33(const QString &text);

// g_tk：Cookie 里的 qqmusic_key / qm_keyst / p_skey / skey 依次尝试
QString gTk(const QString &cookie);

// 纯数字 uin：去掉 Cookie/返回里的前导 0 与非数字字符
QString numericUin(const QString &uin);

// 解析 ptuiCB(...)：返回引号内字段列表（第 0 项是状态码，第 2 项是跳转地址）
QStringList ptuiCbFields(const QByteArray &body);

// 从重定向链路的 URL 里取授权码（code / authorization_code）
QString authCodeFromUrl(const QUrl &url);

// QQLogin 请求体：code 为扫码确认后拿到的授权码
QJsonObject loginByCodeRequest(const QString &code);
// QQLogin 请求体：musicid/musickey 用于校验并续期已有登录态（Cookie 登录走这条）
QJsonObject refreshKeyRequest(const QString &musicid, const QString &musickey);

// 把 QQLogin 返回的凭证组装成后续接口使用的 Cookie 串
QString sessionCookie(const QString &musicid, const QString &musickey,
                      const QString &openid, const QString &accessToken);

} // namespace Qq
} // namespace Furui
