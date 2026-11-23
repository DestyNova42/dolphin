// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/WiiBanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <unordered_map>

#include <QPainter>
#include <QPainterPath>

namespace WiiBanner
{
namespace
{
// ------------------------------------------------------------------ bytes

uint16_t BE16(const char* p)
{
  return static_cast<uint16_t>((uint8_t(p[0]) << 8) | uint8_t(p[1]));
}
uint32_t BE32(const char* p)
{
  return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
         (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
}
float BEF(const char* p)
{
  const uint32_t u = BE32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

struct Span
{
  const char* data = nullptr;
  qsizetype size = 0;
  bool Has(qsizetype off, qsizetype n) const { return off >= 0 && n >= 0 && off + n <= size; }
  uint8_t U8(qsizetype o) const { return Has(o, 1) ? uint8_t(data[o]) : 0; }
  uint16_t U16(qsizetype o) const { return Has(o, 2) ? BE16(data + o) : 0; }
  uint32_t U32(qsizetype o) const { return Has(o, 4) ? BE32(data + o) : 0; }
  float F(qsizetype o) const { return Has(o, 4) ? BEF(data + o) : 0.0f; }
  std::string Str(qsizetype o, qsizetype max) const
  {
    std::string s;
    for (qsizetype i = o; i < size && i < o + max && data[i]; ++i)
      s.push_back(data[i]);
    return s;
  }
  Span Sub(qsizetype o, qsizetype n = -1) const
  {
    if (o < 0 || o > size)
      return {};
    Span s{data + o, n < 0 ? size - o : std::min<qsizetype>(n, size - o)};
    return s;
  }
};

Span SpanOf(const QByteArray& a)
{
  return {a.constData(), a.size()};
}

// Strips "IMD5" and decompresses LZ77 when present
QByteArray Unwrap(QByteArray d)
{
  if (d.startsWith("IMD5"))
    d = d.mid(0x20);
  if (d.startsWith("LZ77") || ((d.size() > 4) && (uint8_t(d[0]) == 0x10 || uint8_t(d[0]) == 0x11) &&
                               !d.startsWith("U\xAA" "8-")))
    d = DecompressLZ77(d);
  return d;
}
}  // namespace

// ------------------------------------------------------------------ containers

QByteArray DecompressLZ77(const QByteArray& in)
{
  QByteArray src = in.startsWith("LZ77") ? in.mid(4) : in;
  const Span d = SpanOf(src);
  if (d.size < 4)
    return {};
  const uint8_t type = d.U8(0);
  qsizetype p = 4;
  uint32_t size = uint32_t(d.U8(1)) | (uint32_t(d.U8(2)) << 8) | (uint32_t(d.U8(3)) << 16);
  if (size == 0)
  {
    size = uint32_t(d.U8(4)) | (uint32_t(d.U8(5)) << 8) | (uint32_t(d.U8(6)) << 16) |
           (uint32_t(d.U8(7)) << 24);
    p = 8;
  }
  if (size > 64u * 1024 * 1024)
    return {};
  QByteArray out;
  out.reserve(size);
  while (static_cast<uint32_t>(out.size()) < size && p < d.size)
  {
    const uint8_t flags = d.U8(p++);
    for (int i = 0; i < 8 && static_cast<uint32_t>(out.size()) < size; ++i)
    {
      if (flags & (0x80 >> i))
      {
        int n, disp;
        if (type == 0x10)
        {
          const int b = (d.U8(p) << 8) | d.U8(p + 1);
          p += 2;
          n = (b >> 12) + 3;
          disp = (b & 0xFFF) + 1;
        }
        else
        {
          const int b = d.U8(p);
          const int ind = b >> 4;
          if (ind == 0)
          {
            n = (((b & 0xF) << 4) | (d.U8(p + 1) >> 4)) + 0x11;
            disp = (((d.U8(p + 1) & 0xF) << 8) | d.U8(p + 2)) + 1;
            p += 3;
          }
          else if (ind == 1)
          {
            n = (((b & 0xF) << 12) | (d.U8(p + 1) << 4) | (d.U8(p + 2) >> 4)) + 0x111;
            disp = (((d.U8(p + 2) & 0xF) << 8) | d.U8(p + 3)) + 1;
            p += 4;
          }
          else
          {
            n = ind + 1;
            disp = (((b & 0xF) << 8) | d.U8(p + 1)) + 1;
            p += 2;
          }
        }
        if (disp > out.size())
          return {};
        for (int j = 0; j < n; ++j)
          out.append(out.at(out.size() - disp));
      }
      else
      {
        out.append(char(d.U8(p++)));
      }
    }
  }
  return out;
}

std::map<std::string, QByteArray> ReadU8(const QByteArray& data)
{
  std::map<std::string, QByteArray> files;
  const Span d = SpanOf(data);
  if (!data.startsWith("U\xAA" "8-"))
    return files;
  const uint32_t root = d.U32(4);
  const uint32_t count = d.U32(root + 8);
  if (count == 0 || count > 100000)
    return files;
  const qsizetype strings = root + qsizetype(count) * 12;
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};
  for (uint32_t i = 1; i < count; ++i)
  {
    const qsizetype e = root + qsizetype(i) * 12;
    const uint8_t type = d.U8(e);
    const uint32_t name_off = d.U32(e) & 0xFFFFFF;
    const uint32_t a = d.U32(e + 4), b = d.U32(e + 8);
    const std::string name = d.Str(strings + name_off, 256);
    while (dirs.size() > 1 && i >= dirs.back().first)
      dirs.pop_back();
    const std::string prefix = dirs.back().second;
    if (type == 1)
      dirs.emplace_back(b, prefix + name + "/");
    else if (d.Has(a, b))
      files[prefix + name] = data.mid(a, b);
  }
  return files;
}

// ------------------------------------------------------------------ textures

namespace
{
QRgb RGB5A3(uint16_t v)
{
  if (v & 0x8000)
  {
    const int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    return qRgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
  }
  const int a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
  return qRgba(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}
QRgb RGB565(uint16_t v)
{
  const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return qRgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
QRgb IA8(uint16_t v)
{
  const int a = v >> 8, i = v & 0xFF;
  return qRgba(i, i, i, a);
}

QImage DecodeImage(Span d, int w, int h, int fmt, const std::vector<QRgb>& palette)
{
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return {};
  QImage img(w, h, QImage::Format_ARGB32);
  img.fill(Qt::transparent);
  const auto put = [&](int x, int y, QRgb c) {
    if (x < w && y < h)
      reinterpret_cast<QRgb*>(img.scanLine(y))[x] = c;
  };
  const auto pal = [&](int i) -> QRgb {
    return i >= 0 && i < int(palette.size()) ? palette[i] : qRgba(0, 0, 0, 0);
  };
  int bw, bh;
  switch (fmt)
  {
  case 0:  // I4
  case 8:  // C4
  case 14:  // CMPR
    bw = 8, bh = 8;
    break;
  case 1:  // I8
  case 2:  // IA4
  case 9:  // C8
    bw = 8, bh = 4;
    break;
  default:
    bw = 4, bh = 4;
    break;
  }
  qsizetype p = 0;
  for (int by = 0; by < h; by += bh)
  {
    for (int bx = 0; bx < w; bx += bw)
    {
      switch (fmt)
      {
      case 0:
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; x += 2)
          {
            const uint8_t v = d.U8(p++);
            const int i0 = (v >> 4) * 17, i1 = (v & 15) * 17;
            put(bx + x, by + y, qRgba(i0, i0, i0, i0));
            put(bx + x + 1, by + y, qRgba(i1, i1, i1, i1));
          }
        break;
      case 1:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 8; ++x)
          {
            const int i = d.U8(p++);
            put(bx + x, by + y, qRgba(i, i, i, i));
          }
        break;
      case 2:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 8; ++x)
          {
            const uint8_t v = d.U8(p++);
            const int a = (v >> 4) * 17, i = (v & 15) * 17;
            put(bx + x, by + y, qRgba(i, i, i, a));
          }
        break;
      case 3:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x, p += 2)
            put(bx + x, by + y, IA8(d.U16(p)));
        break;
      case 4:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x, p += 2)
            put(bx + x, by + y, RGB565(d.U16(p)));
        break;
      case 5:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x, p += 2)
            put(bx + x, by + y, RGB5A3(d.U16(p)));
        break;
      case 6:
        for (int i = 0; i < 16; ++i)
        {
          const int x = i % 4, y = i / 4;
          const int a = d.U8(p + i * 2), r = d.U8(p + i * 2 + 1);
          const int g = d.U8(p + 32 + i * 2), b = d.U8(p + 32 + i * 2 + 1);
          put(bx + x, by + y, qRgba(r, g, b, a));
        }
        p += 64;
        break;
      case 8:
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; x += 2)
          {
            const uint8_t v = d.U8(p++);
            put(bx + x, by + y, pal(v >> 4));
            put(bx + x + 1, by + y, pal(v & 15));
          }
        break;
      case 9:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 8; ++x)
            put(bx + x, by + y, pal(d.U8(p++)));
        break;
      case 10:
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x, p += 2)
            put(bx + x, by + y, pal(d.U16(p) & 0x3FFF));
        break;
      case 14:
        for (int sb = 0; sb < 4; ++sb)
        {
          const int sx = bx + (sb % 2) * 4, sy = by + (sb / 2) * 4;
          const uint16_t c0 = d.U16(p), c1 = d.U16(p + 2);
          const uint32_t bits = d.U32(p + 4);
          p += 8;
          QRgb col[4];
          col[0] = RGB565(c0);
          col[1] = RGB565(c1);
          const auto mix = [](QRgb a, QRgb b, int wa, int wb, int div) {
            return qRgba((qRed(a) * wa + qRed(b) * wb) / div, (qGreen(a) * wa + qGreen(b) * wb) / div,
                         (qBlue(a) * wa + qBlue(b) * wb) / div, 255);
          };
          if (c0 > c1)
          {
            col[2] = mix(col[0], col[1], 2, 1, 3);
            col[3] = mix(col[0], col[1], 1, 2, 3);
          }
          else
          {
            col[2] = mix(col[0], col[1], 1, 1, 2);
            col[3] = qRgba(0, 0, 0, 0);
          }
          for (int i = 0; i < 16; ++i)
            put(sx + i % 4, sy + i / 4, col[(bits >> (30 - i * 2)) & 3]);
        }
        break;
      default:
        return {};
      }
    }
  }
  return img;
}
}  // namespace

