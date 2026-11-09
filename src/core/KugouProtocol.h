#pragma once
// 福瑞音乐 - 酷狗网关签名与扫码登录参数
//
// 签名算法（依据 MakcRe/KuGouMusicApi 与 ImUpXuu/KuGouWebPlayer，并已用真实请求验证）：
//   signature = md5( keyparam + 排序后的 "k=v" 直接拼接 + body原文 + keyparam )
//   参与签名的参数集合不含 signature 自身；body 参与签名的字节必须与实际发送的完全一致。
// 域名说明：jtb/japi/loginsdk/api/oauth.kugou.com 现在都已 DNS 不存在，
// 可用的是 login-user.kugou.com（扫码）与 gateway.kugou.com（业务接口）。
//
// gateway.kugou.com 的实测规矩：用 App 盐且 appid=1005（否则退 20006）、
// 必须带 x-router 头声明后端服务（路由先于签名校验：头缺失或域名不存在退 502，
// 域名在但路径不认识退 60020「无效 uri」）、请求体不能为空（空体退 20010）。
// token 绑 appid，用别的 appid 扫的码来这里用退 20018；退 20017 表示签名已通过、只是登录态不行。
// 另有一类接口把服务名写在路径里（如 /pubsongs/...），这种不用 x-router。
#include <QByteArray>
#include <QString>
#include <QVariantMap>

namespace Furui {
namespace Kugou {

// 酷狗有两把 keyparam：扫码等 Web 接口用 Web，gateway.kugou.com 的业务接口用 App。
// 已用真实请求验证：gateway 用错盐退 20006（签名不通过），用对退 20017（只怪 token 是假的）。
enum class Keyparam { Web, App };

// 对应端的 keyparam
QString keyparam(Keyparam kp);

// Web 端 keyparam（等价于 keyparam(Keyparam::Web)，扫码接口用）
QString webSalt();

// 一次登录会话内固定的公共参数：dfid/mid/uuid/appid/clientver/clienttime
QVariantMap baseParams();

// 同一批公共参数按 kg 前缀写进端点模板变量表：kgMid / kgUuid / kgDfid / kgAppid /
// kgClientver / kgClienttime。酷狗连游客态的歌单接口都要签名，签名里必须带上这些设备参数，
// 所以公共链路（MusicSource）和登录链路（UserApi）都调它，避免两边各写一份。
void addDeviceVars(QVariantMap *vars);

// 登录态两项：扫码存进 Cookie 的 token 与 userid，写成 kgToken / kgUserid。
// 酷狗的网关接口不认 Cookie 头，token 和 userid 是当普通参数（query 或请求体）送的，
// 而且会参与签名，所以必须单独摊成模板变量。Cookie 里没有 userid 时退回变量表里已有的 uid。
void addAuthVars(QVariantMap *vars, const QString &cookie);

// 按上面的规则算 signature（params 里不要带 signature）
QString sign(const QVariantMap &params, const QByteArray &body = QByteArray(),
             Keyparam kp = Keyparam::Web);

// 加上 signature 并拼成完整 URL（值按 percent-encoding 送出，服务端解出来参与校验）
QString signedUrl(const QString &baseUrl, QVariantMap params,
                  const QByteArray &body = QByteArray(), Keyparam kp = Keyparam::Web);

// 扫码成功后写进本地登录态的 Cookie
QString sessionCookie(const QString &token, const QString &userid);

} // namespace Kugou
} // namespace Furui
