// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Keeps the saves of two machines (PC, console) in step through a shared folder
// (OneDrive, Syncthing, Nextcloud…): GameCube memory cards, Wii saves, "Resume" states.

#pragma once

#include <QString>

namespace BigPictureSync
{
// The folder chosen in the settings ("" = off).
QString Folder();
// Copies in both directions, the most recent file wins. Returns the number of files copied,
// or -1 if the folder can't be used. Call it only when no game runs.
int SyncNow();
}  // namespace BigPictureSync
