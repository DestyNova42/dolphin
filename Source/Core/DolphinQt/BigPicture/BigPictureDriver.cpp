// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureDriver.h"

#include <QMetaObject>
#include <QPointer>

#include <thread>

#ifdef _WIN32
#include <Windows.h>
// Windows.h first
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <combaseapi.h>
#include <devguid.h>
#include <newdev.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "setupapi")
#pragma comment(lib, "newdev")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#endif

namespace BigPictureDriver
{
#ifdef _WIN32
namespace
{
// Interface GUID published by WinUSB for these devices (libusb finds them through it).
constexpr wchar_t INTERFACE_GUID[] = L"{6D7B2D5A-4C3E-4A8B-9C21-5F0A8E3B7D42}";

std::wstring Upper(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
  return s;
}

std::wstring Property(HDEVINFO set, SP_DEVINFO_DATA* data, DWORD prop)
{
  DWORD type = 0, size = 0;
  SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type, nullptr, 0, &size);
  if (size == 0)
    return {};
  std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
  if (!SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type,
                                         reinterpret_cast<BYTE*>(buf.data()), size, nullptr))
    return {};
  // REG_MULTI_SZ: keep the separators as '|'
  for (wchar_t& c : buf)
  {
    if (c == L'\0')
      c = L'|';
  }
  return buf;
}

// What a device is, from its hardware ids. Empty if it doesn't need WinUSB.
std::string Classify(const std::wstring& hwids)
{
  const std::wstring ids = Upper(hwids);
  const auto s2 = [&](const wchar_t* pid) {
    return ids.find(std::wstring(L"VID_057E&PID_") + pid + L"&MI_01") != std::wstring::npos;
  };
  if (s2(L"2073"))
    return "Manette GameCube (Switch 2)";
  if (s2(L"2069"))
    return "Manette Pro (Switch 2)";
  if (s2(L"2066") || s2(L"2067"))
    return "Joy-Con 2";
  return {};
}

bool HasUsbDriver(const std::wstring& service)
{
  const std::wstring s = Upper(service);
  return s.rfind(L"WINUSB", 0) == 0 || s.rfind(L"LIBUSBK", 0) == 0 || s.rfind(L"LIBUSB0", 0) == 0;
}

template <typename F>
void ForEachPresentUsbDevice(F&& fn)
{
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE)
    return;
  SP_DEVINFO_DATA data{};
  data.cbSize = sizeof(data);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i)
  {
    const std::wstring hwids = Property(set, &data, SPDRP_HARDWAREID);
    const std::string kind = Classify(hwids);
    if (kind.empty())
      continue;
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    SetupDiGetDeviceInstanceIdW(set, &data, id, MAX_DEVICE_ID_LEN, nullptr);
    fn(std::wstring(id), kind, Property(set, &data, SPDRP_SERVICE));
  }
  SetupDiDestroyDeviceInfoList(set);
}

bool InstallWinUsb(const std::wstring& instance_id)
{
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE)
    return false;
  bool ok = false;
  SP_DEVINFO_DATA dev{};
  dev.cbSize = sizeof(dev);
  do
  {
    if (!SetupDiOpenDeviceInfoW(set, instance_id.c_str(), nullptr, 0, &dev))
      break;

    // Interface GUID, read by WinUSB when the device starts.
    if (HKEY key = SetupDiCreateDevRegKeyW(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr,
                                           nullptr);
        key != INVALID_HANDLE_VALUE)
    {
      std::wstring multi = INTERFACE_GUID;
      multi.push_back(L'\0');
      multi.push_back(L'\0');
      RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                     reinterpret_cast<const BYTE*>(multi.data()),
                     static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
      RegCloseKey(key);
    }

    // Offer the drivers of the "USB devices" class, from Windows' own winusb.inf.
    const GUID usb_class = GUID_DEVCLASS_USBDEVICE;
    wchar_t class_str[64] = {};
    StringFromGUID2(usb_class, class_str, 64);
    SetupDiSetDeviceRegistryPropertyW(
        set, &dev, SPDRP_CLASSGUID, reinterpret_cast<const BYTE*>(class_str),
        static_cast<DWORD>((wcslen(class_str) + 1) * sizeof(wchar_t)));
    dev.ClassGuid = usb_class;

    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(params);
    if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params))
      break;
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    swprintf_s(params.DriverPath, L"%s\\INF\\winusb.inf", windir);
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params))
      break;
    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER))
      break;
    SP_DRVINFO_DATA_W driver{};
    driver.cbSize = sizeof(driver);
    if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, 0, &driver))
      break;
    if (!SetupDiSetSelectedDriverW(set, &dev, &driver))
      break;
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, set, &dev, &driver, DIIDFLAG_NOFINISHINSTALLUI, &reboot))
      break;

    // Restart it so the interface is published right away.
    SP_PROPCHANGE_PARAMS change{};
    change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.StateChange = DICS_PROPCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;
    if (SetupDiSetClassInstallParamsW(set, &dev, &change.ClassInstallHeader, sizeof(change)))
      SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
    ok = true;
  } while (false);
  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

template <typename F>
void Post(QObject* context, F&& fn)
{
  QPointer<QObject> guard(context);
  QMetaObject::invokeMethod(
      context,
      [guard, fn = std::forward<F>(fn)]() mutable {
        if (guard)
          fn();
      },
      Qt::QueuedConnection);
}
}  // namespace

std::vector<Target> DevicesNeedingDriver()
{
  std::vector<Target> targets;
  ForEachPresentUsbDevice([&](const std::wstring& id, const std::string& kind,
                              const std::wstring& service) {
    if (!HasUsbDriver(service))
      targets.push_back({id, kind});
  });
  return targets;
}

void InstallAsync(QObject* context, std::function<void(bool)> on_done)
{
  std::thread([context, on_done = std::move(on_done)] {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring arg(INSTALL_ARG, INSTALL_ARG + sizeof(INSTALL_ARG) - 1);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";  // Windows asks for administrator rights
    info.lpFile = exe;
    info.lpParameters = arg.c_str();
    info.nShow = SW_HIDE;
    bool ok = false;
    if (ShellExecuteExW(&info) && info.hProcess)
    {
      WaitForSingleObject(info.hProcess, 120000);
      DWORD code = 1;
      GetExitCodeProcess(info.hProcess, &code);
      CloseHandle(info.hProcess);
      ok = code == 0;
    }
    Post(context, [on_done, ok] { on_done(ok); });
  }).detach();
}

int RunElevatedInstall()
{
  int failures = 0;
  for (const Target& t : DevicesNeedingDriver())
  {
    if (!InstallWinUsb(t.instance_id))
      ++failures;
  }
  return failures == 0 ? 0 : 1;
}

#else

std::vector<Target> DevicesNeedingDriver()
{
  return {};
}
void InstallAsync(QObject* context, std::function<void(bool)> on_done)
{
  QMetaObject::invokeMethod(context, [on_done] { on_done(false); }, Qt::QueuedConnection);
}
int RunElevatedInstall()
{
  return 1;
}

#endif
}  // namespace BigPictureDriver
