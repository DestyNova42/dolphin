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
#include <set>
#include <string>
#include <thread>
#include <vector>

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
constexpr u16 PRODUCT_GAMECUBE = 0x2073;

// GATT attributes (see ndeadly/switch2_controller_research, bluetooth_interface.md)
constexpr winrt::guid SERVICE_HID{0xab7de9be, 0x89fe, 0x49ad,
                                  {0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0}};
constexpr winrt::guid SERVICE_OTHER{0x00c5af5d, 0x1964, 0x4e30,
                                    {0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x80}};
constexpr winrt::guid CHAR_OTHER_WRITE{0x00c5af5d, 0x1964, 0x4e30,
                                       {0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x82}};
constexpr winrt::guid CHAR_INPUT_GC{0x8261cba1, 0x9435, 0x420c,
                                    {0x84, 0xd6, 0xf0, 0xc7, 0x5a, 0x2c, 0x8e, 0x4d}};
constexpr winrt::guid CHAR_RUMBLE_GC{0x3f8fb670, 0xab25, 0x45bf,
                                     {0xb5, 0x40, 0x38, 0xc7, 0x28, 0x34, 0xd0, 0x64}};
constexpr winrt::guid CHAR_COMMAND_GC{0xaf95885e, 0x44b3, 0x4a24,
                                      {0x9c, 0xf0, 0x48, 0x3c, 0xc1, 0x29, 0x46, 0x9a}};
constexpr winrt::guid CHAR_RESPONSE{0xc765a961, 0xd9d8, 0x4d36,
                                    {0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a}};
constexpr winrt::guid CHAR_RESPONSE_GC{0x46f6ad29, 0xcdaf, 0x4569,
                                       {0xa2, 0xfe, 0x33, 0x90, 0x20, 0xb9, 0x46, 0x04}};
constexpr winrt::guid CHAR_UNKNOWN_NOTIFY{0xd3bd69d2, 0x841c, 0x4241,
                                          {0xab, 0x15, 0xf8, 0x6f, 0x40, 0x6d, 0x2a, 0x80}};
constexpr winrt::guid DESC_REPORT_RATE{0x679d5510, 0x5a24, 0x4dee,
                                       {0x95, 0x57, 0x95, 0xdf, 0x80, 0x48, 0x6e, 0xcb}};

// Stick range from the centre (12-bit raw values)
constexpr float MAIN_STICK_RANGE = 1225.0f;
constexpr float C_STICK_RANGE = 1120.0f;

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

// State shared by the Bluetooth callbacks and the Dolphin device.
struct Connection
{
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
  std::atomic<bool> rumble_on{false};

  // calibration taken from the first reports
  bool calibrated = false;
  float lcx = 2048, lcy = 2048, rcx = 2048, rcy = 2048;
  float lt_zero = 30, rt_zero = 30, lt_max = 195, rt_max = 195;

  u8 rumble_tid = 0;
  bool rumble_sent_on = false;
  std::chrono::steady_clock::time_point last_rumble{};

