// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureTriggers.h"

#include <array>
#include <cstring>
#include <map>

#include "DolphinQt/Settings.h"

#ifdef HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace BigPictureTriggers
{
int Mode()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/adaptive_triggers"), 1).toInt();
}

#ifdef HAVE_SDL3
namespace
{
// Same layout as SDL's DS5EffectsState_t (see SDL's testcontroller.c)
struct DS5Effects
{
  Uint8 enable_bits1;
  Uint8 enable_bits2;
  Uint8 rumble_right;
  Uint8 rumble_left;
  Uint8 headphone_volume;
  Uint8 speaker_volume;
  Uint8 microphone_volume;
  Uint8 audio_enable_bits;
  Uint8 mic_light_mode;
  Uint8 audio_mute_bits;
  Uint8 right_trigger[11];
  Uint8 left_trigger[11];
  Uint8 unknown1[6];
  Uint8 enable_bits3;
  Uint8 unknown2[2];
  Uint8 led_anim;
  Uint8 led_brightness;
  Uint8 pad_lights;
  Uint8 led_red;
  Uint8 led_green;
  Uint8 led_blue;
};
static_assert(sizeof(DS5Effects) == 47);

std::array<Uint8, 11> Effect(int mode)
{
  std::array<Uint8, 11> e{};
  if (mode <= 0)
  {
    e[0] = 0x05;  // no effect
    return e;
  }
  // "Weapon" effect: resistance from zone 6 that gives way at zone 8 (of 0..9), i.e. the
  // trigger travels freely, then clicks near the end like a GameCube L / R.
  constexpr int start = 6, end = 8;
  const int strength = mode == 2 ? 8 : 5;  // 1..8
  const Uint16 zones = static_cast<Uint16>((1 << start) | (1 << end));
  e[0] = 0x25;
  e[1] = static_cast<Uint8>(zones & 0xff);
  e[2] = static_cast<Uint8>(zones >> 8);
  e[3] = static_cast<Uint8>(strength - 1);
  return e;
}

struct Pad
{
  SDL_Gamepad* gamepad = nullptr;
  int applied = -1;  // effect mode last sent
};
std::map<SDL_JoystickID, Pad> s_pads;

void Apply(int mode)
{
  if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    return;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  std::map<SDL_JoystickID, Pad> seen;
  for (int i = 0; ids && i < count; ++i)
  {
    if (SDL_GetGamepadTypeForID(ids[i]) != SDL_GAMEPAD_TYPE_PS5)
      continue;
    Pad pad;
    if (const auto it = s_pads.find(ids[i]); it != s_pads.end())
    {
      pad = it->second;
      s_pads.erase(it);
    }
    if (!pad.gamepad)
      pad.gamepad = SDL_OpenGamepad(ids[i]);
    if (pad.gamepad && pad.applied != mode)
    {
      DS5Effects fx{};
      fx.enable_bits1 = 0x04 | 0x08;  // right and left trigger effects
      const auto e = Effect(mode);
      std::memcpy(fx.right_trigger, e.data(), e.size());
      std::memcpy(fx.left_trigger, e.data(), e.size());
      if (SDL_SendGamepadEffect(pad.gamepad, &fx, sizeof(fx)))
        pad.applied = mode;
    }
    seen[ids[i]] = pad;
  }
  SDL_free(ids);
  // Controllers that went away
  for (auto& [id, pad] : s_pads)
  {
    if (pad.gamepad)
      SDL_CloseGamepad(pad.gamepad);
  }
  s_pads = std::move(seen);
}
}  // namespace

void Update(bool active)
{
  static int s_last = -2;
  static int s_ticks = 0;
  const int mode = active ? Mode() : 0;
  // Re-check for newly connected controllers once a second, at once when the state changes
  if (mode == s_last && ++s_ticks < 16)
    return;
  s_ticks = 0;
  if (mode == 0 && s_last == 0 && s_pads.empty())
    return;
  s_last = mode;
  Apply(mode);
}

void Release()
{
  Apply(0);
  for (auto& [id, pad] : s_pads)
  {
    if (pad.gamepad)
      SDL_CloseGamepad(pad.gamepad);
  }
  s_pads.clear();
}
#else
void Update(bool)
{
}
void Release()
{
}
#endif
}  // namespace BigPictureTriggers
