#include "dropboxservice.h"

#include <QFileInfo>

#include <qjson/parser.h>

#include "core/application.h"
#include "core/logging.h"
#include "core/network.h"
#include "core/player.h"
#include "core/utilities.h"
#include "core/waitforsignal.h"
#include "internet/dropboxauthenticator.h"
#include "internet/dropboxurlhandler.h"
#include "library/librarybackend.h"

using Utilities::ParseRFC822DateTime;

const char* DropboxService::kServiceName = "Dropbox";
const char* DropboxService::kSettingsGroup = "Dropbox";

namespace {

static const char* kServiceId = "dropbox";

static const char* kMediaEndpoint =
    "https://api.dropbox.com/1/media/dropbox/";
static const char* kDeltaEndpoint =
    "https://api.dropbox.com/1/delta";

}  // namespace

DropboxService::DropboxService(Application* app, InternetModel* parent)
    : CloudFileService(
        app, parent,
        kServiceName, kServiceId,
        QIcon(":/providers/dropbox.png"),
        SettingsDialog::Page_Dropbox),
      network_(new NetworkAccessManager(this)) {
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  access_token_ = settings.value("access_token").toString();
  access_token_secret_ = settings.value("access_token_secret").toString();
  app->player()->RegisterUrlHandler(new DropboxUrlHandler(this, this));
}

bool DropboxService::has_credentials() const {
  return !access_token_.isEmpty();
}

void DropboxService::Connect() {
  if (has_credentials()) {
    RequestFileList();
  } else {
    ShowSettingsDialog();
  }
}

void DropboxService::AuthenticationFinished(DropboxAuthenticator* authenticator) {
  authenticator->deleteLater();

  access_token_ = authenticator->access_token();
  access_token_secret_ = authenticator->access_token_secret();

  QSettings settings;
  settings.beginGroup(kSettingsGroup);

  settings.setValue("access_token", access_token_);
  settings.setValue("access_token_secret", access_token_secret_);
  settings.setValue("name", authenticator->name());

  emit Connected();

  RequestFileList();
}

QByteArray DropboxService::GenerateAuthorisationHeader() {
  return DropboxAuthenticator::GenerateAuthorisationHeader(
      access_token_,
      access_token_secret_);
}

void DropboxService::RequestFileList() {
  QSettings s;
  s.beginGroup(kSettingsGroup);

  QUrl url = QUrl(QString(kDeltaEndpoint));
  if (s.contains("cursor")) {
    url.addQueryItem("cursor", s.value("cursor").toString());
  }
  QNetworkRequest request(url);
  request.setRawHeader("Authorization", GenerateAuthorisationHeader());

  QNetworkReply* reply = network_->post(request, QByteArray());
  NewClosure(reply, SIGNAL(finished()),
             this, SLOT(RequestFileListFinished(QNetworkReply*)), reply);
}

void DropboxService::RequestFileListFinished(QNetworkReply* reply) {
  reply->deleteLater();

  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  if (response.contains("reset") &&
      response["reset"].toBool()) {
    qLog(Debug) << "Resetting Dropbox DB";
    library_backend_->DeleteAll();
  }

  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  settings.setValue("cursor", response["cursor"].toString());

  QVariantList contents = response["entries"].toList();
  qLog(Debug) << "Delta found:" << contents.size();
  foreach (const QVariant& c, contents) {
    QVariantList item = c.toList();
    QString path = item[0].toString();

    QUrl url;
    url.setScheme("dropbox");
    url.setPath(path);

    if (item[1].isNull()) {
      // Null metadata indicates path deleted.
      qLog(Debug) << "Deleting:" << url;
      Song song = library_backend_->GetSongByUrl(url);
      if (song.is_valid()) {
        library_backend_->DeleteSongs(SongList() << song);
      }
      continue;
    }

    QVariantMap metadata = item[1].toMap();
    if (metadata["is_dir"].toBool()) {
      continue;
    }

    if (ShouldIndexFile(url, metadata["mime_type"].toString())) {
      QNetworkReply* reply = FetchContentUrl(url);
      NewClosure(reply, SIGNAL(finished()),
                 this, SLOT(FetchContentUrlFinished(QNetworkReply*, QVariantMap)),
                 reply, metadata);
    }
  }

  if (response.contains("has_more") && response["has_more"].toBool()) {
    RequestFileList();
  }
}

QNetworkReply* DropboxService::FetchContentUrl(const QUrl& url) {
  QUrl request_url(QString(kMediaEndpoint) + url.path());
  QNetworkRequest request(request_url);
  request.setRawHeader("Authorization", GenerateAuthorisationHeader());
  return network_->post(request, QByteArray());
}

void DropboxService::FetchContentUrlFinished(
    QNetworkReply* reply, const QVariantMap& data) {
  reply->deleteLater();
  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  QFileInfo info(data["path"].toString());

  QUrl url;
  url.setScheme("dropbox");
  url.setPath(data["path"].toString());

  Song song;
  song.set_url(url);
  song.set_etag(data["rev"].toString());
  song.set_mtime(ParseRFC822DateTime(data["modified"].toString()).toTime_t());
  song.set_title(info.fileName());
  song.set_filesize(data["bytes"].toInt());
  song.set_ctime(0);

  MaybeAddFileToDatabase(
      song,
      data["mime_type"].toString(),
      QUrl::fromEncoded(response["url"].toByteArray()),
      QString::null);
}

QUrl DropboxService::GetStreamingUrlFromSongId(const QUrl& url) {
  QNetworkReply* reply = FetchContentUrl(url);
  WaitForSignal(reply, SIGNAL(finished()));

  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  return response["url"].toUrl();
}
