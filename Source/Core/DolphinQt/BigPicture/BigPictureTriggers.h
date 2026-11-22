// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// DualSense adaptive triggers: a GameCube-like feel (free travel, then a click at the end).
// Only touches the trigger effects of PS5 controllers seen by SDL; nothing in the emulation.
namespace BigPictureTriggers
{
// 0 = off, 1 = GameCube click, 2 = firmer GameCube click
int Mode();

// Call regularly: applies the effect to every DualSense while `active` (in a game), clears it
// otherwise. Cheap when nothing changed.
void Update(bool active);

// Clears the effect on every DualSense (leaving the controller mode).
void Release();
}  // namespace BigPictureTriggers
