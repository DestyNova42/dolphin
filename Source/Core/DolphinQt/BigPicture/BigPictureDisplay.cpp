// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureDisplay.h"

#ifdef _WIN32
#include <Windows.h>

#include <vector>

#pragma comment(lib, "user32")
#endif

namespace BigPictureDisplay
{
#ifdef _WIN32
namespace
{
struct Saved
{
  LUID adapter;
  UINT32 id;
  bool enabled;
};
std::vector<Saved> s_saved;

void SetState(const LUID& adapter, UINT32 id, bool on)
{
  DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE set{};
  set.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
  set.header.size = sizeof(set);
  set.header.adapterId = adapter;
  set.header.id = id;
  set.enableAdvancedColor = on ? 1 : 0;
  DisplayConfigSetDeviceInfo(&set.header);
}
}  // namespace

void MatchHdr(bool hdr)
{
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
    return;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS)
  {
    return;
  }
  for (UINT32 i = 0; i < path_count; ++i)
  {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size = sizeof(info);
    info.header.adapterId = paths[i].targetInfo.adapterId;
    info.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
      continue;
    // Only screens that can do HDR
    if (!info.advancedColorSupported || info.advancedColorForceDisabled)
      continue;
    const bool enabled = info.advancedColorEnabled != 0;
    if (enabled == hdr)
      continue;
    bool known = false;
    for (const Saved& s : s_saved)
    {
      if (s.id == info.header.id && s.adapter.LowPart == info.header.adapterId.LowPart &&
          s.adapter.HighPart == info.header.adapterId.HighPart)
        known = true;
    }
    if (!known)
      s_saved.push_back({info.header.adapterId, info.header.id, enabled});
    SetState(info.header.adapterId, info.header.id, hdr);
  }
}

void RestoreHdr()
{
  for (const Saved& s : s_saved)
    SetState(s.adapter, s.id, s.enabled);
  s_saved.clear();
}

#else

void MatchHdr(bool)
{
}
void RestoreHdr()
{
}

#endif
}  // namespace BigPictureDisplay
