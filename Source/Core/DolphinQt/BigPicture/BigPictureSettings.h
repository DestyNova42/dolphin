// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Settings of Dolphin (and of the controller mode itself) as a controller-navigable list.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "Common/CommonTypes.h"

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
  std::function<void()> clear;      // X button (optional), e.g. unmap a control
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
  // Map one control: group is a PadGroup, mirror_index/flip as in the setup wizard.
  std::function<void(int port, int group, int index, int mirror_index, bool flip,
                     const QString& prompt)>
      remap_control;
  std::function<void(int port, int group)> calibrate_stick;
  std::function<void()> open_memory_cards;
  std::function<void(int port)> test_controller;
  std::function<void()> check_updates;
  std::function<void()> install_drivers;
  std::function<void(int port)> test_wiimote;
  std::function<void()> sync_now;
  std::function<void(const QString&)> notify;  // short message on screen
  std::function<void()> open_classic;
  std::function<void()> interface_changed;  // a controller-mode preference changed
};

namespace Config
{
class Layer;
}

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
  Cube3D,
};
// The colours of the GameCube models
enum class Theme
{
  IndigoNight,
  IndigoGameCube,
  Spice,
  JetBlack,
  Platinum,
  PearlWhite,
  GameColor,
};
enum class IntroStyle
{
  GameCube,
  Minimal,
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
Theme GetTheme();
IntroStyle GetIntroStyle();
bool AutoSwitchController();
bool CheckUpdatesOnStart();
Transition GetTransition();
bool IntroEnabled();
bool MusicEnabled();
int MusicVolume();  // 0-100
int SfxVolume();    // 0-100
}  // namespace BigPicturePrefs

std::vector<BigPictureSettingCategory> BuildBigPictureSettings(const BigPictureSettingsHost& host);

// Per-game settings, written to the game's INI in GameSettings (layer kept alive by the caller).
std::vector<BigPictureSettingCategory>
BuildBigPictureGameSettings(const std::shared_ptr<Config::Layer>& layer);

// Short list for the pause menu.
std::vector<BigPictureSettingCategory> BuildBigPictureQuickSettings();

// Gecko / Action Replay codes of a game (enabled state saved immediately in GameSettings).
std::vector<BigPictureSettingCategory> BuildBigPictureCheatSettings(const std::string& game_id,
                                                                     u16 revision);

// Per-device controller profiles: <User>/Config/Profiles/GCPad/Mode manette - <device>.ini
std::string BigPictureProfilePath(const std::string& device_name);
void SaveBigPictureDeviceProfile(int port);
// Saves the controller settings and the profile of every controller in use.
void SaveBigPicturePads();
// GameCube buttons on GameCube buttons: Switch 2 GameCube controller (USB or Bluetooth).
bool ApplyGameCubeProfile(int port, const std::string& device);
bool IsSwitch2GameCube(const std::string& device);
bool LoadBigPictureDeviceProfile(int port, const std::string& device);

// Control groups of every emulated controller: code = kind * 100 + group
// (kind 0 GameCube pad / PadGroup, 1 Wii Remote, 2 Nunchuk, 3 Classic Controller).
namespace ControllerEmu
{
class ControlGroup;
class EmulatedController;
}  // namespace ControllerEmu
ControllerEmu::EmulatedController* BigPictureController(int port, int code);
ControllerEmu::ControlGroup* BigPictureGroup(int port, int code);
int BigPictureWiiExtension(int port);  // 0 none, 1 Nunchuk, 2 Classic Controller
// Wii Remote (+ Nunchuk) or Classic Controller on the controller of the same player.
bool ApplyWiiProfile(int port, int extension);
std::vector<BigPictureSettingCategory> BuildBigPictureWiiControllers(const BigPictureSettingsHost& host);

// Per game: HD textures and controller profile.
bool HasTexturePack(const std::string& game_id);
std::vector<BigPictureSettingCategory>
BuildBigPictureGameExtras(const std::shared_ptr<Config::Layer>& layer, const std::string& game_id,
                          const QString& title, std::function<void(const QString&)> notify);

// "Portable" / "TV" presets, applied for the next game (current-run settings only).
void ApplyBigPictureDisplayPreset(int screen_height);

// Writes changed settings to disk.
void SaveBigPictureSettings();

// Applies a standard gamepad layout (Xbox / PlayStation / Switch Pro…) to a GameCube port.
bool ApplyStandardGamepadProfile(int port, const std::string& device = {});