QImage DecodeTPL(const QByteArray& data)
{
  const Span d = SpanOf(data);
  if (d.U32(0) != 0x0020AF30 || d.U32(4) == 0)
    return {};
  const uint32_t table = d.U32(8);
  const uint32_t ih = d.U32(table), ph = d.U32(table + 4);
  const int h = d.U16(ih), w = d.U16(ih + 2);
  const int fmt = int(d.U32(ih + 4));
  const uint32_t off = d.U32(ih + 8);
  std::vector<QRgb> palette;
  if (ph)
  {
    const int n = d.U16(ph);
    const uint32_t pfmt = d.U32(ph + 4), poff = d.U32(ph + 8);
    for (int i = 0; i < n; ++i)
    {
      const uint16_t v = d.U16(poff + i * 2);
      palette.push_back(pfmt == 0 ? IA8(v) : pfmt == 1 ? RGB565(v) : RGB5A3(v));
    }
  }
  return DecodeImage(d.Sub(off), w, h, fmt, palette);
}

// ------------------------------------------------------------------ sound

namespace
{
int16_t Clamp16(int v)
{
  return static_cast<int16_t>(std::clamp(v, -32768, 32767));
}

std::vector<int16_t> DecodeDSPADPCM(Span d, size_t samples, const int16_t coef[16])
{
  std::vector<int16_t> out;
  out.reserve(samples);
  int hist1 = 0, hist2 = 0;
  qsizetype p = 0;
  while (out.size() < samples && d.Has(p, 1))
  {
    const uint8_t header = d.U8(p++);
    const int scale = 1 << (header & 15);
    const int idx = (header >> 4) & 7;
    const int c1 = coef[idx * 2], c2 = coef[idx * 2 + 1];
    for (int i = 0; i < 14 && out.size() < samples; ++i)
    {
      const uint8_t byte = d.U8(p + i / 2);
      int nib = (i & 1) ? (byte & 15) : (byte >> 4);
      if (nib >= 8)
        nib -= 16;
      const int s = ((nib * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2;
      const int16_t v = Clamp16(s >> 11);
      hist2 = hist1;
      hist1 = v;
      out.push_back(v);
    }
    p += 7;
  }
  return out;
}

Sound DecodeWAV(Span d)
{
  Sound s;
  int channels = 0, bits = 0;
  qsizetype p = 12;
  while (d.Has(p, 8))
  {
    const std::string id(d.data + p, 4);
    const uint32_t len = uint32_t(uint8_t(d.data[p + 4])) | (uint32_t(uint8_t(d.data[p + 5])) << 8) |
                         (uint32_t(uint8_t(d.data[p + 6])) << 16) |
                         (uint32_t(uint8_t(d.data[p + 7])) << 24);
    if (id == "fmt ")
    {
      channels = uint8_t(d.data[p + 10]);
      s.rate = int(uint8_t(d.data[p + 12]) | (uint8_t(d.data[p + 13]) << 8) |
                   (uint8_t(d.data[p + 14]) << 16));
      bits = uint8_t(d.data[p + 22]);
    }
    else if (id == "data" && bits == 16 && channels > 0)
    {
      const qsizetype n = std::min<qsizetype>(len, d.size - p - 8) / 2;
      const int16_t* src = reinterpret_cast<const int16_t*>(d.data + p + 8);
      for (qsizetype i = 0; i < n; i += channels)
      {
        s.samples.push_back(src[i]);
        s.samples.push_back(channels > 1 ? src[i + 1] : src[i]);
      }
    }
    p += 8 + len + (len & 1);
  }
  return s;
}
}  // namespace

Sound DecodeBNS(const QByteArray& data)
{
  const Span d = SpanOf(data);
  if (data.startsWith("RIFF"))
    return DecodeWAV(d);
  Sound s;
  if (!data.startsWith("BNS "))
    return s;
  const uint32_t info = d.U32(0x10);
  const uint32_t data_off = d.U32(0x18);
  const Span in = d.Sub(info);
  const int codec = in.U8(8);
  s.loop = in.U8(9) != 0;
  const int channels = std::clamp<int>(in.U8(10), 1, 2);
  s.rate = in.U16(12);
  s.loop_start = in.U32(16);
  const uint32_t total = in.U32(20);
  const uint32_t ch_list = in.U32(24);
  if (codec != 0 || total == 0 || total > 48000 * 600)
    return {};
  std::vector<std::vector<int16_t>> chans;
  for (int c = 0; c < channels; ++c)
  {
    const uint32_t ci = in.U32(8 + ch_list + c * 4) + 8;
    const uint32_t dat = in.U32(ci);
    const uint32_t ad = in.U32(ci + 4) + 8;
    int16_t coef[16];
    for (int k = 0; k < 16; ++k)
      coef[k] = static_cast<int16_t>(in.U16(ad + k * 2));
    chans.push_back(DecodeDSPADPCM(d.Sub(data_off + 8 + dat), total, coef));
  }
  const size_t n = chans[0].size();
  s.samples.resize(n * 2);
  for (size_t i = 0; i < n; ++i)
  {
    s.samples[i * 2] = chans[0][i];
    s.samples[i * 2 + 1] = channels > 1 && i < chans[1].size() ? chans[1][i] : chans[0][i];
  }
  if (s.loop_start >= n)
    s.loop_start = 0;
  return s;
}

// ------------------------------------------------------------------ layout

namespace
{
struct Color4
{
  float r = 1, g = 1, b = 1, a = 1;
  float& operator[](int i) { return i == 0 ? r : i == 1 ? g : i == 2 ? b : a; }
  float operator[](int i) const { return i == 0 ? r : i == 1 ? g : i == 2 ? b : a; }
};

struct TexMap
{
  int texture = 0;
  int wrap_s = 0, wrap_t = 0;
};
struct TexSRT
{
  float tx = 0, ty = 0, rot = 0, sx = 1, sy = 1;
};
struct TevStage
{
  uint8_t tcg, chan, map, swap;
  uint8_t c_ab, c_cd, c_op, c_cl;
  uint8_t a_ab, a_cd, a_op, a_cl;
};

struct Material
{
  std::string name;
  std::array<Color4, 3> regs;  // TEV registers 0..2 ("black", "white", 2)
  std::array<Color4, 4> konst;
  std::vector<TexMap> maps;
  std::vector<TexSRT> srts;
  std::vector<int> tcg_mtx;  // texcoord gen -> SRT index (-1: none)
  bool chan_ctrl = false;
  int color_src = 1, alpha_src = 1;  // 0 register, 1 vertex
  Color4 mat_color;
  std::array<std::array<int, 4>, 4> swaps{{{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}}};
  std::vector<TevStage> stages;
  int blend_src = 4, blend_dst = 5;  // src alpha / inv src alpha
  bool blend = true;
};

enum class PaneType
{
  Null,
  Picture,
  Text,
  Window,
  Bounding,
};

struct Pane
{
  PaneType type = PaneType::Null;
  std::string name;
  bool visible = true;
  bool influenced_alpha = false;
  int origin = 4;
  float alpha = 255;
  float t[3]{}, rot[3]{}, scale[2]{1, 1}, size[2]{};
  std::array<Color4, 4> vtx;  // TL, TR, BL, BR
  int material = -1;
  std::array<std::array<float, 2>, 4> uv{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
  std::vector<int> children;
  int parent = -1;
};

// ------------------------------------------------------------------ animation

struct Key
{
  float frame, value, slope;
};
struct Track
{
  int target_index = 0;  // material: texmap / SRT index
  int target = 0;
  bool step = false;
  std::vector<Key> keys;

  float Eval(float f) const
  {
    if (keys.empty())
      return 0;
    if (f <= keys.front().frame)
      return keys.front().value;
    if (f >= keys.back().frame)
      return keys.back().value;
    size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].frame <= f)
      ++i;
    const Key& a = keys[i];
    const Key& b = keys[std::min(i + 1, keys.size() - 1)];
    if (step || b.frame <= a.frame)
      return a.value;
    const float dt = b.frame - a.frame;
    const float u = (f - a.frame) / dt;
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u;
    const float h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    return h00 * a.value + h10 * a.slope * dt + h01 * b.value + h11 * b.slope * dt;
  }
};
struct AnimTag
{
  std::string kind;  // RLPA, RLTS, RLVI, RLVC, RLMC, RLTP
  std::vector<Track> tracks;
};
struct AnimEntry
{
  std::string name;
  bool material = false;
  std::vector<AnimTag> tags;
};
struct Animation
{
  int frames = 0;
  bool loop = false;
  std::vector<std::string> textures;  // for RLTP
  std::vector<AnimEntry> entries;
};

Animation ParseBRLAN(const QByteArray& data)
{
  Animation anim;
  const Span d = SpanOf(data);
  if (!data.startsWith("RLAN"))
    return anim;
  qsizetype p = d.U16(0xC);
  const int sections = d.U16(0xE);
  for (int si = 0; si < sections && d.Has(p, 8); ++si)
  {
    const Span s = d.Sub(p, d.U32(p + 4));
    if (std::string(s.data, 4) == "pai1")
    {
      anim.frames = s.U16(8);
      anim.loop = s.U8(10) != 0;
      const int ntex = s.U16(12), nent = s.U16(14);
      const uint32_t eo = s.U32(16);
      for (int i = 0; i < ntex; ++i)
        anim.textures.push_back(s.Str(0x14 + s.U32(0x14 + i * 4), 128));
      for (int i = 0; i < nent; ++i)
      {
        const Span e = s.Sub(s.U32(eo + i * 4));
        AnimEntry entry;
        entry.name = e.Str(0, 20);
        const int ntags = e.U8(20);
        entry.material = e.U8(21) != 0;
        for (int t = 0; t < ntags; ++t)
        {
          const Span tg = e.Sub(e.U32(24 + t * 4));
          AnimTag tag;
          tag.kind = std::string(tg.data, std::min<qsizetype>(4, tg.size));
          const int n = tg.U8(4);
          for (int k = 0; k < n; ++k)
          {
            const Span te = tg.Sub(tg.U32(8 + k * 4));
            Track tr;
            tr.target_index = te.U8(0);
            tr.target = te.U8(1);
            const int type = te.U8(2);
            const int nkeys = te.U16(4);
            const Span keys = te.Sub(te.U32(8));
            tr.step = type == 1;
            for (int j = 0; j < nkeys; ++j)
            {
              if (type == 1)
                tr.keys.push_back({keys.F(j * 8), float(keys.U16(j * 8 + 4)), 0});
              else
                tr.keys.push_back({keys.F(j * 12), keys.F(j * 12 + 4), keys.F(j * 12 + 8)});
            }
            tag.tracks.push_back(std::move(tr));
          }
          entry.tags.push_back(std::move(tag));
        }
        anim.entries.push_back(std::move(entry));
      }
    }
    p += s.size;
  }
  return anim;
}

Color4 ColorU8(uint32_t rgba)
{
  return {float((rgba >> 24) & 255) / 255, float((rgba >> 16) & 255) / 255,
          float((rgba >> 8) & 255) / 255, float(rgba & 255) / 255};
}
}  // namespace

// ------------------------------------------------------------------ the banner

struct Banner::Impl
{
  float width = 608, height = 456;
  std::vector<std::string> texture_names;
  std::unordered_map<std::string, QImage> images;  // by file name
  std::vector<QImage> textures;                    // by txl1 index
  std::vector<Material> materials;
  std::vector<Pane> panes;  // panes[0] = root
  Animation start, loop;
  bool has_start = false;
  double frame = 0;
  bool in_loop = false;
  std::unordered_map<std::string, int> pane_by_name, mat_by_name;

