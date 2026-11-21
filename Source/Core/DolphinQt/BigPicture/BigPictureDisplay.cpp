// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureDisplay.h"

#ifdef _WIN32
#include <Windows.h>

#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "user32")
#endif

namespace BigPictureDisplay
{
#ifdef _WIN32
namespace
{
// Windows 11 24H2 API (declared here: the SDK hides it behind recent NTDDI versions).
// The older "advanced color" call only turns on colour management there, not HDR.
constexpr auto GET_ADVANCED_COLOR_INFO_2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
constexpr auto SET_HDR_STATE = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16);

struct AdvancedColorInfo2
{
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  union
  {
    struct
    {
      UINT32 advancedColorSupported : 1;
      UINT32 advancedColorActive : 1;
      UINT32 reserved1 : 1;
      UINT32 advancedColorLimitedByPolicy : 1;
      UINT32 highDynamicRangeSupported : 1;
      UINT32 highDynamicRangeUserEnabled : 1;
      UINT32 wideColorSupported : 1;
      UINT32 wideColorUserEnabled : 1;
      UINT32 reserved : 24;
    };
    UINT32 value;
  };
  UINT32 colorEncoding;
  UINT32 bitsPerColorChannel;
  UINT32 activeColorMode;  // 0 SDR, 1 WCG, 2 HDR
};

struct SetHdrState
{
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  union
  {
    struct
    {
      UINT32 enableHdr : 1;
      UINT32 reserved : 31;
    };
    UINT32 value;
  };
};

struct Target
{
  LUID adapter;
  UINT32 id;
};

struct Saved
{
  Target target;
  bool hdr;
};
std::vector<Saved> s_saved;

std::vector<Target> ActiveTargets()
{
  std::vector<Target> targets;
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
    return targets;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS)
  {
    return targets;
  }
  for (UINT32 i = 0; i < path_count; ++i)
    targets.push_back({paths[i].targetInfo.adapterId, paths[i].targetInfo.id});
  return targets;
}

// supported / enabled HDR, with the new API when present
bool QueryHdr(const Target& t, bool* supported)
{
  AdvancedColorInfo2 info2{};
  info2.header.type = GET_ADVANCED_COLOR_INFO_2;
  info2.header.size = sizeof(info2);
  info2.header.adapterId = t.adapter;
  info2.header.id = t.id;
  if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS)
  {
    *supported = info2.highDynamicRangeSupported != 0;
    return info2.activeColorMode == 2;
  }
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
  info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info.header.size = sizeof(info);
  info.header.adapterId = t.adapter;
  info.header.id = t.id;
  if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
  {
    *supported = false;
    return false;
  }
  *supported = info.advancedColorSupported && !info.advancedColorForceDisabled;
  return info.advancedColorEnabled != 0;
}

void SetHdr(const Target& t, bool on)
{
  SetHdrState set{};
  set.header.type = SET_HDR_STATE;
  set.header.size = sizeof(set);
  set.header.adapterId = t.adapter;
  set.header.id = t.id;
  set.enableHdr = on ? 1 : 0;
  if (DisplayConfigSetDeviceInfo(&set.header) == ERROR_SUCCESS)
    return;
  DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE old{};
  old.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
  old.header.size = sizeof(old);
  old.header.adapterId = t.adapter;
  old.header.id = t.id;
  old.enableAdvancedColor = on ? 1 : 0;
  DisplayConfigSetDeviceInfo(&old.header);
}

// Windows' own shortcut (Win + Alt + B): switches HDR of the main screen.
void PressHdrShortcut()
{
  INPUT in[6] = {};
  const WORD keys[3] = {VK_LWIN, VK_MENU, 'B'};
  for (int i = 0; i < 3; ++i)
  {
    in[i].type = INPUT_KEYBOARD;
    in[i].ki.wVk = keys[i];
    in[5 - i].type = INPUT_KEYBOARD;
    in[5 - i].ki.wVk = keys[i];
    in[5 - i].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  SendInput(6, in, sizeof(INPUT));
}

bool SameTarget(const Target& a, const Target& b)
{
  return a.id == b.id && a.adapter.LowPart == b.adapter.LowPart &&
         a.adapter.HighPart == b.adapter.HighPart;
}

void Apply(const Target& t, bool on)
{
  SetHdr(t, on);
  // Check: if Windows ignored the call, use its keyboard shortcut.
  for (int i = 0; i < 10; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool supported = false;
    if (QueryHdr(t, &supported) == on)
      return;
  }
  PressHdrShortcut();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
}
}  // namespace

void MatchHdr(bool hdr)
{
  for (const Target& t : ActiveTargets())
  {
    bool supported = false;
    const bool enabled = QueryHdr(t, &supported);
    if (!supported || enabled == hdr)
      continue;
    bool known = false;
    for (const Saved& s : s_saved)
      known |= SameTarget(s.target, t);
    if (!known)
      s_saved.push_back({t, enabled});
    Apply(t, hdr);
  }
}

void RestoreHdr()
{
  for (const Saved& s : s_saved)
  {
    bool supported = false;
    if (QueryHdr(s.target, &supported) != s.hdr)
      Apply(s.target, s.hdr);
  }
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
