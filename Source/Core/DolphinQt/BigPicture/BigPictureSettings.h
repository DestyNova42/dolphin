// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Settings of Dolphin (and of the controller mode itself) as a controller-navigable list.

#pragma once

#include <functional>
#include <vector>

#include <QString>

struct BigPictureSettingItem
{
  enum class Kind
  {
    Toggle,
    Choice,
    Action,
  };

  QString label;
  QString help;
  Kind kind = Kind::Choice;
  std::function<QString()> value;   // Choice: current value, shown between arrows
  std::function<bool()> is_on;      // Toggle: current state
  std::function<void(int)> change;  // Left/Right (-1/+1). Toggles flip.
  std::function<void()> activate;   // A button (actions; toggles flip; choices step forward)
};

struct BigPictureSettingCategory
{
  QString name;
  std::vector<BigPictureSettingItem> items;
};

// What the settings need from the controller-mode window.
struct BigPictureSettingsHost
{
  std::function<void(int port)> configure_controller;
  std::function<void()> open_classic;
  std::function<void()> interface_changed;  // a controller-mode preference changed
};

// Controller-mode preferences (stored in Qt settings, "bigpicture/...").
namespace BigPicturePrefs
{
enum class View
{
  Covers,
  List,
};
enum class Effect
{
  Halo,
  Particles,
  Cubes,
  Waves,
  None,
};
enum class Transition
{
  Fade,
  Slide,
  Zoom,
  None,
};

View GetView();
Effect GetEffect();
Transition GetTransition();
bool IntroEnabled();
bool MusicEnabled();
int MusicVolume();  // 0-100
int SfxVolume();    // 0-100
}  // namespace BigPicturePrefs

std::vector<BigPictureSettingCategory> BuildBigPictureSettings(const BigPictureSettingsHost& host);

// Writes changed settings to disk.
void SaveBigPictureSettings();