  struct Cache
  {
    size_t key = 0;
    QImage image;
    QRectF uv;  // area of texcoord space covered by `image`
  };
  std::unordered_map<int, Cache> cache;  // per pane

  bool ParseBRLYT(const QByteArray& data);
  void Apply(const Animation& anim, float f);
  void DrawPane(QPainter& p, int index, const QTransform& parent, float parent_alpha);
  QImage RenderMaterial(const Pane& pane, const Material& mat, const QRectF& uv, int w, int h);
};

bool Banner::Impl::ParseBRLYT(const QByteArray& data)
{
  const Span d = SpanOf(data);
  if (!data.startsWith("RLYT"))
    return false;
  qsizetype p = d.U16(0xC);
  const int sections = d.U16(0xE);
  std::vector<int> stack;  // parents
  int last = -1;
  for (int si = 0; si < sections && d.Has(p, 8); ++si)
  {
    const Span s = d.Sub(p, d.U32(p + 4));
    const std::string magic(s.data, 4);
    if (magic == "lyt1")
    {
      width = s.F(12);
      height = s.F(16);
    }
    else if (magic == "txl1")
    {
      const int n = s.U16(8);
      for (int i = 0; i < n; ++i)
        texture_names.push_back(s.Str(12 + s.U32(12 + i * 8), 128));
    }
    else if (magic == "mat1")
    {
      const int n = s.U16(8);
      for (int i = 0; i < n; ++i)
      {
        const Span m = s.Sub(s.U32(12 + i * 4));
        Material mat;
        mat.name = m.Str(0, 20);
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 4; ++c)
            mat.regs[r][c] = std::clamp<float>(int16_t(m.U16(20 + (r * 4 + c) * 2)), -1024, 1023) / 255.0f;
        for (int k = 0; k < 4; ++k)
          mat.konst[k] = ColorU8(m.U32(44 + k * 4));
        const uint32_t fl = m.U32(60);
        const int ntm = fl & 15, nsrt = (fl >> 4) & 15, ntcg = (fl >> 8) & 15;
        const bool swap = (fl >> 12) & 1;
        const int nisrt = (fl >> 13) & 3, nist = (fl >> 15) & 7, ntev = (fl >> 18) & 31;
        const bool acmp = (fl >> 23) & 1, bmode = (fl >> 24) & 1, cc = (fl >> 25) & 1,
                   mcol = (fl >> 27) & 1;
        qsizetype q = 64;
        for (int k = 0; k < ntm; ++k, q += 4)
          mat.maps.push_back({m.U16(q), m.U8(q + 2), m.U8(q + 3)});
        for (int k = 0; k < nsrt; ++k, q += 20)
          mat.srts.push_back({m.F(q), m.F(q + 4), m.F(q + 8), m.F(q + 12), m.F(q + 16)});
        for (int k = 0; k < ntcg; ++k, q += 4)
        {
          const int mtx = m.U8(q + 2);
          mat.tcg_mtx.push_back(mtx >= 30 && mtx < 60 ? (mtx - 30) / 3 : -1);
        }
        if (cc)
        {
          mat.chan_ctrl = true;
          mat.color_src = m.U8(q);
          mat.alpha_src = m.U8(q + 1);
          q += 4;
        }
        if (mcol)
        {
          mat.mat_color = ColorU8(m.U32(q));
          q += 4;
        }
        if (swap)
        {
          for (int k = 0; k < 4; ++k)
          {
            const uint8_t v = m.U8(q + k);
            mat.swaps[k] = {v & 3, (v >> 2) & 3, (v >> 4) & 3, (v >> 6) & 3};
          }
          q += 4;
        }
        q += nisrt * 20 + nist * 4;
        for (int k = 0; k < ntev; ++k, q += 16)
        {
          TevStage st;
          std::memcpy(&st, m.data + q, std::min<qsizetype>(12, m.size - q));
          mat.stages.push_back(st);
        }
        if (acmp)
          q += 4;
        if (bmode)
        {
          const int type = m.U8(q);
          mat.blend = type == 1;
          mat.blend_src = m.U8(q + 1);
          mat.blend_dst = m.U8(q + 2);
          q += 4;
        }
        materials.push_back(std::move(mat));
      }
    }
    else if (magic == "pan1" || magic == "pic1" || magic == "txt1" || magic == "wnd1" ||
             magic == "bnd1")
    {
      Pane pane;
      pane.type = magic == "pic1" ? PaneType::Picture :
                  magic == "txt1" ? PaneType::Text :
                  magic == "wnd1" ? PaneType::Window :
                  magic == "bnd1" ? PaneType::Bounding :
                                    PaneType::Null;
      const uint8_t fl = s.U8(8);
      pane.visible = fl & 1;
      pane.influenced_alpha = fl & 2;
      pane.origin = s.U8(9);
      pane.alpha = s.U8(10);
      pane.name = s.Str(12, 16);
      for (int k = 0; k < 3; ++k)
      {
        pane.t[k] = s.F(36 + k * 4);
        pane.rot[k] = s.F(48 + k * 4);
      }
      pane.scale[0] = s.F(60);
      pane.scale[1] = s.F(64);
      pane.size[0] = s.F(68);
      pane.size[1] = s.F(72);
      if (pane.type == PaneType::Picture)
      {
        for (int k = 0; k < 4; ++k)
          pane.vtx[k] = ColorU8(s.U32(76 + k * 4));
        pane.material = s.U16(92);
        if (s.U8(94) > 0)
          for (int k = 0; k < 4; ++k)
            pane.uv[k] = {s.F(96 + k * 8), s.F(100 + k * 8)};
      }
      else if (pane.type == PaneType::Window)
      {
        // Content area only (the frames are drawn by few banners)
        const uint32_t content = s.U32(84);
        const Span c = s.Sub(content);
        for (int k = 0; k < 4; ++k)
          pane.vtx[k] = ColorU8(c.U32(k * 4));
        pane.material = c.U16(16);
        if (c.U8(18) > 0)
          for (int k = 0; k < 4; ++k)
            pane.uv[k] = {c.F(20 + k * 8), c.F(24 + k * 8)};
      }
      const int index = static_cast<int>(panes.size());
      pane.parent = stack.empty() ? -1 : stack.back();
      if (pane.parent >= 0)
        panes[pane.parent].children.push_back(index);
      pane_by_name[pane.name] = index;
      panes.push_back(std::move(pane));
      last = index;
    }
    else if (magic == "pas1")
    {
      if (last >= 0)
        stack.push_back(last);
    }
    else if (magic == "pae1")
    {
      if (!stack.empty())
      {
        last = stack.back();
        stack.pop_back();
      }
    }
    p += s.size;
  }
  for (size_t i = 0; i < materials.size(); ++i)
    mat_by_name[materials[i].name] = static_cast<int>(i);
  return !panes.empty();
}

