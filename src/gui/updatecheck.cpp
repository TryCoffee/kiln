// updatecheck.cpp
#include "updatecheck.hpp"

#include "version_compare.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace kiln {
namespace {

const char* const LATEST = "https://api.github.com/repos/TryCoffee/kiln/releases/latest";
// The only pages the popup may open: a reply cannot send the user anywhere else.
const char* const RELEASES = "https://github.com/TryCoffee/kiln/releases/";

}  // namespace

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {}

void UpdateCheck::start() {
    QNetworkRequest req{QUrl(QString::fromLatin1(LATEST))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Kiln/" KILN_VERSION));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(10000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QJsonObject r = QJsonDocument::fromJson(reply->read(256 * 1024)).object();
        if (r.value(QStringLiteral("draft")).toBool() || r.value(QStringLiteral("prerelease")).toBool()) return;
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        const QString page = r.value(QStringLiteral("html_url")).toString();
        const auto latest = parse_version(tag.toStdString());
        const auto mine = parse_version(KILN_VERSION);
        if (!latest || !mine || !version_newer(*latest, *mine)) return;
        if (!page.startsWith(QLatin1String(RELEASES))) return;
        const QUrl url(page, QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host() != QLatin1String("github.com")) return;
        QString version = tag;
        if (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V'))) version.remove(0, 1);
        emit available(version, url);
    });
}

}  // namespace kiln
