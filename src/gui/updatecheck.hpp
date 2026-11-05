// updatecheck.hpp — asks GitHub, once per start, whether a newer Kiln has been released.
// One HTTPS request to api.github.com, nothing about the user in it; any failure (no
// network, GitHub down, a reply that is not what we expect) is silent.
#pragma once

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

namespace kiln {

class UpdateCheck : public QObject {
    Q_OBJECT

public:
    explicit UpdateCheck(QObject* parent = nullptr);
    void start();

signals:
    /// A release newer than this build: its version ("0.2.0") and its page on GitHub.
    void available(const QString& version, const QUrl& page);

private:
    QNetworkAccessManager* net_;
};

}  // namespace kiln