void Banner::Impl::Apply(const Animation& anim, float f)
{
  for (const AnimEntry& e : anim.entries)
  {
    if (e.material)
    {
      const auto it = mat_by_name.find(e.name);
      if (it == mat_by_name.end())
        continue;
      Material& m = materials[it->second];
      for (const AnimTag& tag : e.tags)
      {
        for (const Track& tr : tag.tracks)
        {
          const float v = tr.Eval(f);
          if (tag.kind == "RLMC")
          {
            const int c = tr.target & 3;
            if (tr.target < 4)
              m.mat_color[c] = v / 255.0f;
            else if (tr.target < 16)
              m.regs[(tr.target - 4) / 4][c] = v / 255.0f;
            else if (tr.target < 32)
              m.konst[(tr.target - 16) / 4][c] = v / 255.0f;
          }
          else if (tag.kind == "RLTS" && tr.target_index < int(m.srts.size()))
          {
            TexSRT& srt = m.srts[tr.target_index];
            float* fields[] = {&srt.tx, &srt.ty, &srt.rot, &srt.sx, &srt.sy};
            if (tr.target < 5)
              *fields[tr.target] = v;
          }
          else if (tag.kind == "RLTP" && tr.target_index < int(m.maps.size()))
          {
            // Texture pattern: the animation's own texture list
            const int idx = int(std::lround(v));
            if (idx >= 0 && idx < int(anim.textures.size()))
            {
              const auto name = anim.textures[idx];
              const auto t = std::find(texture_names.begin(), texture_names.end(), name);
              if (t != texture_names.end())
                m.maps[tr.target_index].texture = int(t - texture_names.begin());
              else
              {
                texture_names.push_back(name);
                textures.push_back(images.count(name) ? images[name] : QImage());
                m.maps[tr.target_index].texture = int(texture_names.size()) - 1;
              }
            }
          }
        }
      }
    }
    else
    {
      const auto it = pane_by_name.find(e.name);
      if (it == pane_by_name.end())
        continue;
      Pane& pn = panes[it->second];
      for (const AnimTag& tag : e.tags)
      {
        for (const Track& tr : tag.tracks)
        {
          const float v = tr.Eval(f);
          if (tag.kind == "RLPA")
          {
            switch (tr.target)
            {
            case 0:
            case 1:
            case 2:
              pn.t[tr.target] = v;
              break;
            case 3:
            case 4:
            case 5:
              pn.rot[tr.target - 3] = v;
              break;
            case 6:
            case 7:
              pn.scale[tr.target - 6] = v;
              break;
            case 8:
            case 9:
              pn.size[tr.target - 8] = v;
              break;
            default:
              break;
            }
          }
          else if (tag.kind == "RLVI")
          {
            pn.visible = v >= 0.5f;
          }
          else if (tag.kind == "RLVC")
          {
            if (tr.target < 16)
              pn.vtx[tr.target / 4][tr.target % 4] = v / 255.0f;
            else if (tr.target == 16)
              pn.alpha = v;
          }
          else if (tag.kind == "RLMC" && pn.material >= 0 &&
                   pn.material < int(materials.size()))
          {
            // Some files put material colours on the pane entry
            Material& m = materials[pn.material];
            const int c = tr.target & 3;
            if (tr.target < 4)
              m.mat_color[c] = v / 255.0f;
            else if (tr.target < 16)
              m.regs[(tr.target - 4) / 4][c] = v / 255.0f;
          }
        }
      }
    }
  }
}

