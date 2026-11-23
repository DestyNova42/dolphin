// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Menu music and sound effects of the controller mode.
// The sounds are not shipped: they are read from <User>/BigPicture/Sounds/ (FLAC files).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
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
    Erase,
    Count,
  };

  BigPictureAudio();
  ~BigPictureAudio();

  BigPictureAudio(const BigPictureAudio&) = delete;
  BigPictureAudio& operator=(const BigPictureAudio&) = delete;

  void Play(Sound sound);
  void StartMusic();
  void StopMusic();
  // 0..1 each; music 0 disables it.
  void SetVolumes(float music, float sfx);
  // Sound of a Wii channel banner (interleaved stereo); replaces the previous one.
  void PlayBanner(std::vector<s16> samples, int rate, bool loop, size_t loop_start);
  void StopBanner();
  bool HasMusic() const { return m_loaded && !m_music.empty(); }

  static std::string SoundDirectory();

  struct Clip
  {
    std::vector<s16> samples;  // interleaved stereo, at the file's own rate
    double step = 1.0;         // source frames per output frame
    size_t Frames() const { return samples.size() / 2; }
    bool empty() const { return samples.empty(); }
  };

private:
  struct Voice
  {
    const Clip* clip;
    double pos;
    float gain = 1.0f;
    int fade = -1;  // frames left of a quick fade-out (-1: playing normally)
  };

  void LoadAll();
  void OpenStream();
  void CloseStream();
  long Mix(float* out, long frames);

  static long DataCallback(cubeb_stream* stream, void* user_data, const void* input_buffer,
                           void* output_buffer, long num_frames);

  std::array<Clip, static_cast<size_t>(Sound::Count)> m_sounds;
  Clip m_music;
  Clip m_banner;
  double m_banner_pos = 0;
  bool m_banner_on = false;
  bool m_banner_loop = false;
  double m_banner_loop_start = 0;

  std::mutex m_mutex;
  std::vector<Voice> m_voices;
  std::array<std::chrono::steady_clock::time_point, 32> m_last_play{};
  float m_limiter = 1.0f;
  double m_music_pos = 0;
  bool m_music_on = false;
  float m_music_gain = 0.0f;
  float m_music_volume = 0.45f;
  float m_sfx_volume = 0.9f;

  std::atomic<bool> m_loaded{false};
  std::thread m_loader;

  std::shared_ptr<cubeb> m_ctx;
  cubeb_stream* m_stream = nullptr;
};