  void Parse(const std::vector<u8>& r)
  {
    if (r.size() < 14)
      return;
    // Input report 0x0A (GameCube), without the report id
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
    lt_max = std::max(lt_max, tl);
    rt_max = std::max(rt_max, tr);
    const auto axis = [](float v, float c, float range) {
      return std::clamp((v - c) / range, -1.0f, 1.0f);
    };
    lx.store(axis(rlx, lcx, MAIN_STICK_RANGE), std::memory_order_relaxed);
    ly.store(axis(rly, lcy, MAIN_STICK_RANGE), std::memory_order_relaxed);
    rx.store(axis(rrx, rcx, C_STICK_RANGE), std::memory_order_relaxed);
    ry.store(axis(rry, rcy, C_STICK_RANGE), std::memory_order_relaxed);
    lt.store(std::clamp((tl - lt_zero) / (lt_max - lt_zero), 0.0f, 1.0f), std::memory_order_relaxed);
    rt.store(std::clamp((tr - rt_zero) / (rt_max - rt_zero), 0.0f, 1.0f), std::memory_order_relaxed);
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
  explicit Device(std::shared_ptr<Connection> conn) : m_conn(std::move(conn))
  {
    // Same names as SDL gives this controller over USB, so one profile fits both.
    struct ButtonDef
    {
      const char* name;
      u32 mask;
    };
    static constexpr ButtonDef buttons[] = {
        {"Button S", 0x000002},    // A
        {"Button W", 0x000001},    // B
        {"Button E", 0x000008},    // X
        {"Button N", 0x000004},    // Y
        {"Shoulder R", 0x000010},  // Z
        {"Misc 4", 0x000020},      // R (end of travel)
        {"Start", 0x000040},       // Start / +
        {"Shoulder L", 0x001000},  // ZL
        {"Misc 3", 0x002000},      // L (end of travel)
        {"Pad S", 0x000100},
        {"Pad E", 0x000200},
        {"Pad W", 0x000400},
        {"Pad N", 0x000800},
        {"Guide", 0x010000},   // Home
        {"Misc 1", 0x020000},  // Capture
        {"Misc 2", 0x100000},  // C
    };
    for (const auto& b : buttons)
      AddInput(new Button(b.name, m_conn->buttons, b.mask));
    AddInput(new Axis("Left X-", m_conn->lx, -1));
    AddInput(new Axis("Left X+", m_conn->lx, 1));
    AddInput(new Axis("Left Y-", m_conn->ly, -1));
    AddInput(new Axis("Left Y+", m_conn->ly, 1));
    AddInput(new Axis("Right X-", m_conn->rx, -1));
    AddInput(new Axis("Right X+", m_conn->rx, 1));
    AddInput(new Axis("Right Y-", m_conn->ry, -1));
    AddInput(new Axis("Right Y+", m_conn->ry, 1));
    AddInput(new Axis("Trigger L", m_conn->lt, 1));
    AddInput(new Axis("Trigger R", m_conn->rt, 1));
    AddOutput(new Motor(m_conn->rumble_on));
  }

  std::string GetName() const override { return "Nintendo GameCube Controller"; }
  std::string GetSource() const override { return "Switch2"; }
  int GetSortPriority() const override { return 2; }

  Core::DeviceRemoval UpdateInput() override
  {
    if (!m_conn->valid.load())
      return Core::DeviceRemoval::Remove;
    m_conn->SendRumble();
    return Core::DeviceRemoval::Keep;
  }

  bool IsValid() const override { return m_conn->valid.load(); }

private:
  class Button final : public Input
  {
  public:
    Button(const char* name, const std::atomic<u32>& bits, u32 mask)
        : m_name(name), m_bits(bits), m_mask(mask)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      return (m_bits.load(std::memory_order_relaxed) & m_mask) ? 1.0 : 0.0;
    }

  private:
    const char* m_name;
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

  class Motor final : public Output
  {
  public:
    explicit Motor(std::atomic<bool>& on) : m_on(on) {}
    std::string GetName() const override { return "Motor"; }
    void SetState(ControlState state) override { m_on.store(state > 0.5); }

  private:
    std::atomic<bool>& m_on;
  };

  std::shared_ptr<Connection> m_conn;
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
    std::lock_guard lk(m_s->mutex);
    for (auto& [addr, conn] : m_s->connections)
    {
      if (conn->valid)
        (void)GetControllerInterface().AddDevice(std::make_shared<Device>(conn));
    }
  }

private:
  void OnAdvert(const Adv::BluetoothLEAdvertisementReceivedEventArgs& args)
  {
    if (m_s->shutdown)
      return;
    try
    {
      bool is_gamecube = false;
      for (const auto& md : args.Advertisement().GetManufacturerDataByCompanyId(NINTENDO_COMPANY_ID))
      {
        const std::vector<u8> d = FromBuffer(md.Data());
        // 01 00 03 [vendor 7e 05] [product]
        if (d.size() >= 7 && d[3] == 0x7e && d[4] == 0x05 &&
            static_cast<u16>(d[5] | (d[6] << 8)) == PRODUCT_GAMECUBE)
        {
          is_gamecube = true;
        }
      }
      if (!is_gamecube)
        return;
    }
    catch (const winrt::hresult_error&)
    {
      return;
    }

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
    std::thread([shared = m_s, address] {
      Common::SetCurrentThreadName("Switch2 BLE connect");
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      auto conn = Connect(address);
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
        (void)g_controller_interface.AddDevice(std::make_shared<Device>(conn));
      winrt::uninit_apartment();
    }).detach();
  }

  static Gatt::GattCharacteristic FindChar(const Gatt::GattDeviceService& service,
                                           const winrt::guid& uuid)
  {
    const auto result = service.GetCharacteristicsForUuidAsync(uuid, BT::BluetoothCacheMode::Uncached).get();
    if (result.Status() != Gatt::GattCommunicationStatus::Success ||
        result.Characteristics().Size() == 0)
      return nullptr;
    return result.Characteristics().GetAt(0);
  }

  static std::shared_ptr<Connection> Connect(u64 address)
  {
    auto conn = std::make_shared<Connection>();
    conn->address = address;
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
              L"Windows.Devices.Bluetooth.BluetoothLEDevice", L"RequestPreferredConnectionParameters"))
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
          conn->device.GetGattServicesForUuidAsync(SERVICE_HID, BT::BluetoothCacheMode::Uncached).get();
      if (services.Status() != Gatt::GattCommunicationStatus::Success ||
          services.Services().Size() == 0)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: HID service not found");
        conn->Close();
        return nullptr;
      }
      const auto hid = services.Services().GetAt(0);
      conn->input = FindChar(hid, CHAR_INPUT_GC);
      conn->rumble = FindChar(hid, CHAR_RUMBLE_GC);
      conn->command = FindChar(hid, CHAR_COMMAND_GC);
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
          conn->device.GetGattServicesForUuidAsync(SERVICE_OTHER, BT::BluetoothCacheMode::Uncached).get();
      if (other.Status() == Gatt::GattCommunicationStatus::Success && other.Services().Size() > 0)
      {
        if (auto c = FindChar(other.Services().GetAt(0), CHAR_OTHER_WRITE))
          (void)c.WriteValueAsync(ToBuffer({0x01, 0x00}), Gatt::GattWriteOption::WriteWithResponse).get();
      }
      for (const winrt::guid& uuid : {CHAR_RESPONSE, CHAR_RESPONSE_GC, CHAR_UNKNOWN_NOTIFY})
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
        // GameCube: report id + 4 bytes of rumble before the command
        std::vector<u8> packet(5, 0);
        packet.insert(packet.end(), cmd.begin(), cmd.end());
        (void)conn->command.WriteValueAsync(ToBuffer(packet), Gatt::GattWriteOption::WriteWithoutResponse)
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
        const auto descs = conn->input.GetDescriptorsForUuidAsync(DESC_REPORT_RATE,
                                                                  BT::BluetoothCacheMode::Uncached)
                               .get();
        if (descs.Status() == Gatt::GattCommunicationStatus::Success && descs.Descriptors().Size() > 0)
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
      NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Switch2BLE: GameCube controller connected");
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
