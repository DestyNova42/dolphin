// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureSync.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include "Common/FileUtil.h"
#include "DolphinQt/Settings.h"

namespace BigPictureSync
{
namespace
{
// What is synced, relative to Dolphin's user folder (the same layout on Windows and Linux).
bool IsSynced(const QString& rel)
{
  if (rel.startsWith(QStringLiteral("GC/")))
    return true;  // memory cards (files and GCI folders)
  if (rel.startsWith(QStringLiteral("Wii/title/")) && rel.contains(QStringLiteral("/data/")))
    return true;  // Wii saves only (not the installed channels)
  if (rel.startsWith(QStringLiteral("StateSaves/")) &&
      rel.contains(QStringLiteral(".manette-reprise.")))
    return true;  // "Resume" points
  return false;
}

QStringList Files(const QString& root)
{
  QStringList list;
  for (const QString& sub : {QStringLiteral("GC"), QStringLiteral("Wii/title"),
                             QStringLiteral("StateSaves")})
  {
    const QString base = QDir(root).filePath(sub);
    if (!QFileInfo(base).isDir())
      continue;
    QDirIterator it(base, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
      const QString rel = QDir(root).relativeFilePath(it.next());
      if (IsSynced(rel))
        list << rel;
    }
  }
  return list;
}

bool CopyNewer(const QString& from, const QString& to)
{
  QDir().mkpath(QFileInfo(to).absolutePath());
  const QString temp = to + QStringLiteral(".sync-tmp");
  QFile::remove(temp);
  if (!QFile::copy(from, temp))
    return false;
  QFile::remove(to);
  if (!QFile::rename(temp, to))
    return false;
  // Keep the date: otherwise the copy would look newer than the original next time.
  QFile f(to);
  if (f.open(QIODevice::ReadWrite))
    f.setFileTime(QFileInfo(from).lastModified(), QFileDevice::FileModificationTime);
  return true;
}
}  // namespace

QString Folder()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/sync_dir")).toString();
}

int SyncNow()
{
  const QString dir = Folder();
  if (dir.isEmpty())
    return 0;
  if (!QFileInfo(dir).isDir())
    return -1;
  const QString local = QString::fromStdString(File::GetUserPath(D_USER_IDX));
  const QString remote = QDir(dir).filePath(QStringLiteral("Dolphin-Manette"));
  QDir().mkpath(remote);

  QStringList all = Files(local) + Files(remote);
  all.removeDuplicates();
  int copied = 0;
  for (const QString& rel : all)
  {
    const QString a = QDir(local).filePath(rel);
    const QString b = QDir(remote).filePath(rel);
    const QFileInfo fa(a), fb(b);
    // 2 s of tolerance: file systems don't all keep the same precision
    if (fa.exists() && (!fb.exists() || fa.lastModified() > fb.lastModified().addSecs(2)))
      copied += CopyNewer(a, b) ? 1 : 0;
    else if (fb.exists() && (!fa.exists() || fb.lastModified() > fa.lastModified().addSecs(2)))
      copied += CopyNewer(b, a) ? 1 : 0;
  }
  return copied;
}
}  // namespace BigPictureSync
