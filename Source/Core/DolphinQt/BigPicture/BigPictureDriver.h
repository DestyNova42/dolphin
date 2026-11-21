// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// USB drivers for Nintendo controllers, without Zadig.
//
// The command interface of the Switch 2 controllers (GameCube NSO, Pro Controller 2,
// Joy-Con 2) needs the WinUSB driver. (The Wii U / Switch GameCube adapter works without it,
// through SDL.) Windows ships it, but
// never assigns it to these devices by itself. Dolphin assigns the Microsoft-signed WinUSB
// driver to them, after one Windows administrator confirmation.

#pragma once

#include <functional>
#include <string>
#include <vector>

class QObject;

namespace BigPictureDriver
{
struct Target
{
  std::wstring instance_id;
  std::string description;  // "Manette GameCube (Switch 2)", ...
};

// Devices plugged in that still need the driver.
std::vector<Target> DevicesNeedingDriver();

// Asks for administrator rights and installs the driver on every device that needs it.
// on_done(ok) runs on the thread of `context`.
void InstallAsync(QObject* context, std::function<void(bool)> on_done);

// Entry point of the elevated process (Dolphin.exe --bp-install-usb-drivers). Returns 0 on success.
int RunElevatedInstall();

constexpr char INSTALL_ARG[] = "--bp-install-usb-drivers";
}  // namespace BigPictureDriver
