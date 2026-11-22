// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureUpdater.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

#include <cstdio>

#include "Common/HttpRequest.h"

#ifndef BP_BUILD_ID
#define BP_BUILD_ID "dev"
#endif

namespace
{
#ifdef _WIN32
constexpr char RELEASE_API[] =
    "https://api.github.com/repos/DestyNova42/dolphin/releases/tags/manette-latest";
constexpr char ASSET_NAME[] = "Dolphin-Manette-Windows.zip";
#else
constexpr char RELEASE_API[] =
    "https://api.github.com/repos/DestyNova42/dolphin/releases/tags/manette-linux-latest";
constexpr char ASSET_NAME[] = "Dolphin-Manette-x86_64.AppImage";
#endif

template <typename F>
void Post(QObject* context, F&& fn)
{
  QPointer<QObject> guard(context);
  QMetaObject::invokeMethod(
      context,
      [guard, fn = std::forward<F>(fn)]() mutable {
        if (guard)
          fn();
      },
      Qt::QueuedConnection);
}
}  // namespace

namespace BigPictureUpdater
{
QString BuildId()
{
  return QStringLiteral(BP_BUILD_ID);
}

void Check(QObject* context,
           std::function<void(bool, const QString&, const QString&, const QString&)> on_result)
{
  std::thread([context, on_result = std::move(on_result)] {
    Common::HttpRequest request(std::chrono::seconds(15));
    const auto response = request.Get(
        RELEASE_API, {{"User-Agent", "Dolphin-Manette"}, {"Accept", "application/vnd.github+json"}});
    bool available = false;
    QString version, url, error;
    if (!response)
    {
      error = QCoreApplication::translate("BigPicture", "Impossible de joindre GitHub");
    }
    else
    {
      const QJsonObject release =
          QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(response->data()),
                                             static_cast<qsizetype>(response->size())))
              .object();
      version = release.value(QStringLiteral("name")).toString();
      for (const QJsonValue& asset : release.value(QStringLiteral("assets")).toArray())
      {
        if (asset.toObject().value(QStringLiteral("name")).toString() ==
            QLatin1String(ASSET_NAME))
        {
          url = asset.toObject().value(QStringLiteral("browser_download_url")).toString();
        }
      }
      if (url.isEmpty() || version.isEmpty())
        error = QCoreApplication::translate("BigPicture", "Aucune version publiée");
      else
        available = version != BuildId();
    }
    Post(context, [on_result, available, version, url, error] {
      on_result(available, version, url, error);
    });
  }).detach();
}

void Install(QObject* context, const QString& url, std::function<void(bool, const QString&)> on_ready)
{
  std::thread([context, url, on_ready = std::move(on_ready)] {
    Common::HttpRequest request(std::chrono::minutes(15));
    const auto response =
        request.Get(url.toStdString(), {{"User-Agent", "Dolphin-Manette"}});
    const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#ifdef _WIN32
    const QString zip = QDir(temp).filePath(QStringLiteral("Dolphin-Manette-update.zip"));
#else
    // Linux: the new AppImage is written next to the running one, then replaces it.
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    const QString zip = appimage.isEmpty() ? QString() : appimage + QStringLiteral(".new");
    if (zip.isEmpty())
    {
      Post(context, [on_ready] {
        on_ready(false, QCoreApplication::translate("BigPicture",
                                                    "Mise à jour possible seulement avec l'AppImage"));
      });
      return;
    }
#endif
    bool ok = false;
    QString error;
    if (!response || response->empty())
    {
      error = QCoreApplication::translate("BigPicture", "Téléchargement impossible");
    }
    else
    {
      QFile file(zip);
      ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(reinterpret_cast<const char*>(response->data()),
                      static_cast<qint64>(response->size())) ==
               static_cast<qint64>(response->size());
      if (!ok)
        error = QCoreApplication::translate("BigPicture", "Écriture du fichier impossible");
    }

    Post(context, [on_ready, ok, error, zip, temp] {
      if (!ok)
      {
        on_ready(false, error);
        return;
      }
#ifndef _WIN32
      {
        const QString target = qEnvironmentVariable("APPIMAGE");
        QFile::setPermissions(zip, QFile::permissions(zip) | QFileDevice::ExeOwner |
                                       QFileDevice::ExeUser | QFileDevice::ExeGroup |
                                       QFileDevice::ExeOther);
        // Replacing a running AppImage is fine on Linux: it keeps running from the old file.
        const bool replaced = ::rename(zip.toLocal8Bit().constData(),
                                       target.toLocal8Bit().constData()) == 0;
        if (replaced)
        {
          QProcess::startDetached(QStringLiteral("sh"),
                                  {QStringLiteral("-c"),
                                   QStringLiteral("sleep 2; exec \"$0\""), target});
        }
        on_ready(replaced, replaced ? QString() :
                                      QCoreApplication::translate("BigPicture",
                                                                  "Remplacement impossible"));
        return;
      }
#endif
      // Script run after Dolphin quits: unpack over the installation, then restart.
      const QString script = QDir(temp).filePath(QStringLiteral("Dolphin-Manette-update.ps1"));
      QFile ps1(script);
      if (!ps1.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
      {
        on_ready(false, QCoreApplication::translate("BigPicture", "Script de mise à jour impossible"));
        return;
      }
      ps1.write(R"(param([string]$Zip, [string]$Dest, [int]$ProcId, [string]$Exe)
try { Wait-Process -Id $ProcId -Timeout 60 -ErrorAction SilentlyContinue } catch {}
Start-Sleep -Milliseconds 800
$tmp = Join-Path $env:TEMP 'Dolphin-Manette-update'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $Zip -DestinationPath $tmp -Force
Copy-Item -Path (Join-Path $tmp '*') -Destination $Dest -Recurse -Force
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Start-Process -FilePath $Exe
)");
      ps1.close();
      const bool started = QProcess::startDetached(
          QStringLiteral("powershell.exe"),
          {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
           QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
           QStringLiteral("-File"), script, zip, QCoreApplication::applicationDirPath(),
           QString::number(QCoreApplication::applicationPid()),
           QCoreApplication::applicationFilePath()});
      on_ready(started,
               started ? QString() :
                         QCoreApplication::translate("BigPicture", "PowerShell introuvable"));
    });
  }).detach();
}
}  // namespace BigPictureUpdater
