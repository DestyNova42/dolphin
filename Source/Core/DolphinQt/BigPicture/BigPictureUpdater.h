// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Updates of the controller-mode build: the GitHub workflow of the fork publishes every build
// (official Dolphin changes merged daily) as the "manette-latest" release.

#pragma once

#include <functional>

#include <QString>

class QObject;

namespace BigPictureUpdater
{
// Identifier of this build (the git commit it was built from), "dev" for local builds.
QString BuildId();

// Asks GitHub for the latest build. Callbacks run on the thread of `context`.
// on_result(update_available, version, download_url, error)
void Check(QObject* context,
           std::function<void(bool, const QString&, const QString&, const QString&)> on_result);

// Downloads the build, then replaces this installation once Dolphin has quit and restarts it.
// on_ready(ok, error) is called when Dolphin can quit.
void Install(QObject* context, const QString& url,
             std::function<void(bool, const QString&)> on_ready);
}  // namespace BigPictureUpdater
