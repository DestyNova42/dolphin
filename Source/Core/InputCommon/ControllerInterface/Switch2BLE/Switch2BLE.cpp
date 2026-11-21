// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Switch2BLE/Switch2BLE.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <set>
#include <string>
#include <thread>
#include <vector>

// C++/WinRT event delegates trip C4265 (in the SDK headers).
#pragma warning(disable : 4265)
#include <winrt/base.h>
#include <winrt/windows.devices.bluetooth.advertisement.h>
#include <winrt/windows.devices.bluetooth.genericattributeprofile.h>
#include <winrt/windows.devices.bluetooth.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.storage.streams.h>
#pragma comment(lib, "windowsapp")

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::Switch2BLE
{
namespace BT = winrt::Windows::Devices::Bluetooth;
namespace Adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace Gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace Streams = winrt::Windows::Storage::Streams;

namespace
{
constexpr u16 NINTENDO_COMPANY_ID = 0x0553;

// GATT attributes (see ndeadly/switch2_controller_research, bluetooth_interface.md)
constexpr winrt::guid SERVICE_HID{0xab7de9be, 0x89fe, 0x49ad,
                                  {0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0}};
constexpr winrt::guid SERVICE_OTHER{0x00c5af5d, 0x1964, 0x4e30,
                                    {0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x80}};
constexpr winrt::guid CHAR_OTHER_WRITE{0x00c5af5d, 0x1964, 0x4e30,
                                       {0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x82}};
constexpr winrt::guid CHAR_RESPONSE{0xc765a961, 0xd9d8, 0x4d36,
                                    {0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a}};
constexpr winrt::guid CHAR_UNKNOWN_NOTIFY{0xd3bd69d2, 0x841c, 0x4241,
                                          {0xab, 0x15, 0xf8, 0x6f, 0x40, 0x6d, 0x2a, 0x80}};
constexpr winrt::guid DESC_REPORT_RATE{0x679d5510, 0x5a24, 0x4dee,
                                       {0x95, 0x57, 0x95, 0xdf, 0x80, 0x48, 0x6e, 0xcb}};

constexpr winrt::guid G(u32 a, u16 b, u16 c, u64 d)
{
  return winrt::guid{a, b, c,
                     {static_cast<u8>(d >> 56), static_cast<u8>(d >> 48), static_cast<u8>(d >> 40),
                      static_cast<u8>(d >> 32), static_cast<u8>(d >> 24), static_cast<u8>(d >> 16),
                      static_cast<u8>(d >> 8), static_cast<u8>(d)}};
}

enum class Kind
{
  GameCube,
  Pro,
  JoyConL,
  JoyConR,
};

// Per controller type: attributes of bluetooth_interface.md
struct KindInfo
{
  u16 product;
  winrt::guid input;      // handle 0x000E
  winrt::guid command;    // handle 0x0016 (rumble + command)
  winrt::guid response;   // handle 0x001E
  winrt::guid rumble;     // handle 0x0012 (GameCube only here)
  size_t command_prefix;  // bytes before the command header on 0x0016
  float stick_range;
  float c_stick_range;
};

const KindInfo& Info(Kind kind)
{
  static const KindInfo gc{0x2073,
                           G(0x8261cba1, 0x9435, 0x420c, 0x84d6f0c75a2c8e4dull),
                           G(0xaf95885e, 0x44b3, 0x4a24, 0x9cf0483cc129469aull),
                           G(0x46f6ad29, 0xcdaf, 0x4569, 0xa2fe339020b94604ull),
                           G(0x3f8fb670, 0xab25, 0x45bf, 0xb54038c72834d064ull),
                           5,
                           1225.0f,
                           1120.0f};
  static const KindInfo pro{0x2069,
                            G(0x7492866c, 0xec3e, 0x4619, 0x825832755ffcc0f8ull),
                            G(0x3dacbc7e, 0x6955, 0x40b5, 0x8eaf6f9809e8b379ull),
                            G(0x506d9f7d, 0x4278, 0x4e95, 0xa549326ba77657e0ull),
                            {},
                            33,
                            1300.0f,
                            1300.0f};
  static const KindInfo jcl{0x2067,
                            G(0xcc1bbbb5, 0x7354, 0x4d32, 0xa716a81cb241a32aull),
                            G(0xce49a830, 0xdced, 0x48ae, 0x931ec8cf88aadbeaull),
                            G(0x63a3810f, 0xaec7, 0x474b, 0x90103d52403cb996ull),
                            {},
                            17,
                            1300.0f,
                            1300.0f};
  static const KindInfo jcr{0x2066,
                            G(0xd5a9e01e, 0x2ffc, 0x4cca, 0xb20c8b67142bf442ull),
                            G(0x65a724b3, 0xf1e7, 0x4a61, 0x8078a342376b27ffull),
                            G(0x640ca58e, 0x0e88, 0x410c, 0xa7f3426faf2b690bull),
                            {},
                            17,
                            1300.0f,
                            1300.0f};
  switch (kind)
  {
  case Kind::Pro:
    return pro;
  case Kind::JoyConL:
    return jcl;
  case Kind::JoyConR:
    return jcr;
  default:
    return gc;
  }
}

std::optional<Kind> KindFromProduct(u16 product)
{
  for (Kind k : {Kind::GameCube, Kind::Pro, Kind::JoyConL, Kind::JoyConR})
  {
    if (Info(k).product == product)
      return k;
  }
  return std::nullopt;
}

Streams::IBuffer ToBuffer(const std::vector<u8>& bytes)
{
  Streams::DataWriter writer;
  writer.WriteBytes(winrt::array_view<const u8>(bytes.data(), bytes.data() + bytes.size()));
  return writer.DetachBuffer();
}

std::vector<u8> FromBuffer(const Streams::IBuffer& buffer)
{
  std::vector<u8> bytes(buffer.Length());
  Streams::DataReader::FromBuffer(buffer).ReadBytes(bytes);
  return bytes;
}

// One controller: Bluetooth objects + its latest state.
struct Connection
{
  Kind kind = Kind::GameCube;
  u64 address = 0;
  BT::BluetoothLEDevice device{nullptr};
  Gatt::GattSession session{nullptr};
  Gatt::GattCharacteristic input{nullptr};
  Gatt::GattCharacteristic rumble{nullptr};
  Gatt::GattCharacteristic command{nullptr};
  std::vector<Gatt::GattCharacteristic> notify_keepalive;
  winrt::event_token input_token{};
  winrt::event_token status_token{};

  std::atomic<bool> valid{true};
  std::atomic<u32> buttons{0};
  std::atomic<float> lx{0}, ly{0}, rx{0}, ry{0}, lt{0}, rt{0};
  std::atomic<float> battery{100};
  std::atomic<bool> rumble_on{false};

  bool calibrated = false;
  float lcx = 2048, lcy = 2048, rcx = 2048, rcy = 2048;
  float lt_zero = 30, rt_zero = 30, lt_max = 195, rt_max = 195;

  u8 rumble_tid = 0;
  bool rumble_sent_on = false;
  std::chrono::steady_clock::time_point last_rumble{};

  static float Axis(float v, float c, float range)
  {
    return std::clamp((v - c) / range, -1.0f, 1.0f);
  }

  void Parse(const std::vector<u8>& r)
  {
    if (r.size() < 14)
      return;
    const KindInfo& info = Info(kind);
    // Power info: [2:5] battery level 0-9
    battery.store(std::min(100.0f, ((r[1] >> 2) & 0x0F) * 100.0f / 9.0f), std::memory_order_relaxed);

    const bool joycon = kind == Kind::JoyConL || kind == Kind::JoyConR;
    if (joycon)
    {
      // 2 bytes of buttons, one stick at 0x05
      buttons.store(r[2] | (r[3] << 8), std::memory_order_relaxed);
      const float sx = static_cast<float>(r[5] | ((r[6] & 0x0F) << 8));
      const float sy = static_cast<float>((r[6] >> 4) | (r[7] << 4));
      if (!calibrated)
      {
        calibrated = true;
        if (std::abs(sx - 2048.0f) < 500.0f && std::abs(sy - 2048.0f) < 500.0f)
        {
          lcx = sx;
          lcy = sy;
        }
      }
      auto& ax = kind == Kind::JoyConL ? lx : rx;
      auto& ay = kind == Kind::JoyConL ? ly : ry;
      ax.store(Axis(sx, lcx, info.stick_range), std::memory_order_relaxed);
      ay.store(Axis(sy, lcy, info.stick_range), std::memory_order_relaxed);
      return;
    }

    // GameCube and Pro: 3 bytes of buttons, two sticks
    buttons.store(r[2] | (r[3] << 8) | (r[4] << 16), std::memory_order_relaxed);
    const float rlx = static_cast<float>(r[5] | ((r[6] & 0x0F) << 8));
    const float rly = static_cast<float>((r[6] >> 4) | (r[7] << 4));
    const float rrx = static_cast<float>(r[8] | ((r[9] & 0x0F) << 8));
    const float rry = static_cast<float>((r[9] >> 4) | (r[10] << 4));
    const float tl = r[12], tr = r[13];
    if (!calibrated)
    {
      calibrated = true;
      const auto near_centre = [](float v) { return std::abs(v - 2048.0f) < 500.0f; };
      if (near_centre(rlx) && near_centre(rly))
      {
        lcx = rlx;
        lcy = rly;
      }
      if (near_centre(rrx) && near_centre(rry))
      {
        rcx = rrx;
        rcy = rry;
      }
      lt_zero = std::min(tl, 60.0f);
      rt_zero = std::min(tr, 60.0f);
    }
    lx.store(Axis(rlx, lcx, info.stick_range), std::memory_order_relaxed);
    ly.store(Axis(rly, lcy, info.stick_range), std::memory_order_relaxed);
    rx.store(Axis(rrx, rcx, info.c_stick_range), std::memory_order_relaxed);
    ry.store(Axis(rry, rcy, info.c_stick_range), std::memory_order_relaxed);
    if (kind == Kind::GameCube)
    {
      lt_max = std::max(lt_max, tl);
      rt_max = std::max(rt_max, tr);
      lt.store(std::clamp((tl - lt_zero) / (lt_max - lt_zero), 0.0f, 1.0f), std::memory_order_relaxed);
      rt.store(std::clamp((tr - rt_zero) / (rt_max - rt_zero), 0.0f, 1.0f), std::memory_order_relaxed);
    }
  }

  void SendRumble()
  {
    if (!rumble)
      return;
    const bool on = rumble_on.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    // A packet is a short pulse: keep sending while on.
    if (on == rumble_sent_on && (!on || now - last_rumble < std::chrono::milliseconds(30)))
      return;
    rumble_sent_on = on;
    last_rumble = now;
    std::vector<u8> packet(42, 0);
    packet[1] = static_cast<u8>(0x50 | (rumble_tid++ & 0x0F));
    packet[2] = on ? 1 : 0;
    try
    {
      (void)rumble.WriteValueAsync(ToBuffer(packet), Gatt::GattWriteOption::WriteWithoutResponse);
    }
    catch (const winrt::hresult_error&)
    {
    }
  }

  void Close()
  {
    valid = false;
    try
    {
      if (input)
        input.ValueChanged(input_token);
      if (device)
        device.ConnectionStatusChanged(status_token);
      if (session)
        session.Close();
      if (device)
        device.Close();
    }
    catch (const winrt::hresult_error&)
    {
    }
    input = nullptr;
    rumble = nullptr;
    command = nullptr;
    notify_keepalive.clear();
    session = nullptr;
    device = nullptr;
  }
};

// ------------------------------------------------------------------ Dolphin device

class Device final : public Core::Device
{
public:
  // A Joy-Con pair is one device made of two connections.
  explicit Device(std::shared_ptr<Connection> a, std::shared_ptr<Connection> b = nullptr)
  {
    for (auto& c : {a, b})
    {
      if (c)
        m_conns.push_back(c);
    }
    const Kind kind = m_conns.front()->kind;
    if (kind == Kind::GameCube)
      BuildGameCube(*m_conns.front());
    else if (kind == Kind::Pro)
      BuildPro(*m_conns.front());
    else
    {
      for (auto& c : m_conns)
        BuildJoyCon(*c);
    }
    AddInput(new Battery(*this));
  }

  std::string GetName() const override
  {
    switch (m_conns.front()->kind)
    {
    case Kind::GameCube:
      return "Nintendo GameCube Controller";
    case Kind::Pro:
      return "Nintendo Switch 2 Pro Controller";
    default:
      if (m_conns.size() == 2)
        return "Joy-Con 2";
      return m_conns.front()->kind == Kind::JoyConL ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
    }
  }
  std::string GetSource() const override { return "Switch2"; }
  int GetSortPriority() const override { return 2; }

  bool Contains(const Connection* c) const
  {
    return std::any_of(m_conns.begin(), m_conns.end(),
                       [c](const auto& own) { return own.get() == c; });
  }

  Core::DeviceRemoval UpdateInput() override
  {
    if (!IsValid())
      return Core::DeviceRemoval::Remove;
    for (auto& c : m_conns)
      c->SendRumble();
    return Core::DeviceRemoval::Keep;
  }

  bool IsValid() const override
  {
    return std::any_of(m_conns.begin(), m_conns.end(), [](const auto& c) { return c->valid.load(); });
  }

  float BatteryLevel() const
  {
    float level = 100;
    for (const auto& c : m_conns)
    {
      if (c->valid)
        level = std::min(level, c->battery.load());
    }
    return level;
  }

private:
  class Button final : public Input
  {
  public:
    Button(const char* name, const std::atomic<u32>& bits, u32 mask, const char* alias = nullptr)
        : m_name(name), m_alias(alias), m_bits(bits), m_mask(mask)
    {
    }
    std::string GetName() const override { return m_name; }
    // Like SDL: positional face buttons also answer to the XInput names.
    bool IsMatchingName(std::string_view name) const override
    {
      return name == m_name || (m_alias && name == m_alias);
    }
    ControlState GetState() const override
    {
      return (m_bits.load(std::memory_order_relaxed) & m_mask) ? 1.0 : 0.0;
    }

  private:
    const char* m_name;
    const char* m_alias;
    const std::atomic<u32>& m_bits;
    u32 m_mask;
  };

  class Axis final : public Input
  {
  public:
    Axis(const char* name, const std::atomic<float>& value, float sign)
        : m_name(name), m_value(value), m_sign(sign)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      return static_cast<ControlState>(m_value.load(std::memory_order_relaxed) * m_sign);
    }

  private:
    const char* m_name;
    const std::atomic<float>& m_value;
    float m_sign;
  };

  class Battery final : public Input
  {
  public:
    explicit Battery(const Device& dev) : m_dev(dev) {}
    std::string GetName() const override { return "Battery"; }
    ControlState GetState() const override { return m_dev.BatteryLevel(); }
    bool IsDetectable() const override { return false; }

  private:
    const Device& m_dev;
  };

  class Motor final : public Output
  {
  public:
    explicit Motor(std::atomic<bool>& on) : m_on(on) {}
    std::string GetName() const override { return "Motor"; }
    void SetState(ControlState state) override { m_on.store(state > 0.5); }

  private:
    std::atomic<bool>& m_on;
  };

  void Sticks(Connection& c, bool left, bool right)
  {
    if (left)
    {
      AddInput(new Axis("Left X-", c.lx, -1));
      AddInput(new Axis("Left X+", c.lx, 1));
      AddInput(new Axis("Left Y-", c.ly, -1));
      AddInput(new Axis("Left Y+", c.ly, 1));
    }
    if (right)
    {
      AddInput(new Axis("Right X-", c.rx, -1));
      AddInput(new Axis("Right X+", c.rx, 1));
      AddInput(new Axis("Right Y-", c.ry, -1));
      AddInput(new Axis("Right Y+", c.ry, 1));
    }
  }

  // Same names as SDL gives this controller over USB, so one profile fits both.
  void BuildGameCube(Connection& c)
  {
    struct Def
    {
      const char* name;
      u32 mask;
    };
    static constexpr Def defs[] = {
        {"Button S", 0x000002},    // A
        {"Button W", 0x000001},    // B
        {"Button E", 0x000008},    // X
        {"Button N", 0x000004},    // Y
        {"Shoulder R", 0x000010},  // Z
        {"Misc 4", 0x000020},      // R (end of travel)
        {"Start", 0x000040},
        {"Shoulder L", 0x001000},  // ZL
        {"Misc 3", 0x002000},      // L (end of travel)
        {"Pad S", 0x000100},       {"Pad E", 0x000200}, {"Pad W", 0x000400},
        {"Pad N", 0x000800},       {"Guide", 0x010000},  // Home
        {"Misc 1", 0x020000},                            // Capture
        {"Misc 2", 0x100000},                            // C
    };
    for (const auto& d : defs)
      AddInput(new Button(d.name, c.buttons, d.mask));
    Sticks(c, true, true);
    AddInput(new Axis("Trigger L", c.lt, 1));
    AddInput(new Axis("Trigger R", c.rt, 1));
    AddOutput(new Motor(c.rumble_on));
  }

  // Positional names like SDL (south = bottom button) + XInput aliases.
  void BuildPro(Connection& c)
  {
    AddInput(new Button("Button S", c.buttons, 0x000001, "Button A"));  // B
    AddInput(new Button("Button E", c.buttons, 0x000002, "Button B"));  // A
    AddInput(new Button("Button W", c.buttons, 0x000004, "Button X"));  // Y
    AddInput(new Button("Button N", c.buttons, 0x000008, "Button Y"));  // X
    AddInput(new Button("Shoulder R", c.buttons, 0x000010));
    AddInput(new Button("Trigger R", c.buttons, 0x000020));  // ZR (digital)
    AddInput(new Button("Start", c.buttons, 0x000040));
    AddInput(new Button("Thumb R", c.buttons, 0x000080));
    AddInput(new Button("Pad S", c.buttons, 0x000100));
    AddInput(new Button("Pad E", c.buttons, 0x000200));
    AddInput(new Button("Pad W", c.buttons, 0x000400));
    AddInput(new Button("Pad N", c.buttons, 0x000800));
    AddInput(new Button("Shoulder L", c.buttons, 0x001000));
    AddInput(new Button("Trigger L", c.buttons, 0x002000));  // ZL (digital)
    AddInput(new Button("Back", c.buttons, 0x004000));
    AddInput(new Button("Thumb L", c.buttons, 0x008000));
    AddInput(new Button("Guide", c.buttons, 0x010000));
    AddInput(new Button("Misc 1", c.buttons, 0x020000));    // Capture
    AddInput(new Button("Paddle 1", c.buttons, 0x040000));  // GR
    AddInput(new Button("Paddle 2", c.buttons, 0x080000));  // GL
    AddInput(new Button("Misc 2", c.buttons, 0x100000));    // C
    Sticks(c, true, true);
  }

  void BuildJoyCon(Connection& c)
  {
    if (c.kind == Kind::JoyConL)
    {
      AddInput(new Button("Pad S", c.buttons, 0x0001));
      AddInput(new Button("Pad E", c.buttons, 0x0002));
      AddInput(new Button("Pad W", c.buttons, 0x0004));
      AddInput(new Button("Pad N", c.buttons, 0x0008));
      AddInput(new Button("Shoulder L", c.buttons, 0x0010));
      AddInput(new Button("Trigger L", c.buttons, 0x0020));
      AddInput(new Button("Back", c.buttons, 0x0040));
      AddInput(new Button("Thumb L", c.buttons, 0x0080));
      AddInput(new Button("Misc 1", c.buttons, 0x0100));  // Capture
      AddInput(new Button("Paddle 4", c.buttons, 0x4000));  // SR (L)
      AddInput(new Button("Paddle 2", c.buttons, 0x8000));  // SL (L)
      Sticks(c, true, false);
    }
    else
    {
      AddInput(new Button("Button S", c.buttons, 0x0001, "Button A"));  // B
      AddInput(new Button("Button E", c.buttons, 0x0002, "Button B"));  // A
      AddInput(new Button("Button W", c.buttons, 0x0004, "Button X"));  // Y
      AddInput(new Button("Button N", c.buttons, 0x0008, "Button Y"));  // X
      AddInput(new Button("Shoulder R", c.buttons, 0x0010));
      AddInput(new Button("Trigger R", c.buttons, 0x0020));
      AddInput(new Button("Start", c.buttons, 0x0040));
      AddInput(new Button("Thumb R", c.buttons, 0x0080));
      AddInput(new Button("Guide", c.buttons, 0x0100));
      AddInput(new Button("Misc 2", c.buttons, 0x1000));    // C
      AddInput(new Button("Paddle 3", c.buttons, 0x4000));  // SR (R)
      AddInput(new Button("Paddle 1", c.buttons, 0x8000));  // SL (R)
      Sticks(c, false, true);
    }
  }

  std::vector<std::shared_ptr<Connection>> m_conns;
};

// ------------------------------------------------------------------ backend

// Outlives the backend: connection threads may still be running when Dolphin quits.
struct SharedState
{
  std::mutex mutex;
  std::map<u64, std::shared_ptr<Connection>> connections;
  std::set<u64> connecting;
  std::atomic<bool> shutdown{false};
};

bool IsJoyCon(const Connection& c)
{
  return c.kind == Kind::JoyConL || c.kind == Kind::JoyConR;
}

// Devices for the current connections: Joy-Con L + R become one controller.
std::vector<std::shared_ptr<Device>> MakeDevices(SharedState& s)
{
  std::vector<std::shared_ptr<Device>> devices;
  std::shared_ptr<Connection> jl, jr;
  for (auto& [addr, conn] : s.connections)
  {
    if (!conn->valid)
      continue;
    if (conn->kind == Kind::JoyConL && !jl)
      jl = conn;
    else if (conn->kind == Kind::JoyConR && !jr)
      jr = conn;
    else
      devices.push_back(std::make_shared<Device>(conn));
  }
  if (jl || jr)
    devices.push_back(std::make_shared<Device>(jl ? jl : jr, jl ? jr : nullptr));
  return devices;
}

class InputBackend final : public ciface::InputBackend
{
public:
  explicit InputBackend(ControllerInterface* controller_interface)
      : ciface::InputBackend(controller_interface)
  {
    try
    {
      m_watcher = Adv::BluetoothLEAdvertisementWatcher();
      m_watcher.ScanningMode(Adv::BluetoothLEScanningMode::Active);
      m_received = m_watcher.Received(
          [this](const Adv::BluetoothLEAdvertisementWatcher&,
                 const Adv::BluetoothLEAdvertisementReceivedEventArgs& args) { OnAdvert(args); });
      m_watcher.Start();
    }
    catch (const winrt::hresult_error& e)
    {
      WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: no Bluetooth LE ({:08x})",
                   static_cast<u32>(e.code()));
      m_watcher = nullptr;
    }
  }

  ~InputBackend() override
  {
    m_s->shutdown = true;
    try
    {
      if (m_watcher)
      {
        m_watcher.Received(m_received);
        m_watcher.Stop();
      }
    }
    catch (const winrt::hresult_error&)
    {
    }
    std::lock_guard lk(m_s->mutex);
    for (auto& [addr, conn] : m_s->connections)
      conn->Close();
    m_s->connections.clear();
  }

  void PopulateDevices() override
  {
    std::vector<std::shared_ptr<Device>> devices;
    {
      std::lock_guard lk(m_s->mutex);
      devices = MakeDevices(*m_s);
    }
    for (auto& d : devices)
      (void)GetControllerInterface().AddDevice(d);
  }

private:
  void OnAdvert(const Adv::BluetoothLEAdvertisementReceivedEventArgs& args)
  {
    if (m_s->shutdown)
      return;
    std::optional<Kind> kind;
    try
    {
      for (const auto& md : args.Advertisement().GetManufacturerDataByCompanyId(NINTENDO_COMPANY_ID))
      {
        const std::vector<u8> d = FromBuffer(md.Data());
        // 01 00 03 [vendor 7e 05] [product]
        if (d.size() >= 7 && d[3] == 0x7e && d[4] == 0x05)
          kind = KindFromProduct(static_cast<u16>(d[5] | (d[6] << 8)));
      }
    }
    catch (const winrt::hresult_error&)
    {
      return;
    }
    if (!kind)
      return;

    const u64 address = args.BluetoothAddress();
    {
      std::lock_guard lk(m_s->mutex);
      if (m_s->connecting.contains(address))
        return;
      if (auto it = m_s->connections.find(address); it != m_s->connections.end())
      {
        if (it->second->valid)
          return;
        it->second->Close();
        m_s->connections.erase(it);
      }
      m_s->connecting.insert(address);
    }
    std::thread([shared = m_s, address, k = *kind] {
      Common::SetCurrentThreadName("Switch2 BLE connect");
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      auto conn = Connect(address, k);
      bool add = false;
      {
        std::lock_guard lk(shared->mutex);
        shared->connecting.erase(address);
        if (conn && !shared->shutdown)
        {
          shared->connections[address] = conn;
          add = true;
        }
        else if (conn)
        {
          conn->Close();
        }
      }
      if (add)
      {
        if (IsJoyCon(*conn))
        {
          // Rebuild the Joy-Con device so both halves end up in one controller.
          g_controller_interface.RemoveDevice([](const Core::Device* d) {
            return d->GetSource() == "Switch2" && d->GetName().starts_with("Joy-Con");
          });
          std::vector<std::shared_ptr<Device>> devices;
          {
            std::lock_guard lk(shared->mutex);
            devices = MakeDevices(*shared);
          }
          for (auto& d : devices)
          {
            if (d->GetName().starts_with("Joy-Con"))
              (void)g_controller_interface.AddDevice(d);
          }
        }
        else
        {
          (void)g_controller_interface.AddDevice(std::make_shared<Device>(conn));
        }
      }
      winrt::uninit_apartment();
    }).detach();
  }

  static Gatt::GattCharacteristic FindChar(const Gatt::GattDeviceService& service,
                                           const winrt::guid& uuid)
  {
    const auto result =
        service.GetCharacteristicsForUuidAsync(uuid, BT::BluetoothCacheMode::Uncached).get();
    if (result.Status() != Gatt::GattCommunicationStatus::Success ||
        result.Characteristics().Size() == 0)
      return nullptr;
    return result.Characteristics().GetAt(0);
  }

  static std::shared_ptr<Connection> Connect(u64 address, Kind kind)
  {
    const KindInfo& info = Info(kind);
    auto conn = std::make_shared<Connection>();
    conn->address = address;
    conn->kind = kind;
    try
    {
      conn->device = BT::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
      if (!conn->device)
        return nullptr;
      conn->session = Gatt::GattSession::FromDeviceIdAsync(conn->device.BluetoothDeviceId()).get();
      if (conn->session)
        conn->session.MaintainConnection(true);

      // Windows 11: ask for a short connection interval (the default limits input to ~16 Hz).
      if (winrt::Windows::Foundation::Metadata::ApiInformation::IsMethodPresent(
              L"Windows.Devices.Bluetooth.BluetoothLEDevice",
              L"RequestPreferredConnectionParameters"))
      {
        try
        {
          conn->device.RequestPreferredConnectionParameters(
              BT::BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
        }
        catch (const winrt::hresult_error&)
        {
        }
      }

      const auto services =
          conn->device.GetGattServicesForUuidAsync(SERVICE_HID, BT::BluetoothCacheMode::Uncached)
              .get();
      if (services.Status() != Gatt::GattCommunicationStatus::Success ||
          services.Services().Size() == 0)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: HID service not found");
        conn->Close();
        return nullptr;
      }
      const auto hid = services.Services().GetAt(0);
      conn->input = FindChar(hid, info.input);
      conn->command = FindChar(hid, info.command);
      if (kind == Kind::GameCube)
        conn->rumble = FindChar(hid, info.rumble);
      if (!conn->input || !conn->command)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: characteristics not found");
        conn->Close();
        return nullptr;
      }

      std::weak_ptr<Connection> weak = conn;
      conn->status_token = conn->device.ConnectionStatusChanged(
          [weak](const BT::BluetoothLEDevice& dev, const winrt::Windows::Foundation::IInspectable&) {
            if (dev.ConnectionStatus() == BT::BluetoothConnectionStatus::Disconnected)
            {
              if (auto c = weak.lock())
                c->valid = false;
            }
          });

      // Same order as the console.
      const auto other =
          conn->device.GetGattServicesForUuidAsync(SERVICE_OTHER, BT::BluetoothCacheMode::Uncached)
              .get();
      if (other.Status() == Gatt::GattCommunicationStatus::Success && other.Services().Size() > 0)
      {
        if (auto c = FindChar(other.Services().GetAt(0), CHAR_OTHER_WRITE))
          (void)c.WriteValueAsync(ToBuffer({0x01, 0x00}), Gatt::GattWriteOption::WriteWithResponse)
              .get();
      }
      for (const winrt::guid& uuid : {CHAR_RESPONSE, info.response, CHAR_UNKNOWN_NOTIFY})
      {
        if (auto c = FindChar(hid, uuid))
        {
          (void)c.WriteClientCharacteristicConfigurationDescriptorAsync(
                     Gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify)
              .get();
          conn->notify_keepalive.push_back(c);
        }
      }

      const auto send = [&](std::vector<u8> cmd) {
        // report id + rumble data before the command, depending on the controller
        std::vector<u8> packet(info.command_prefix, 0);
        packet.insert(packet.end(), cmd.begin(), cmd.end());
        (void)conn->command
            .WriteValueAsync(ToBuffer(packet), Gatt::GattWriteOption::WriteWithoutResponse)
            .get();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
      };
      send({0x07, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00});
      send({0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00});
      send({0x16, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00});
      // Connection vibration, player 1 LED
      send({0x0a, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00});
      send({0x09, 0x91, 0x01, 0x07, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00});
      // Feature flags: buttons, sticks, analog triggers (no motion)
      send({0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00});
      send({0x11, 0x91, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00});
      send({0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00});

      // Report rate (?) then input notifications
      try
      {
        const auto descs = conn->input
                               .GetDescriptorsForUuidAsync(DESC_REPORT_RATE,
                                                           BT::BluetoothCacheMode::Uncached)
                               .get();
        if (descs.Status() == Gatt::GattCommunicationStatus::Success &&
            descs.Descriptors().Size() > 0)
          (void)descs.Descriptors().GetAt(0).WriteValueAsync(ToBuffer({0x85, 0x00})).get();
      }
      catch (const winrt::hresult_error&)
      {
      }
      conn->input_token = conn->input.ValueChanged(
          [weak](const Gatt::GattCharacteristic&, const Gatt::GattValueChangedEventArgs& args) {
            if (auto c = weak.lock())
            {
              try
              {
                c->Parse(FromBuffer(args.CharacteristicValue()));
              }
              catch (const winrt::hresult_error&)
              {
              }
            }
          });
      const auto status = conn->input
                              .WriteClientCharacteristicConfigurationDescriptorAsync(
                                  Gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify)
                              .get();
      if (status != Gatt::GattCommunicationStatus::Success)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: input notifications refused");
        conn->Close();
        return nullptr;
      }
      NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: controller connected");
      return conn;
    }
    catch (const winrt::hresult_error& e)
    {
      WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: connection failed ({:08x})",
                   static_cast<u32>(e.code()));
      conn->Close();
      return nullptr;
    }
  }

  Adv::BluetoothLEAdvertisementWatcher m_watcher{nullptr};
  winrt::event_token m_received{};
  std::shared_ptr<SharedState> m_s = std::make_shared<SharedState>();
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::Switch2BLE
