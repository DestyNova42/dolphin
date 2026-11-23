// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QTransform>

class QPainter;

// Plays the animated channel banner of a Wii game (opening.bnr): the scene the Wii Menu shows
// when the game's channel is selected, with its sound. Display only; no emulation involved.
namespace WiiBanner
{
struct Sound
{
  std::vector<int16_t> samples;  // interleaved stereo
  int rate = 32000;
  bool loop = false;
  size_t loop_start = 0;  // in frames
};

class Banner
{
public:
  // `data`: the whole opening.bnr file (IMET header + U8 archive).
  static std::unique_ptr<Banner> Load(const QByteArray& data);
  ~Banner();

  QSizeF LayoutSize() const { return m_size; }

  // Advances the animation by `frames` (60 per second): banner_Start once, then banner_Loop.
  void Advance(double frames);
  void Restart();

  // Draws the scene so that the layout's height fills `target` (wider screens see more of it,
  // like the Wii Menu in 16:9).
  void Render(QPainter& painter, const QRectF& target);

  const Sound& GetSound() const { return m_sound; }

  struct Impl;

private:
  Banner();
  std::unique_ptr<Impl> m_impl;
  QSizeF m_size;
  Sound m_sound;
};

// Helpers, public for the tests
QByteArray DecompressLZ77(const QByteArray& data);
std::map<std::string, QByteArray> ReadU8(const QByteArray& data);
QImage DecodeTPL(const QByteArray& data);
Sound DecodeBNS(const QByteArray& data);
}  // namespace WiiBanner
