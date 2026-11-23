// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Progress of the work done before a game shows its first image (shader compilation,
// custom texture preloading), for a frontend that draws its own loading screen.
// Host side only: nothing here changes how the console is emulated.

#pragma once

#include <atomic>
#include <cstdint>

namespace VideoCommon::LoadingProgress
{
enum class Phase : int
{
  None = 0,
  Shaders = 1,
  Textures = 2,
};

inline std::atomic<int> s_phase{0};
inline std::atomic<uint64_t> s_done{0};
inline std::atomic<uint64_t> s_total{0};
// The user gave up waiting: stop the current wait as soon as possible.
inline std::atomic<bool> s_cancel{false};
// A frontend draws the progress itself: the render window does not need to show it.
inline std::atomic<bool> s_external_ui{false};

inline void Report(Phase phase, uint64_t done, uint64_t total)
{
  s_done.store(done);
  s_total.store(total);
  s_phase.store(static_cast<int>(phase));
}

inline void Clear()
{
  s_phase.store(static_cast<int>(Phase::None));
  s_done.store(0);
  s_total.store(0);
}

inline bool CancelRequested()
{
  return s_cancel.load();
}
}  // namespace VideoCommon::LoadingProgress
