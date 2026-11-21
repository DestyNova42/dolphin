// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Switch 2 GameCube controller (NSO) over Bluetooth LE, on Windows.
// The controller doesn't use standard HID over GATT: this backend talks its own protocol
// (documented by ndeadly/switch2_controller_research), without any extra software.

#pragma once

#include <memory>
#include <string>

#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface::Switch2BLE
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
// What the Bluetooth side is doing, for the interface ("Connectée", "Échec : ...").
std::string GetStatus();
}