namespace
{
// ------------------------------------------------------------------ TEV (software)

struct V4
{
  float r, g, b, a;
  float operator[](int i) const { return i == 0 ? r : i == 1 ? g : i == 2 ? b : a; }
};

float Wrap(float v, int mode)
{
  if (mode == 1)
    return v - std::floor(v);
  if (mode == 2)
  {
    const float m = v - 2 * std::floor(v / 2);
    return m > 1 ? 2 - m : m;
  }
  return std::clamp(v, 0.0f, 1.0f);
}

V4 Sample(const QImage& img, float u, float v, int ws, int wt)
{
  if (img.isNull())
    return {1, 1, 1, 1};
  const int w = img.width(), h = img.height();
  // Bilinear
  const float x = Wrap(u, ws) * w - 0.5f, y = Wrap(v, wt) * h - 0.5f;
  int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - x0, fy = y - y0;
  const auto px = [&](int xi, int yi) {
    if (ws == 1)
      xi = ((xi % w) + w) % w;
    else
      xi = std::clamp(xi, 0, w - 1);
    if (wt == 1)
      yi = ((yi % h) + h) % h;
    else
      yi = std::clamp(yi, 0, h - 1);
    return reinterpret_cast<const QRgb*>(img.constScanLine(yi))[xi];
  };
  const QRgb c00 = px(x0, y0), c10 = px(x0 + 1, y0), c01 = px(x0, y0 + 1), c11 = px(x0 + 1, y0 + 1);
  const auto ch = [&](int (*get)(QRgb)) {
    const float top = get(c00) * (1 - fx) + get(c10) * fx;
    const float bot = get(c01) * (1 - fx) + get(c11) * fx;
    return (top * (1 - fy) + bot * fy) / 255.0f;
  };
  return {ch(qRed), ch(qGreen), ch(qBlue), ch(qAlpha)};
}

float KonstColor(int sel, const std::array<Color4, 4>& k, int comp)
{
  if (sel < 8)
    return (8 - sel) / 8.0f;
  if (sel >= 0x0C && sel <= 0x0F)
    return k[sel - 0x0C][comp];
  if (sel >= 0x10)
    return k[(sel - 0x10) % 4][(sel - 0x10) / 4];
  return 1;
}
float KonstAlpha(int sel, const std::array<Color4, 4>& k)
{
  if (sel < 8)
    return (8 - sel) / 8.0f;
  if (sel >= 0x10)
    return k[(sel - 0x10) % 4][(sel - 0x10) / 4];
  return 1;
}

float Bias(int b)
{
  return b == 1 ? 0.5f : b == 2 ? -0.5f : 0.0f;
}
float Scale(int s)
{
  return s == 1 ? 2.0f : s == 2 ? 4.0f : s == 3 ? 0.5f : 1.0f;
}
}  // namespace

