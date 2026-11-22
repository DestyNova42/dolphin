// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// HDR of the screen follows Dolphin's HDR setting while a game runs (Windows).

#pragma once

#include <functional>

namespace BigPictureDisplay
{
// Puts the screens in HDR (or SDR) to match `hdr`, remembering their previous state.
void MatchHdr(bool hdr);
// Gives the screens back their previous state.
void RestoreHdr();

// Windows: calls on_power(true) before the PC sleeps, on_power(false) when it wakes up.
void InstallPowerNotifications(std::function<void(bool suspend)> on_power);
}  // namespace BigPictureDisplay
