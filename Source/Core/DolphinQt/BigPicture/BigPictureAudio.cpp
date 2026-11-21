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
constexpr float MUSIC_VOLUME = 0.45f;
constexpr float SFX_VOLUME = 0.9f;

// Part of the file name (lower case) that identifies each sound.
constexpr const char* SOUND_KEYS[] = {
    "startup",          // Startup
    "option move 1",    // Move
    "menu turn side",   // Page
    "option confirm",   // Confirm
    "menu back",        // Back
    "option cancel",    // Cancel
    "menu select",      // Launch
    "press start",      // MenuOpen
    "option select",    // Toggle
    "error",            // Error
    "memory card copy", // SaveState
    "memory card move", // LoadState
};
constexpr const char* MUSIC_KEY = "main menu";

std::string Lower(std::string s)
{
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Decodes a FLAC file to interleaved stereo at OUTPUT_RATE.
std::vector<float> DecodeFlac(const std::filesystem::path& path)
{
  unsigned int channels = 0;
  unsigned int rate = 0;
  drflac_uint64 frames = 0;
#ifdef _WIN32
  float* data = drflac_open_file_and_read_pcm_frames_f32_w(path.wstring().c_str(), &channels,
                                                           &rate, &frames, nullptr);
#else
  float* data = drflac_open_file_and_read_pcm_frames_f32(path.string().c_str(), &channels, &rate,
                                                         &frames, nullptr);
#endif
  std::vector<float> out;
  if (!data || channels == 0 || rate == 0 || frames == 0)
  {
    if (data)
      drflac_free(data, nullptr);
    return out;
  }

  const double step = static_cast<double>(rate) / OUTPUT_RATE;
  const size_t out_frames = static_cast<size_t>(frames / step);
  out.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; ++i)
  {
    const double src = i * step;
    const size_t a = static_cast<size_t>(src);
    const size_t b = std::min<size_t>(a + 1, frames - 1);
    const float t = static_cast<float>(src - a);
    for (int c = 0; c < 2; ++c)
    {
      const unsigned int sc = std::min<unsigned int>(c, channels - 1);
      const float va = data[a * channels + sc];
      const float vb = data[b * channels + sc];
      out[i * 2 + c] = va + (vb - va) * t;
    }
  }
  drflac_free(data, nullptr);
  return out;
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
  m_voices.push_back({&clip, 0});
}

void BigPictureAudio::StartMusic()
{
  std::lock_guard lk(m_mutex);
  m_music_on = true;
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
    const size_t music_frames = m_music.size() / 2;
    for (long i = 0; i < frames; ++i)
    {
      // ~0.5 s fades in and out
      const float target = m_music_on ? 1.0f : 0.0f;
      m_music_gain += (target - m_music_gain) * 0.0001f;
      if (!m_music_on && m_music_gain < 0.001f)
      {
        m_music_gain = 0.0f;
        break;
      }
      const float g = m_music_gain * MUSIC_VOLUME;
      out[i * 2] += m_music[m_music_pos * 2] * g;
      out[i * 2 + 1] += m_music[m_music_pos * 2 + 1] * g;
      if (++m_music_pos >= music_frames)
        m_music_pos = 0;
    }
  }

  for (Voice& v : m_voices)
  {
    const size_t clip_frames = v.clip->size() / 2;
    for (long i = 0; i < frames && v.pos < clip_frames; ++i, ++v.pos)
    {
      out[i * 2] += (*v.clip)[v.pos * 2] * SFX_VOLUME;
      out[i * 2 + 1] += (*v.clip)[v.pos * 2 + 1] * SFX_VOLUME;
    }
  }
  std::erase_if(m_voices, [](const Voice& v) { return v.pos >= v.clip->size() / 2; });

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
