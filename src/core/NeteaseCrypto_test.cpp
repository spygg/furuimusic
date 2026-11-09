#include <QCoreApplication>
#include <cstdio>
#include "NeteaseCrypto.h"

using namespace Furui::Netease;

static QString qs(const QByteArray &b) { return QString::fromLatin1(b); }

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    fprintf(stderr, "selfTest=%s\n", selfTest() ? "PASS" : "FAIL");

    QJsonObject p;
    p.insert(QStringLiteral("username"), QStringLiteral("test@example.com"));
    p.insert(QStringLiteral("password"), QStringLiteral("secret"));
    p.insert(QStringLiteral("rememberLogin"), QStringLiteral("true"));
    QByteArray sk = "abcdefghijklmnop";
    QByteArray form = weapiBuild(p, sk);
    fprintf(stderr, "FORM=%s\n", form.constData());

    // 诊断 RSA：手写最小复现
    QByteArray reversed2 = sk;
    std::reverse(reversed2.begin(), reversed2.end());
    fprintf(stderr, "reversed=%s\n", qs(reversed2).toUtf8().constData());
    Q_UNUSED(reversed2);
    return 0;
}
