// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Menu music and sound effects of the controller mode.
// The sounds are not shipped: they are read from <User>/BigPicture/Sounds/ (FLAC files).

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

struct cubeb;
struct cubeb_stream;

class BigPictureAudio
{
public:
  enum class Sound
  {
    Startup,
    Move,
    Page,
    Confirm,
    Back,
    Cancel,
    Launch,
    MenuOpen,
    Toggle,
    Error,
    SaveState,
    LoadState,
    Count,
  };

  BigPictureAudio();
  ~BigPictureAudio();

  BigPictureAudio(const BigPictureAudio&) = delete;
  BigPictureAudio& operator=(const BigPictureAudio&) = delete;

  void Play(Sound sound);
  void StartMusic();
  void StopMusic();
  bool HasMusic() const { return m_loaded && !m_music.empty(); }

  static std::string SoundDirectory();

private:
  using Clip = std::vector<float>;  // interleaved stereo, 48 kHz

  struct Voice
  {
    const Clip* clip;
    size_t pos;
  };

  void LoadAll();
  void OpenStream();
  void CloseStream();
  long Mix(float* out, long frames);

  static long DataCallback(cubeb_stream* stream, void* user_data, const void* input_buffer,
                           void* output_buffer, long num_frames);

  std::array<Clip, static_cast<size_t>(Sound::Count)> m_sounds;
  Clip m_music;

  std::mutex m_mutex;
  std::vector<Voice> m_voices;
  size_t m_music_pos = 0;
  bool m_music_on = false;
  float m_music_gain = 0.0f;

  std::atomic<bool> m_loaded{false};
  std::thread m_loader;

  std::shared_ptr<cubeb> m_ctx;
  cubeb_stream* m_stream = nullptr;
};