QImage Banner::Impl::RenderMaterial(const Pane& pane, const Material& mat, const QRectF& uvr,
                                    int w, int h)
{
  QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
  const int nmaps = static_cast<int>(mat.maps.size());
  std::vector<const QImage*> tex(nmaps);
  for (int i = 0; i < nmaps; ++i)
  {
    const int t = mat.maps[i].texture;
    tex[i] = t >= 0 && t < int(textures.size()) && !textures[t].isNull() ? &textures[t] : nullptr;
  }
  // Texture matrices (NW4R: scale and rotation around the texture centre, then translation)
  struct Mtx
  {
    float a, b, c, d, e, f;
  };
  const auto srt_mtx = [&](int map) -> Mtx {
    int idx = map < int(mat.tcg_mtx.size()) ? mat.tcg_mtx[map] : map;
    if (idx < 0 || idx >= int(mat.srts.size()))
      return {1, 0, 0, 1, 0, 0};
    const TexSRT& s = mat.srts[idx];
    const float r = s.rot * float(std::numbers::pi) / 180.0f;
    const float cr = std::cos(r), sr = std::sin(r);
    const float a = s.sx * cr, b = -s.sy * sr, c = s.sx * sr, d = s.sy * cr;
    // t' = M (t - 0.5) + 0.5 + translation
    return {a, b, c, d, -0.5f * (a + b) + 0.5f + s.tx, -0.5f * (c + d) + 0.5f + s.ty};
  };
  // One matrix per texcoord generator
  const int ntcg = std::max<int>({nmaps, int(mat.tcg_mtx.size()), 1});
  std::vector<Mtx> mtx(ntcg);
  for (int i = 0; i < ntcg; ++i)
    mtx[i] = srt_mtx(i);

  const bool vtx_uniform = std::all_of(pane.vtx.begin(), pane.vtx.end(), [&](const Color4& c) {
    return c.r == pane.vtx[0].r && c.g == pane.vtx[0].g && c.b == pane.vtx[0].b &&
           c.a == pane.vtx[0].a;
  });

  // Default stages (no TEV setup in the file)
  std::vector<TevStage> stages = mat.stages;
  const bool use_default = stages.empty();

  for (int y = 0; y < h; ++y)
  {
    QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(y));
    const float fy = (y + 0.5f) / h;
    const float v = float(uvr.top() + fy * uvr.height());
    for (int x = 0; x < w; ++x)
    {
      const float fx = (x + 0.5f) / w;
      const float u = float(uvr.left() + fx * uvr.width());
      // Rasterised colour (vertex colours across the quad)
      Color4 vc = pane.vtx[0];
      if (!vtx_uniform)
      {
        for (int c = 0; c < 4; ++c)
        {
          const float top = pane.vtx[0][c] * (1 - fx) + pane.vtx[1][c] * fx;
          const float bot = pane.vtx[2][c] * (1 - fx) + pane.vtx[3][c] * fx;
          vc[c] = top * (1 - fy) + bot * fy;
        }
      }
      V4 ras;
      if (mat.chan_ctrl)
      {
        ras.r = mat.color_src == 1 ? vc.r : mat.mat_color.r;
        ras.g = mat.color_src == 1 ? vc.g : mat.mat_color.g;
        ras.b = mat.color_src == 1 ? vc.b : mat.mat_color.b;
        ras.a = mat.alpha_src == 1 ? vc.a : mat.mat_color.a;
      }
      else
      {
        ras = {vc.r * mat.mat_color.r, vc.g * mat.mat_color.g, vc.b * mat.mat_color.b,
               vc.a * mat.mat_color.a};
      }
      const auto sample = [&](int map, int tcg) -> V4 {
        if (map < 0 || map >= nmaps || !tex[map])
          return {1, 1, 1, 1};
        const Mtx& m = mtx[std::clamp(tcg, 0, ntcg - 1)];
        const float tu = m.a * u + m.b * v + m.e, tv = m.c * u + m.d * v + m.f;
        return Sample(*tex[map], tu, tv, mat.maps[map].wrap_s, mat.maps[map].wrap_t);
      };

      V4 res;
      if (use_default)
      {
        const Color4& c0 = mat.regs[0];
        const Color4& c1 = mat.regs[1];
        V4 t = nmaps > 0 ? sample(0, 0) : V4{1, 1, 1, 1};
        if (nmaps > 1)
        {
          const V4 t1 = sample(1, 1);
          t = {t.r + (t1.r - t.r) * t1.a, t.g + (t1.g - t.g) * t1.a, t.b + (t1.b - t.b) * t1.a,
               std::max(t.a, t1.a)};
        }
        if (nmaps > 0)
          res = {(c0.r + (c1.r - c0.r) * t.r) * ras.r, (c0.g + (c1.g - c0.g) * t.g) * ras.g,
                 (c0.b + (c1.b - c0.b) * t.b) * ras.b, (c0.a + (c1.a - c0.a) * t.a) * ras.a};
        else
          res = ras;
      }
      else
      {
        V4 regs[4] = {{0, 0, 0, 0},
                      {mat.regs[0].r, mat.regs[0].g, mat.regs[0].b, mat.regs[0].a},
                      {mat.regs[1].r, mat.regs[1].g, mat.regs[1].b, mat.regs[1].a},
                      {mat.regs[2].r, mat.regs[2].g, mat.regs[2].b, mat.regs[2].a}};
        for (const TevStage& st : stages)
        {
          const int map = st.map == 0xFF ? -1 : st.map;
          const auto& rsw = mat.swaps[(st.swap >> 1) & 3];
          const auto& tsw = mat.swaps[(st.swap >> 3) & 3];
          V4 t = map >= 0 ? sample(map, st.tcg) : V4{1, 1, 1, 1};
          {
            const float tc[4] = {t.r, t.g, t.b, t.a};
            t = {tc[tsw[0]], tc[tsw[1]], tc[tsw[2]], tc[tsw[3]]};
          }
          V4 r = st.chan == 0xFF ? V4{0, 0, 0, 0} : ras;
          {
            const float rc[4] = {r.r, r.g, r.b, r.a};
            r = {rc[rsw[0]], rc[rsw[1]], rc[rsw[2]], rc[rsw[3]]};
          }
          const int kc = st.c_cl >> 3, ka = st.a_cl >> 3;
          const auto cin = [&](int sel, int comp) -> float {
            switch (sel)
            {
            case 0: return regs[0][comp];
            case 1: return regs[0].a;
            case 2: return regs[1][comp];
            case 3: return regs[1].a;
            case 4: return regs[2][comp];
            case 5: return regs[2].a;
            case 6: return regs[3][comp];
            case 7: return regs[3].a;
            case 8: return comp == 0 ? t.r : comp == 1 ? t.g : t.b;
            case 9: return t.a;
            case 10: return comp == 0 ? r.r : comp == 1 ? r.g : r.b;
            case 11: return r.a;
            case 12: return 1.0f;
            case 13: return 0.5f;
            case 14: return KonstColor(kc, mat.konst, comp);
            default: return 0.0f;
            }
          };
          const auto ain = [&](int sel) -> float {
            switch (sel)
            {
            case 0: return regs[0].a;
            case 1: return regs[1].a;
            case 2: return regs[2].a;
            case 3: return regs[3].a;
            case 4: return t.a;
            case 5: return r.a;
            case 6: return KonstAlpha(ka, mat.konst);
            default: return 0.0f;
            }
          };
          const auto combine = [](float a, float b, float c, float d, int op, int bias,
                                  int scale) {
            if (op >= 8)  // comparisons, on the first component only
            {
              const bool eq = op & 1;
              const bool pass = eq ? std::abs(a - b) < 1.0f / 255 : a > b;
              return d + (pass ? c : 0.0f);
            }
            const float l = a * (1 - c) + b * c;
            float v2 = (op & 1) ? d - l : d + l;
            v2 = (v2 + Bias(bias)) * Scale(scale);
            return v2;
          };
          const int ca = st.c_ab & 15, cb = st.c_ab >> 4, cc = st.c_cd & 15, cd = st.c_cd >> 4;
          const int aa = st.a_ab & 15, ab = st.a_ab >> 4, ac = st.a_cd & 15, ad = st.a_cd >> 4;
          V4 o;
          for (int comp = 0; comp < 3; ++comp)
            (comp == 0 ? o.r : comp == 1 ? o.g : o.b) =
                combine(cin(ca, comp), cin(cb, comp), cin(cc, comp), cin(cd, comp), st.c_op & 15,
                        (st.c_op >> 4) & 3, st.c_op >> 6);
          o.a = combine(ain(aa), ain(ab), ain(ac), ain(ad), st.a_op & 15, (st.a_op >> 4) & 3,
                        st.a_op >> 6);
          const int creg = (st.c_cl >> 1) & 3, areg = (st.a_cl >> 1) & 3;
          regs[creg].r = std::clamp(o.r, 0.0f, 1.0f);
          regs[creg].g = std::clamp(o.g, 0.0f, 1.0f);
          regs[creg].b = std::clamp(o.b, 0.0f, 1.0f);
          regs[areg].a = std::clamp(o.a, 0.0f, 1.0f);
        }
        res = regs[0];
      }
      const float a = std::clamp(res.a, 0.0f, 1.0f);
      line[x] = qRgba(int(std::clamp(res.r, 0.0f, 1.0f) * a * 255 + 0.5f),
                      int(std::clamp(res.g, 0.0f, 1.0f) * a * 255 + 0.5f),
                      int(std::clamp(res.b, 0.0f, 1.0f) * a * 255 + 0.5f), int(a * 255 + 0.5f));
    }
  }
  return out;
}

