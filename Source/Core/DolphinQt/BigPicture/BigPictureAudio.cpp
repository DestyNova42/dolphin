// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureAudio.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#ifdef HAVE_BIGPICTURE_CUBEB
#include <cubeb/cubeb.h>
#include "AudioCommon/CubebUtils.h"
#ifdef _WIN32
#include <Objbase.h>
#endif
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4267 4456 4457 4701 4703 4996)
#endif
#define DR_FLAC_IMPLEMENTATION
#include "DolphinQt/BigPicture/ThirdParty/dr_flac.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace
{
constexpr unsigned int OUTPUT_RATE = 48000;

// Part of the file name (lower case) that identifies each sound.
constexpr const char* SOUND_KEYS[] = {
    "startup",          // Startup
    "option move 1",    // Move
    "option move 3",    // Page (tabs)
    "option confirm",   // Confirm
    "menu back",        // Back
    "option cancel",    // Cancel
    "menu select",      // Launch
    "press start",      // MenuOpen
    "option select",    // Toggle
    "error",            // Error
    "memory card copy", // SaveState
    "memory card move", // LoadState
    "memory card erase",// Erase
};
constexpr const char* MUSIC_KEY = "main menu";

std::string Lower(std::string s)
{
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Decodes a FLAC file to interleaved stereo 16-bit at its own rate (resampled while mixing).
BigPictureAudio::Clip DecodeFlac(const std::filesystem::path& path)
{
#ifdef _WIN32
  drflac* flac = drflac_open_file_w(path.wstring().c_str(), nullptr);
#else
  drflac* flac = drflac_open_file(path.string().c_str(), nullptr);
#endif
  BigPictureAudio::Clip clip;
  if (!flac)
    return clip;
  const unsigned int channels = flac->channels;
  const unsigned int rate = flac->sampleRate;
  const drflac_uint64 total = flac->totalPCMFrameCount;
  if (channels == 0 || rate == 0 || total == 0)
  {
    drflac_close(flac);
    return clip;
  }
  std::vector<drflac_int16> data(static_cast<size_t>(total) * channels);
  const drflac_uint64 frames = drflac_read_pcm_frames_s16(flac, total, data.data());
  drflac_close(flac);

  clip.step = static_cast<double>(rate) / OUTPUT_RATE;
  clip.samples.resize(static_cast<size_t>(frames) * 2);
  for (size_t i = 0; i < frames; ++i)
  {
    clip.samples[i * 2] = data[i * channels];
    clip.samples[i * 2 + 1] = data[i * channels + (channels > 1 ? 1 : 0)];
  }
  return clip;
}

// Linear interpolation of a stereo frame at a fractional position.
inline void Sample(const BigPictureAudio::Clip& clip, double pos, float* l, float* r)
{
  const size_t frames = clip.Frames();
  const size_t a = static_cast<size_t>(pos);
  const size_t b = a + 1 < frames ? a + 1 : a;
  const float t = static_cast<float>(pos - static_cast<double>(a));
  constexpr float scale = 1.0f / 32768.0f;
  const float la = clip.samples[a * 2] * scale, lb = clip.samples[b * 2] * scale;
  const float ra = clip.samples[a * 2 + 1] * scale, rb = clip.samples[b * 2 + 1] * scale;
  *l = la + (lb - la) * t;
  *r = ra + (rb - ra) * t;
}
}  // namespace

std::string BigPictureAudio::SoundDirectory()
{
  return File::GetUserPath(D_USER_IDX) + "BigPicture/Sounds/";
}

BigPictureAudio::BigPictureAudio()
{
  // Opening the stream and decoding happen off the UI thread (COM is initialised there as MTA).
  m_loader = std::thread([this] {
    OpenStream();
    LoadAll();
    m_loaded = true;
  });
}

BigPictureAudio::~BigPictureAudio()
{
  if (m_loader.joinable())
    m_loader.join();
  CloseStream();
}

void BigPictureAudio::LoadAll()
{
  const std::filesystem::path dir = SoundDirectory();
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
  {
    std::filesystem::create_directories(dir, ec);
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
  {
    if (!entry.is_regular_file() || Lower(entry.path().extension().string()) != ".flac")
      continue;
    const std::string name = Lower(entry.path().filename().string());

    if (name.find(MUSIC_KEY) != std::string::npos)
    {
      Clip clip = DecodeFlac(entry.path());
      std::lock_guard lk(m_mutex);
      m_music = std::move(clip);
      continue;
    }
    for (size_t i = 0; i < std::size(SOUND_KEYS); ++i)
    {
      if (name.find(SOUND_KEYS[i]) != std::string::npos)
      {
        Clip clip = DecodeFlac(entry.path());
        std::lock_guard lk(m_mutex);
        m_sounds[i] = std::move(clip);
        break;
      }
    }
  }
}

void BigPictureAudio::Play(Sound sound)
{
  if (!m_loaded)
    return;
  const Clip& clip = m_sounds[static_cast<size_t>(sound)];
  if (clip.empty())
    return;
  std::lock_guard lk(m_mutex);
  // Navigation sounds cut the previous instance instead of piling up.
  std::erase_if(m_voices, [&](const Voice& v) { return v.clip == &clip; });
  m_voices.push_back({&clip, 0.0});
}

void BigPictureAudio::StartMusic()
{
  std::lock_guard lk(m_mutex);
  m_music_on = true;
}

void BigPictureAudio::SetVolumes(float music, float sfx)
{
  std::lock_guard lk(m_mutex);
  // Perceptual curve: 50 % on the slider sounds like half as loud.
  m_music_volume = 0.7f * music * music;
  m_sfx_volume = sfx * sfx;
}

void BigPictureAudio::StopMusic()
{
  std::lock_guard lk(m_mutex);
  m_music_on = false;
  m_voices.clear();
}

long BigPictureAudio::Mix(float* out, long frames)
{
  std::fill(out, out + frames * 2, 0.0f);
  std::lock_guard lk(m_mutex);

  if (!m_music.empty() && (m_music_on || m_music_gain > 0.0f))
  {
    const double music_frames = static_cast<double>(m_music.Frames());
    for (long i = 0; i < frames; ++i)
    {
      // short fades in and out
      const float target = m_music_on ? 1.0f : 0.0f;
      m_music_gain += (target - m_music_gain) * 0.0001f;
      if (!m_music_on && m_music_gain < 0.001f)
      {
        m_music_gain = 0.0f;
        break;
      }
      float l, r;
      Sample(m_music, m_music_pos, &l, &r);
      const float g = m_music_gain * m_music_volume;
      out[i * 2] += l * g;
      out[i * 2 + 1] += r * g;
      m_music_pos += m_music.step;
      if (m_music_pos >= music_frames - 1)
        m_music_pos = 0;
    }
  }

  for (Voice& v : m_voices)
  {
    const double clip_frames = static_cast<double>(v.clip->Frames()) - 1;
    for (long i = 0; i < frames && v.pos < clip_frames; ++i, v.pos += v.clip->step)
    {
      float l, r;
      Sample(*v.clip, v.pos, &l, &r);
      out[i * 2] += l * m_sfx_volume;
      out[i * 2 + 1] += r * m_sfx_volume;
    }
  }
  std::erase_if(m_voices, [](const Voice& v) {
    return v.pos >= static_cast<double>(v.clip->Frames()) - 1;
  });

  for (long i = 0; i < frames * 2; ++i)
    out[i] = std::clamp(out[i], -1.0f, 1.0f);
  return frames;
}

#ifdef HAVE_BIGPICTURE_CUBEB

long BigPictureAudio::DataCallback(cubeb_stream*, void* user_data, const void*,
                                   void* output_buffer, long num_frames)
{
  return static_cast<BigPictureAudio*>(user_data)->Mix(static_cast<float*>(output_buffer),
                                                       num_frames);
}

static void StateCallback(cubeb_stream*, void*, cubeb_state)
{
}

void BigPictureAudio::OpenStream()
{
#ifdef _WIN32
  // Cubeb's WASAPI backend needs COM on the calling thread.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
  (void)com;
#endif
  m_ctx = CubebUtils::GetContext();
  if (!m_ctx)
    return;

  cubeb_stream_params params{};
  params.rate = OUTPUT_RATE;
  params.channels = 2;
  params.format = CUBEB_SAMPLE_FLOAT32NE;
  params.layout = CUBEB_LAYOUT_STEREO;

  u32 latency = 0;
  if (cubeb_get_min_latency(m_ctx.get(), &params, &latency) != CUBEB_OK)
    latency = 1024;

  if (cubeb_stream_init(m_ctx.get(), &m_stream, "Dolphin Menu Audio", nullptr, nullptr, nullptr,
                        &params, std::max<u32>(latency, 512), DataCallback, StateCallback,
                        this) != CUBEB_OK)
  {
    WARN_LOG_FMT(AUDIO, "Controller mode: could not open the menu audio stream");
    m_stream = nullptr;
    return;
  }
  cubeb_stream_start(m_stream);
}

void BigPictureAudio::CloseStream()
{
  if (m_stream)
  {
    cubeb_stream_stop(m_stream);
    cubeb_stream_destroy(m_stream);
    m_stream = nullptr;
  }
  m_ctx.reset();
}

#else

long BigPictureAudio::DataCallback(cubeb_stream*, void*, const void*, void*, long num_frames)
{
  return num_frames;
}

void BigPictureAudio::OpenStream()
{
}

void BigPictureAudio::CloseStream()
{
}

#endif