namespace
{
// V4 component access for the TEV lambdas
}  // namespace

void Banner::Impl::DrawPane(QPainter& p, int index, const QTransform& parent, float parent_alpha)
{
  const Pane& pane = panes[index];
  if (!pane.visible)
    return;
  // Local matrix: translation, then rotations (X, Y, Z) projected orthographically, then scale.
  const float d2r = float(std::numbers::pi) / 180.0f;
  const float rx = pane.rot[0] * d2r, ry = pane.rot[1] * d2r, rz = pane.rot[2] * d2r;
  const float cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry);
  const float cz = std::cos(rz), sz = std::sin(rz);
  // R = Rz * Ry * Rx ; keep the x/y rows and columns
  const float m00 = cz * cy, m01 = cz * sy * sx - sz * cx;
  const float m10 = sz * cy, m11 = sz * sy * sx + cz * cx;
  const QTransform local(m00 * pane.scale[0], m10 * pane.scale[0], m01 * pane.scale[1],
                         m11 * pane.scale[1], pane.t[0], pane.t[1]);
  const QTransform world = local * parent;
  const float alpha = pane.alpha / 255.0f * (pane.influenced_alpha ? parent_alpha : 1.0f);

  if ((pane.type == PaneType::Picture || pane.type == PaneType::Window) && pane.material >= 0 &&
      pane.material < int(materials.size()) && alpha > 0.001f)
  {
    const Material& mat = materials[pane.material];
    const float w = pane.size[0], h = pane.size[1];
    const int hx = pane.origin % 3, vy = pane.origin / 3;
    const float left = -w * hx / 2.0f;
    const float top = h * vy / 2.0f;  // y up
    // Texcoord bounding box
    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    for (const auto& c : pane.uv)
    {
      u0 = std::min(u0, c[0]);
      u1 = std::max(u1, c[0]);
      v0 = std::min(v0, c[1]);
      v1 = std::max(v1, c[1]);
    }
    if (u1 - u0 < 1e-6f)
      u1 = u0 + 1;
    if (v1 - v0 < 1e-6f)
      v1 = v0 + 1;
    const QRectF uv(u0, v0, u1 - u0, v1 - v0);
    // Resolution: the first texture's, over the texcoord span (or the pane size without one)
    int tw = 64, th = 64;
    if (!mat.maps.empty())
    {
      const int t = mat.maps[0].texture;
      if (t >= 0 && t < int(textures.size()) && !textures[t].isNull())
      {
        tw = textures[t].width();
        th = textures[t].height();
      }
    }
    const int iw = std::clamp(int(std::ceil(tw * uv.width())), 1, 2048);
    const int ih = std::clamp(int(std::ceil(th * uv.height())), 1, 2048);
    // Cache: recomputed only when something the image depends on changes
    size_t key = std::hash<std::string>()(mat.name) ^ (size_t(iw) << 20) ^ size_t(ih);
    const auto mix = [&key](float f) {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      key ^= std::hash<uint32_t>()(u) + 0x9e3779b9 + (key << 6) + (key >> 2);
    };
    for (const auto& r : mat.regs)
      for (int c = 0; c < 4; ++c)
        mix(r[c]);
    for (const auto& k : mat.konst)
      for (int c = 0; c < 4; ++c)
        mix(k[c]);
    for (int c = 0; c < 4; ++c)
      mix(mat.mat_color[c]);
    for (const auto& s : mat.srts)
    {
      mix(s.tx), mix(s.ty), mix(s.rot), mix(s.sx), mix(s.sy);
    }
    for (const auto& m : mat.maps)
      mix(float(m.texture));
    for (const auto& vc : pane.vtx)
      for (int c = 0; c < 4; ++c)
        mix(vc[c]);
    Cache& cache_entry = cache[index];
    if (cache_entry.key != key || cache_entry.image.isNull())
    {
      cache_entry.key = key;
      cache_entry.image = RenderMaterial(pane, mat, uv, iw, ih);
      cache_entry.uv = uv;
    }

    // Image -> texcoords -> pane quad (affine, from the TL / TR / BL corners)
    const QPointF pTL(left, top), pTR(left + w, top), pBL(left, top - h);
    const auto& tTL = pane.uv[0];
    const auto& tTR = pane.uv[1];
    const auto& tBL = pane.uv[2];
    // Solve the affine map texcoord -> pane position
    const float du1 = tTR[0] - tTL[0], dv1 = tTR[1] - tTL[1];
    const float du2 = tBL[0] - tTL[0], dv2 = tBL[1] - tTL[1];
    const float det = du1 * dv2 - du2 * dv1;
    if (std::abs(det) > 1e-9f)
    {
      // pos = pTL + (pTR - pTL) * a + (pBL - pTL) * b where (u, v) = tTL + a*d1 + b*d2
      const QPointF ex = pTR - pTL, ey = pBL - pTL;
      // (a, b) = inv([d1 d2]) * (t - tTL)
      const float i00 = dv2 / det, i01 = -du2 / det, i10 = -dv1 / det, i11 = du1 / det;
      // texcoord -> pane
      const QTransform t2p(i00 * ex.x() + i10 * ey.x(), i00 * ex.y() + i10 * ey.y(),
                           i01 * ex.x() + i11 * ey.x(), i01 * ex.y() + i11 * ey.y(), 0, 0);
      const QPointF origin = pTL - t2p.map(QPointF(tTL[0], tTL[1]));
      const QTransform tex_to_pane(t2p.m11(), t2p.m12(), t2p.m21(), t2p.m22(), origin.x(),
                                   origin.y());
      const QTransform img_to_tex(uv.width() / iw, 0, 0, uv.height() / ih, uv.left(), uv.top());
      p.save();
      p.setTransform(img_to_tex * tex_to_pane * world, false);
      p.setOpacity(alpha);
      if (mat.blend && mat.blend_dst == 1)
        p.setCompositionMode(QPainter::CompositionMode_Plus);
      p.drawImage(QPointF(0, 0), cache_entry.image);
      p.restore();
    }
  }
  for (int child : pane.children)
    DrawPane(p, child, world, alpha);
}

// ------------------------------------------------------------------ public

Banner::Banner() : m_impl(std::make_unique<Impl>())
{
}
Banner::~Banner() = default;

std::unique_ptr<Banner> Banner::Load(const QByteArray& data)
{
  const qsizetype u8 = data.indexOf("U\xAA" "8-");
  if (u8 < 0)
    return nullptr;
  const auto files = ReadU8(data.mid(u8));
  const auto find = [&](const char* name) -> QByteArray {
    for (const auto& [k, v] : files)
      if (k.ends_with(name))
        return v;
    return {};
  };
  const QByteArray banner_bin = find("banner.bin");
  if (banner_bin.isEmpty())
    return nullptr;
  const auto arc = ReadU8(Unwrap(banner_bin));
  if (arc.empty())
    return nullptr;
  std::unique_ptr<Banner> b(new Banner());
  Impl& im = *b->m_impl;
  QByteArray brlyt, start, loop, single;
  for (const auto& [k, v] : arc)
  {
    const auto slash = k.rfind('/');
    const std::string base = slash == std::string::npos ? k : k.substr(slash + 1);
    if (base.ends_with(".tpl"))
      im.images[base] = DecodeTPL(v);
    else if (base.ends_with(".brlyt"))
      brlyt = v;
    else if (base.ends_with("_Start.brlan"))
      start = v;
    else if (base.ends_with("_Loop.brlan"))
      loop = v;
    else if (base.ends_with(".brlan"))
      single = v;
  }
  if (!im.ParseBRLYT(brlyt))
    return nullptr;
  for (const auto& name : im.texture_names)
    im.textures.push_back(im.images.count(name) ? im.images[name] : QImage());
  if (!start.isEmpty())
  {
    im.start = ParseBRLAN(start);
    im.has_start = im.start.frames > 0;
  }
  im.loop = ParseBRLAN(!loop.isEmpty() ? loop : single);
  b->m_size = QSizeF(im.width, im.height);
  b->Restart();

  const QByteArray sound = find("sound.bin");
  if (!sound.isEmpty())
    b->m_sound = DecodeBNS(Unwrap(sound));
  return b;
}

void Banner::Restart()
{
  m_impl->frame = 0;
  m_impl->in_loop = !m_impl->has_start;
  Advance(0);
}

void Banner::Advance(double frames)
{
  Impl& im = *m_impl;
  im.frame += frames;
  if (!im.in_loop && im.frame >= im.start.frames)
  {
    im.Apply(im.start, float(im.start.frames));
    im.frame -= im.start.frames;
    im.in_loop = true;
  }
  if (!im.in_loop)
  {
    im.Apply(im.start, float(im.frame));
    return;
  }
  if (im.loop.frames > 0)
  {
    if (im.frame >= im.loop.frames)
      im.frame = std::fmod(im.frame, double(im.loop.frames));
    im.Apply(im.loop, float(im.frame));
  }
}

void Banner::Render(QPainter& painter, const QRectF& target)
{
  Impl& im = *m_impl;
  if (im.panes.empty())
    return;
  const qreal s = target.height() / im.height;
  // Layout units: origin at the centre, y up
  const QTransform screen(s, 0, 0, -s, target.center().x(), target.center().y());
  painter.save();
  painter.setClipRect(target);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.setRenderHint(QPainter::Antialiasing, true);
  im.DrawPane(painter, 0, screen, 1.0f);
  painter.restore();
}
}  // namespace WiiBanner
