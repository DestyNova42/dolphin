// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureWindow.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <chrono>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <numbers>

#include <fmt/format.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#ifdef HAVE_BIGPICTURE_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <unistd.h>
#endif
#include <QStandardPaths>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSettings>
#include <QTime>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Widescreen.h"
#include "DolphinQt/BigPicture/BigPictureDisplay.h"
#ifdef _WIN32
#include "InputCommon/ControllerInterface/Switch2BLE/Switch2BLE.h"
#endif
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "Core/HW/Sram.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "DiscIO/WiiSaveBanner.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HotkeyManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/TimePlayed.h"
#include "DiscIO/Enums.h"
#include "DolphinQt/BigPicture/BigPictureAudio.h"
#include "DolphinQt/BigPicture/BigPictureDriver.h"
#include "DolphinQt/BigPicture/BigPictureSync.h"
#include "DolphinQt/BigPicture/BigPictureUpdater.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/QtUtils/ImageConverter.h"
#include "DolphinQt/Settings.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/GameFile.h"

// Some Wii headers bring in Windows.h: drop the macros that clash with our names
#ifdef _WIN32
#undef TEXT
#undef DrawText
#undef DrawIcon
#undef DrawState
#endif

using Sound = BigPictureAudio::Sound;
namespace Prefs = BigPicturePrefs;

namespace
{
// Theme (validated mock-up "Mode manette")
// Theme colours (changed by ApplyTheme)
QColor BG(13, 11, 26);
QColor SURFACE(23, 20, 43);
QColor SURFACE_DEEP(18, 15, 36);
QColor RAISED(34, 29, 61);
QColor LINE(46, 40, 80);
QColor TEXT(244, 241, 255);
QColor TEXT_SOFT(185, 178, 214);
QColor TEXT_DIM(141, 133, 179);
QColor ACCENT(139, 108, 255);
QColor ACCENT_SOFT(201, 185, 255);

struct Palette
{
  QColor bg, surface, deep, raised, line, text, soft, dim, accent, accent_soft;
};

Palette PaletteFor(BigPicturePrefs::Theme theme)
{
  using T = BigPicturePrefs::Theme;
  switch (theme)
  {
  case T::IndigoGameCube:  // the indigo console
    return {{38, 31, 96},     {52, 44, 122},    {44, 37, 108},    {66, 56, 146},
            {88, 77, 172},    {248, 246, 255},  {210, 204, 244},  {166, 158, 222},
            {160, 146, 255},  {222, 214, 255}};
  case T::Spice:  // Spice Orange: orange like the console, black like its lid
    return {{240, 124, 20},   {226, 108, 10},   {232, 116, 14},   {252, 146, 48},
            {255, 176, 96},   {255, 255, 255},  {255, 236, 214},  {255, 212, 168},
            {28, 25, 30},     {66, 30, 0}};
  case T::JetBlack:  // Jet Black
    return {{6, 6, 8},        {20, 20, 23},     {12, 12, 14},     {34, 34, 39},
            {52, 52, 60},     {242, 242, 246},  {172, 172, 182},  {122, 122, 134},
            {196, 198, 210},  {228, 229, 236}};
  case T::Platinum:  // Platinum (silver)
    return {{206, 209, 216},  {188, 192, 202},  {197, 200, 208},  {174, 178, 190},
            {150, 154, 168},  {20, 21, 28},     {62, 64, 80},     {98, 101, 118},
            {84, 66, 186},    {72, 56, 170}};
  case T::PearlWhite:  // Pearl White
    return {{244, 243, 247},  {231, 230, 237},  {238, 237, 243},  {219, 218, 228},
            {198, 197, 210},  {26, 25, 36},     {78, 77, 98},     {120, 119, 142},
            {122, 104, 228},  {104, 86, 210}};
  default:
    return {{13, 11, 26},     {23, 20, 43},     {18, 15, 36},     {34, 29, 61},
            {46, 40, 80},     {244, 241, 255},  {185, 178, 214},  {141, 133, 179},
            {139, 108, 255},  {201, 185, 255}};
  }
}
const QColor OK_GREEN(91, 214, 154);
const QColor WARN(255, 180, 71);
const QColor DANGER(255, 180, 163);

constexpr int POLL_INTERVAL_MS = 16;
constexpr qint64 REPEAT_DELAY_MS = 350;
constexpr qint64 REPEAT_RATE_MS = 85;
constexpr int STICK_THRESHOLD = 60;
constexpr int SLOT_COUNT = 8;

constexpr u32 Bit(int action)
{
  return 1u << action;
}

qreal EaseOut(qreal t)
{
  t = std::clamp<qreal>(t, 0, 1);
  return 1 - (1 - t) * (1 - t) * (1 - t);
}

qreal EaseInOut(qreal t)
{
  t = std::clamp<qreal>(t, 0, 1);
  return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

// Deterministic pseudo-random in [0, 1) for the background effects.
qreal Hash01(int i, int salt)
{
  u32 x = static_cast<u32>(i) * 747796405u + static_cast<u32>(salt) * 2891336453u;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return (x & 0xFFFFFF) / static_cast<qreal>(0x1000000);
}

QPainterPath CubePath(qreal s)
{
  // Isometric cube outline centred on (0, 0), "size" s.
  const qreal h = s * 0.5;
  QPainterPath p;
  p.moveTo(0, -h);
  p.lineTo(h * 0.87, -h * 0.5);
  p.lineTo(h * 0.87, h * 0.5);
  p.lineTo(0, h);
  p.lineTo(-h * 0.87, h * 0.5);
  p.lineTo(-h * 0.87, -h * 0.5);
  p.closeSubpath();
  p.moveTo(0, 0);
  p.lineTo(h * 0.87, -h * 0.5);
  p.moveTo(0, 0);
  p.lineTo(-h * 0.87, -h * 0.5);
  p.moveTo(0, 0);
  p.lineTo(0, h);
  return p;
}

struct SetupStep
{
  PadGroup group;
  int index;
  const char* prompt;
  const char* help;
  const char* icon;
  int mirror_index;  // opposite direction for sticks, digital twin for triggers (-1 = none)
  bool flip;         // mirror by flipping the axis sign
};

constexpr SetupStep SETUP_STEPS[] = {
    {PadGroup::Buttons, 0, "Appuie sur A", "Le gros bouton vert.", "A", -1, false},
    {PadGroup::Buttons, 1, "Appuie sur B", "Le petit bouton rouge, en bas à gauche de A.", "B", -1,
     false},
    {PadGroup::Buttons, 2, "Appuie sur X", "Le bouton à droite de A.", "X", -1, false},
    {PadGroup::Buttons, 3, "Appuie sur Y", "Le bouton au-dessus de A.", "Y", -1, false},
    {PadGroup::Buttons, 4, "Appuie sur Z", "Le bouton violet au-dessus de la gâchette R.", "Z",
     -1, false},
    {PadGroup::Buttons, 5, "Appuie sur START", "Le bouton au centre de la manette.",
     "Start-Pause", -1, false},
    {PadGroup::MainStick, 0, "Stick principal vers le haut", "Pousse-le à fond puis relâche.",
     "Control_Stick-U", 1, true},
    {PadGroup::MainStick, 3, "Stick principal vers la droite", "Pousse-le à fond puis relâche.",
     "Control_Stick-R", 2, true},
    {PadGroup::CStick, 0, "Stick C vers le haut", "Le petit stick jaune.", "C-Stick-U", 1, true},
    {PadGroup::CStick, 3, "Stick C vers la droite", "Le petit stick jaune.", "C-Stick-R", 2, true},
    {PadGroup::Triggers, 2, "Enfonce L à fond", "Jusqu'au clic.", "L", 0, false},
    {PadGroup::Triggers, 3, "Enfonce R à fond", "Jusqu'au clic.", "R", 1, false},
    {PadGroup::DPad, 0, "Croix : haut", "La croix directionnelle, sous le stick.", "D-Pad-U", -1,
     false},
    {PadGroup::DPad, 1, "Croix : bas", "La croix directionnelle, sous le stick.", "D-Pad-D", -1,
     false},
    {PadGroup::DPad, 2, "Croix : gauche", "La croix directionnelle, sous le stick.", "D-Pad-L",
     -1, false},
    {PadGroup::DPad, 3, "Croix : droite", "La croix directionnelle, sous le stick.", "D-Pad-R",
     -1, false},
};
constexpr int SETUP_STEP_COUNT = static_cast<int>(std::size(SETUP_STEPS));

ControllerEmu::EmulatedController* PadController(int port)
{
  return Pad::GetConfig()->GetController(port);
}

// "`Left Y-`" -> "`Left Y+`"
std::string FlipAxis(std::string expr)
{
  for (size_t i = expr.size(); i-- > 0;)
  {
    if (expr[i] == '+' || expr[i] == '-')
    {
      expr[i] = expr[i] == '+' ? '-' : '+';
      break;
    }
  }
  return expr;
}

// The external frontend tried before this mode existed routed hotkeys through a DSU server
// ("DSUClient/1/GC"). Without that program, those hotkeys are dead: restore the defaults.
void CleanupLegacyFrontendConfig()
{
  namespace DSU = ciface::DualShockUDPClient::Settings;
  const std::string legacy_entry = "GC:127.0.0.1:26760;";
  std::string servers = Config::Get(DSU::SERVERS);
  if (const size_t pos = servers.find(legacy_entry); pos != std::string::npos)
  {
    servers.erase(pos, legacy_entry.size());
    Config::SetBase(DSU::SERVERS, servers);
    if (servers.empty())
      Config::SetBase(DSU::SERVERS_ENABLED, false);
  }

  auto* hotkeys = HotkeyManagerEmu::GetConfig()->GetController(0);
  if (hotkeys && hotkeys->GetDefaultDevice().source == "DSUClient")
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    hotkeys->LoadDefaults(g_controller_interface);
    hotkeys->UpdateReferences(g_controller_interface);
    HotkeyManagerEmu::GetConfig()->SaveConfig();
  }
}

QString RegionName(DiscIO::Region region)
{
  switch (region)
  {
  case DiscIO::Region::NTSC_J:
    return QStringLiteral("Japon");
  case DiscIO::Region::NTSC_U:
    return QStringLiteral("Amérique");
  case DiscIO::Region::PAL:
    return QStringLiteral("Europe");
  case DiscIO::Region::NTSC_K:
    return QStringLiteral("Corée");
  default:
    return QString();
  }
}

QString LastPlayedKey(const QString& path)
{
  return QStringLiteral("bigpicture/lastplayed/") + QString::number(qHash(path), 16);
}

// Pause menu entries
enum PauseItem
{
  PAUSE_RESUME,
  PAUSE_SAVE,
  PAUSE_LOAD,
  PAUSE_QUICK,
  PAUSE_CHEATS,
  PAUSE_SETTINGS,
  PAUSE_DISC,
  PAUSE_SCREENSHOT,
  PAUSE_QUIT,
};
}  // namespace

// Small click-through window used to show notifications over a running game.
class NotifierWidget final : public QWidget
{
public:
  explicit NotifierWidget(std::function<void(QPainter&)> paint)
      : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                             Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
        m_paint(std::move(paint))
  {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    m_paint(painter);
  }

private:
  std::function<void(QPainter&)> m_paint;
};

// ================================================================== lifecycle

BigPictureWindow::BigPictureWindow(const GameListModel& model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
  setWindowTitle(tr("Dolphin - Mode manette"));
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);  // the pause menu shows the game behind it
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::BlankCursor);

  const QString res_dir =
      QString::fromStdString(File::GetSysDirectory() + "Resources/BigPicture/");

  // Button icons, pre-rendered from the original SVG files (see LICENSES.txt).
  const QDir icon_dir(res_dir + QStringLiteral("png"));
  for (const QString& file : icon_dir.entryList({QStringLiteral("*.png")}, QDir::Files))
  {
    QPixmap pixmap(icon_dir.filePath(file));
    if (!pixmap.isNull())
      m_icons.insert(QFileInfo(file).completeBaseName(), pixmap);
  }

  const auto load_font = [&](const QString& file) {
    const int id = QFontDatabase::addApplicationFont(res_dir + QStringLiteral("Fonts/") + file);
    const QStringList families =
        id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList{};
    return families.isEmpty() ? QStringLiteral("Segoe UI") : families.front();
  };
  m_display_family = load_font(QStringLiteral("SpaceGrotesk.ttf"));
  m_text_family = load_font(QStringLiteral("Inter.ttf"));

  const QStringList favorites =
      Settings::GetQSettings().value(QStringLiteral("bigpicture/favorites")).toStringList();
  m_favorites = QSet<QString>(favorites.begin(), favorites.end());

  m_audio = std::make_unique<BigPictureAudio>();

  BigPictureSettingsHost host;
  host.configure_controller = [this](int port) {
    QTimer::singleShot(0, this, [this, port] { StartSetup(port); });
  };
  host.open_classic = [this] { QTimer::singleShot(0, this, [this] { emit ClassicRequested(); }); };
  host.remap_control = [this](int port, int group, int index, int mirror, bool flip,
                              const QString& prompt) {
    QTimer::singleShot(0, this, [=, this] { StartRemap(port, group, index, mirror, flip, prompt); });
  };
  host.calibrate_stick = [this](int port, int group) {
    QTimer::singleShot(0, this, [this, port, group] { StartCalibration(port, group); });
  };
  host.open_memory_cards = [this] { QTimer::singleShot(0, this, [this] { OpenMemcards(); }); };
  host.test_controller = [this](int port) {
    QTimer::singleShot(0, this, [this, port] {
      m_test_port = port;
      m_test_start_since = -1;
      m_mode_before_players = m_mode;
      BeginTransition(0);
      m_mode = Mode::PadTest;
    });
  };
  host.check_updates = [this] { QTimer::singleShot(0, this, [this] { CheckUpdates(false); }); };
  host.test_wiimote = [this](int port) {
    QTimer::singleShot(0, this, [this, port] {
      m_test_port = port;
      m_test_start_since = -1;
      m_mode_before_players = m_mode;
      BeginTransition(0);
      m_mode = Mode::WiiTest;
    });
  };
  host.sync_now = [this] { QTimer::singleShot(0, this, [this] { SyncSaves(true); }); };
  host.install_drivers = [this] {
    QTimer::singleShot(0, this, [this] {
      if (BigPictureDriver::DevicesNeedingDriver().empty())
      {
        Toast(tr("Branche la manette en USB : aucun pilote à installer"));
        return;
      }
      m_mode_before_driver = m_mode;
      InstallDrivers();
    });
  };
  host.notify = [this](const QString& text) { Toast(text); };
  host.interface_changed = [this] { ApplyPreferences(); };
  m_settings = BuildBigPictureSettings(host);
  m_quick_settings = BuildBigPictureQuickSettings();

  // Controllers: automatic switching and hot plug
  m_device_timer = new QTimer(this);
  m_device_timer->setInterval(60);
  connect(m_device_timer, &QTimer::timeout, this, &BigPictureWindow::PollDevices);
  m_device_timer->start();
  connect(&Settings::Instance(), &Settings::DevicesChanged, this,
          &BigPictureWindow::OnDevicesChanged);
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
    m_known_devices << QString::fromStdString(d);

  // Sleep of the PC / console: pause and save the game first
  BigPictureDisplay::InstallPowerNotifications([this](bool suspend) {
    if (suspend)
      OnSystemSuspend();
    else
      OnSystemResume();
  });
#ifdef HAVE_BIGPICTURE_DBUS
  QDBusConnection::systemBus().connect(
      QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
      QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("PrepareForSleep"), this,
      SLOT(OnPrepareForSleep(bool)));
  TakeSleepInhibitor();
#endif

  m_notifier = new NotifierWidget([this](QPainter& p) {
    const qreal a = std::min<qreal>(1, std::min(m_notify_t / 0.3, (3.0 - m_notify_t) / 0.25));
    const QRectF r = QRectF(m_notifier->rect()).adjusted(8, 8, -8, -8);
    p.setOpacity(std::clamp<qreal>(a, 0, 1));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(SURFACE.red(), SURFACE.green(), SURFACE.blue(), 235));
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    p.setBrush(ACCENT);
    p.drawEllipse(QPointF(r.left() + r.height() / 2, r.center().y()), 7, 7);
    QFont font(m_text_family);
    font.setPixelSize(int(r.height() * 0.34));
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.setPen(TEXT);
    p.drawText(r.adjusted(r.height(), 0, -20, 0), Qt::AlignVCenter, m_notify_text);
  });

  m_clock.start();

  m_poll_timer = new QTimer(this);
  m_poll_timer->setInterval(POLL_INTERVAL_MS);
  connect(m_poll_timer, &QTimer::timeout, this, &BigPictureWindow::PollController);

  m_game_timer = new QTimer(this);
  m_game_timer->setInterval(30);
  connect(m_game_timer, &QTimer::timeout, this, &BigPictureWindow::PollInGame);

  m_anim_timer = new QTimer(this);
  m_anim_timer->setInterval(16);
  m_anim_timer->setTimerType(Qt::PreciseTimer);
  connect(m_anim_timer, &QTimer::timeout, this, &BigPictureWindow::Tick);

  m_setup_timer = new QTimer(this);
  m_setup_timer->setInterval(10);
  connect(m_setup_timer, &QTimer::timeout, this, &BigPictureWindow::UpdateSetup);

  // Devices are enumerated asynchronously at startup: check a bit later whether port 1 works.
  m_setup_check_timer = new QTimer(this);
  m_setup_check_timer->setSingleShot(true);
  m_setup_check_timer->setInterval(2500);
  connect(m_setup_check_timer, &QTimer::timeout, this, [this] {
    if (m_active && isVisible() && m_mode == Mode::Library && NeedsSetup())
      StartSetup(0);
  });

  m_rebuild_timer = new QTimer(this);
  m_rebuild_timer->setSingleShot(true);
  m_rebuild_timer->setInterval(150);
  connect(m_rebuild_timer, &QTimer::timeout, this, &BigPictureWindow::RebuildList);

  connect(&m_model, &QAbstractItemModel::rowsInserted, this, &BigPictureWindow::ScheduleRebuild);
  connect(&m_model, &QAbstractItemModel::rowsRemoved, this, &BigPictureWindow::ScheduleRebuild);
  connect(&m_model, &QAbstractItemModel::modelReset, this, &BigPictureWindow::ScheduleRebuild);
  connect(&m_model, &QAbstractItemModel::dataChanged, this, [this] {
    // Covers are often downloaded after the game was added to the list.
    m_cover_cache.clear();
    m_banner_cache.clear();
    m_ambient_cache.clear();
    ScheduleRebuild();
  });

  ApplyPreferences();
  RebuildList();
}

BigPictureWindow::~BigPictureWindow()
{
  BigPictureDisplay::RestoreHdr();
  delete m_notifier;
}

void BigPictureWindow::ApplyTheme()
{
  const Palette pal = PaletteFor(Prefs::GetTheme());
  BG = pal.bg;
  SURFACE = pal.surface;
  SURFACE_DEEP = pal.deep;
  RAISED = pal.raised;
  LINE = pal.line;
  TEXT = pal.text;
  TEXT_SOFT = pal.soft;
  TEXT_DIM = pal.dim;
  ACCENT = pal.accent;
  ACCENT_SOFT = pal.accent_soft;
  if (Prefs::GetTheme() == Prefs::Theme::GameColor && m_ambient.isValid())
  {
    // Accent follows the selected game's cover colour.
    ACCENT = m_ambient;
    ACCENT_SOFT = m_ambient.lighter(150);
  }
}

void BigPictureWindow::ApplyPreferences()
{
  m_view = Prefs::GetView();
  m_effect = Prefs::GetEffect();
  m_transition = Prefs::GetTransition();
  ApplyTheme();
  m_audio->SetVolumes(Prefs::MusicEnabled() ? Prefs::MusicVolume() / 100.0f : 0.0f,
                      Prefs::SfxVolume() / 100.0f);
  update();
}

void BigPictureWindow::Open()
{
  unsetCursor();
  QTimer::singleShot(1200, this, [this] {
    if (m_active && Core::IsUninitialized(Core::System::GetInstance()))
      SyncSaves(false);
  });
  // The controller mode's CRT shaders were removed: don't leave Dolphin pointing at them.
  if (Config::Get(Config::GFX_ENHANCE_POST_SHADER).rfind("crt_manette", 0) == 0)
    Config::SetBaseOrCurrent(Config::GFX_ENHANCE_POST_SHADER, std::string());
  m_resume_known.clear();
  m_resume_shot_path.clear();
  m_hd_known.clear();
  static bool s_cleaned_up = false;
  if (!s_cleaned_up)
  {
    s_cleaned_up = true;
    CleanupLegacyFrontendConfig();
  }
  m_active = true;
  m_game_timer->stop();
  // Ignore buttons that are still held (e.g. the one used to quit the game).
  m_prev_held = ~0u;
  m_last_tick = m_clock.elapsed();

  QTimer::singleShot(1500, this, [this] { CheckDrivers(); });

  static bool s_checked_updates = false;
  if (!s_checked_updates && Prefs::CheckUpdatesOnStart())
  {
    s_checked_updates = true;
    QTimer::singleShot(6000, this, [this] { CheckUpdates(true); });
  }

  if (!m_intro_done && Prefs::IntroEnabled())
  {
    m_mode = Mode::Intro;
    m_intro_t = 0;
    QTimer::singleShot(250, this, [this] { m_audio->Play(Sound::Startup); });
  }
  else
  {
    m_intro_done = true;
    m_mode = Mode::Library;
    m_overlay_t = 0;
    m_trans_t = 1;
    m_audio->StartMusic();
    m_setup_check_timer->start();
  }

  showFullScreen();
  raise();
  activateWindow();
  setFocus();
  m_poll_timer->start();
  m_anim_timer->start();
}

void BigPictureWindow::Suspend()
{
  if (m_mode == Mode::Setup)
    FinishSetup(false);
  m_poll_timer->stop();
  m_anim_timer->stop();
  m_audio->StopMusic();
  hide();
}

void BigPictureWindow::Close()
{
  m_active = false;
  m_game_timer->stop();
  Suspend();
}

void BigPictureWindow::OnEmulationStateChanged(Core::State state)
{
  if (!m_active)
    return;

  switch (state)
  {
  case Core::State::Starting:
    if (m_mode == Mode::Launching)
      break;  // keep the loading screen until the first frames
    [[fallthrough]];
  case Core::State::Running:
    if (m_mode == Mode::Closing)
      break;  // the game is being stopped behind the black veil
    if (m_mode == Mode::Launching)
    {
      // Reveal the game smoothly: the menu fades away over the running game.
      m_mode = Mode::FadingOut;
      m_fade_out = 0;
      m_home_was_down = true;
      break;
    }
    if (m_mode == Mode::FadingOut)
      break;
    if (m_mode == Mode::Pause)
    {
      // Resumed from elsewhere (hotkey, menu): close the pause menu.
      m_mode = Mode::InGame;
      Suspend();
    }
    else if (m_mode != Mode::InGame)
    {
      m_mode = Mode::InGame;
      Suspend();
    }
    m_home_was_down = true;
    m_game_timer->start();
    break;
  case Core::State::Paused:
    break;
  case Core::State::Uninitialized:
    m_game_timer->stop();
    BigPictureDisplay::RestoreHdr();
    if (m_mode == Mode::InGame || m_mode == Mode::Pause || m_mode == Mode::Launching ||
        m_mode == Mode::FadingOut || m_mode == Mode::Closing)
    {
      m_pause_settings = false;
      QTimer::singleShot(0, this, [this] {
        if (m_active)
        {
          setWindowOpacity(1.0);
          Open();
          m_fade_in = 0;  // fade in from black: no flash of the desktop
        }
      });
    }
    break;
  default:
    break;
  }
}

void BigPictureWindow::ScheduleRebuild()
{
  m_rebuild_timer->start();
}

void BigPictureWindow::RebuildList()
{
  m_all.clear();
  std::map<std::string, size_t> first_disc;  // game ID -> entry index

  const int title_column = static_cast<int>(GameListModel::Column::Title);
  const int rows = m_model.rowCount(QModelIndex());
  std::vector<int> later_discs;

  const auto make_entry = [&](int row, std::shared_ptr<const UICommon::GameFile> game) {
    Entry entry;
    entry.game = std::move(game);
    entry.path = QString::fromStdString(entry.game->GetFilePath());
    entry.discs << entry.path;
    entry.title = m_model.data(m_model.index(row, title_column), Qt::DisplayRole).toString();
    if (entry.title.isEmpty())
      entry.title = QString::fromStdString(entry.game->GetFileName());
    return entry;
  };

  for (int row = 0; row < rows; ++row)
  {
    if (!m_model.ShouldDisplayGameListItem(row))
      continue;
    auto game = m_model.GetGameFile(row);
    if (!game)
      continue;
    if (game->GetDiscNumber() > 0)
    {
      later_discs.push_back(row);
      continue;
    }
    if (!game->GetGameID().empty())
      first_disc.emplace(game->GetGameID(), m_all.size());
    m_all.push_back(make_entry(row, game));
  }

  // Multi-disc games: a single entry, Dolphin finds the other disc when booting.
  for (int row : later_discs)
  {
    auto game = m_model.GetGameFile(row);
    const auto it = first_disc.find(game->GetGameID());
    if (it != first_disc.end())
      m_all[it->second].discs << QString::fromStdString(game->GetFilePath());
    else
      m_all.push_back(make_entry(row, game));
  }

  std::sort(m_all.begin(), m_all.end(), [](const Entry& a, const Entry& b) {
    return QString::localeAwareCompare(a.title, b.title) < 0;
  });

  ApplyTab();
}

void BigPictureWindow::ApplyTab()
{
  QString selected_path;
  if (const Entry* sel = Selected())
    selected_path = sel->path;

  m_entries.clear();
  const QSettings& settings = Settings::GetQSettings();
  switch (m_tab)
  {
  case Tab::All:
    m_entries = m_all;
    break;
  case Tab::Favorites:
    for (const Entry& e : m_all)
    {
      if (m_favorites.contains(e.path))
        m_entries.push_back(e);
    }
    break;
  case Tab::Recent:
  {
    std::vector<std::pair<qint64, Entry>> recent;
    for (const Entry& e : m_all)
    {
      const qint64 when = settings.value(LastPlayedKey(e.path), 0).toLongLong();
      if (when > 0)
        recent.emplace_back(when, e);
    }
    std::sort(recent.begin(), recent.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [when, e] : recent)
      m_entries.push_back(std::move(e));
    break;
  }
  case Tab::Settings:
    break;
  }

  // Sorting (not the "Recent" tab, which is already by date)
  const int sort = settings.value(QStringLiteral("bigpicture/sort"), 0).toInt();
  if (m_tab != Tab::Recent && sort != 0)
  {
    static TimePlayed s_played;
    s_played.Reload();
    std::map<QString, qint64> key;
    for (const Entry& e : m_entries)
    {
      key[e.path] = sort == 1 ?
                        settings.value(LastPlayedKey(e.path), 0).toLongLong() :
                        std::chrono::duration_cast<std::chrono::seconds>(
                            s_played.GetTimePlayed(e.game->GetGameID()))
                            .count();
    }
    std::stable_sort(m_entries.begin(), m_entries.end(), [&key](const Entry& a, const Entry& b) {
      return key[a.path] > key[b.path];
    });
  }

  m_selected = 0;
  for (size_t i = 0; i < m_entries.size(); ++i)
  {
    if (m_entries[i].path == selected_path)
      m_selected = static_cast<int>(i);
  }
  m_focus.assign(m_entries.size(), 0.0);
  if (!m_focus.empty())
    m_focus[m_selected] = 1.0;
  m_scroll = std::max(0, m_selected - 2) * 248.0;
  m_list_scroll = std::max(0, m_selected - 3);
  m_hero_t = 0;
  update();
}

const BigPictureWindow::Entry* BigPictureWindow::Selected() const
{
  if (m_selected < 0 || m_selected >= static_cast<int>(m_entries.size()))
    return nullptr;
  return &m_entries[m_selected];
}

void BigPictureWindow::SetSelected(int index)
{
  const int count = static_cast<int>(m_entries.size());
  index = std::clamp(index, 0, std::max(0, count - 1));
  if (index == m_selected || count == 0)
    return;
  m_selected = index;
  m_hero_t = 0;
  m_audio->Play(Sound::Move);
}

void BigPictureWindow::Toast(const QString& text)
{
  m_toast = text;
  m_toast_t = 2.6;
}

void BigPictureWindow::SetMode(Mode mode)
{
  if (mode == Mode::Menu || mode == Mode::ConfirmQuit || mode == Mode::Pause)
    m_overlay_t = 0;
  m_mode = mode;
}

// ================================================================== input

void BigPictureWindow::PollController()
{
  if (!m_active || !isVisible() || !isActiveWindow())
    return;
  if (m_mode == Mode::Setup || m_mode == Mode::InGame || m_mode == Mode::Remap ||
      m_mode == Mode::Launching || m_mode == Mode::FadingOut || m_mode == Mode::Closing)
    return;
  if (m_mode == Mode::PadTest || m_mode == Mode::WiiTest)
  {
    // Every button is being tested: hold START for one second to leave.
    const GCPadStatus st = Pad::GetStatus(m_test_port);
    const qint64 t = m_clock.elapsed();
    if (st.button & PAD_BUTTON_START)
    {
      if (m_test_start_since < 0)
        m_test_start_since = t;
      else if (t - m_test_start_since > 1000)
      {
        m_test_start_since = -1;
        m_audio->Play(Sound::Back);
        BeginTransition(-1);
        m_mode = m_mode_before_players;
        m_prev_held = ~0u;
      }
    }
    else
    {
      m_test_start_since = -1;
    }
    return;
  }

  const GCPadStatus pad = Pad::GetStatus(0);

  u32 held = 0;
  const int x = static_cast<int>(pad.stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const int y = static_cast<int>(pad.stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  if ((pad.button & PAD_BUTTON_UP) || y > STICK_THRESHOLD)
    held |= Bit(int(Action::Up));
  if ((pad.button & PAD_BUTTON_DOWN) || y < -STICK_THRESHOLD)
    held |= Bit(int(Action::Down));
  if ((pad.button & PAD_BUTTON_LEFT) || x < -STICK_THRESHOLD)
    held |= Bit(int(Action::Left));
  if ((pad.button & PAD_BUTTON_RIGHT) || x > STICK_THRESHOLD)
    held |= Bit(int(Action::Right));
  if (pad.button & PAD_BUTTON_A)
    held |= Bit(int(Action::Accept));
  if (pad.button & PAD_BUTTON_B)
    held |= Bit(int(Action::Back));
  if (pad.button & PAD_BUTTON_START)
    held |= Bit(int(Action::Menu));
  if ((pad.button & PAD_TRIGGER_L) || pad.triggerLeft > 200)
    held |= Bit(int(Action::PageUp));
  if ((pad.button & PAD_TRIGGER_R) || pad.triggerRight > 200)
    held |= Bit(int(Action::PageDown));
  if (pad.button & PAD_BUTTON_Y)
    held |= Bit(int(Action::Favorite));
  if (pad.button & PAD_BUTTON_X)
    held |= Bit(int(Action::Options));
  if (pad.button & PAD_TRIGGER_Z)
    held |= Bit(int(Action::Sort));

  const qint64 now = m_clock.elapsed();
  for (int a = 0; a < int(Action::Count); ++a)
  {
    const bool is_held = held & Bit(a);
    const bool was_held = m_prev_held & Bit(a);
    if (is_held && !was_held)
    {
      m_held_since[a] = m_last_repeat[a] = now;
      Press(static_cast<Action>(a));
    }
    else if (is_held && was_held && a <= int(Action::Right))
    {
      if (now - m_held_since[a] > REPEAT_DELAY_MS && now - m_last_repeat[a] > REPEAT_RATE_MS)
      {
        m_last_repeat[a] = now;
        Press(static_cast<Action>(a));
      }
    }
  }
  m_prev_held = held;
}

// While a game runs: the Home button (or START + Z) opens the pause menu.
void BigPictureWindow::PollInGame()
{
  if (!m_active || m_mode != Mode::InGame)
    return;
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) != Core::State::Running)
    return;

  bool home = false;
  if (auto* pad = PadController(0))
  {
    if (const auto device = g_controller_interface.FindDevice(pad->GetDefaultDevice()))
    {
      for (const char* name : {"Guide", "Home"})
      {
        if (const auto* input = device->FindInput(name))
          home |= input->GetState() > 0.5;
      }
    }
  }
  const GCPadStatus status = Pad::GetStatus(0);
  const bool combo = (status.button & PAD_BUTTON_START) && (status.button & PAD_TRIGGER_Z);

  if (system.IsWii())
  {
    // The Wii has its own HOME menu: a short press goes to the game, hold to open ours.
    const qint64 now = m_clock.elapsed();
    if (home && m_home_down_since < 0)
      m_home_down_since = now;
    if (!home)
      m_home_down_since = -1;
    const bool held = m_home_down_since >= 0 && now - m_home_down_since > 700;
    if ((held || combo) && !m_home_was_down)
    {
      m_home_was_down = true;
      OpenPause();
      return;
    }
    m_home_was_down = held || combo;
    return;
  }
  home |= combo;
  if (home && !m_home_was_down)
    OpenPause();
  m_home_was_down = home;
}

void BigPictureWindow::keyPressEvent(QKeyEvent* event)
{
  if (m_mode == Mode::Setup)
  {
    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat())
      FinishSetup(false);
    return;
  }
  if (m_mode == Mode::Remap)
  {
    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat())
      FinishRemap(false);
    return;
  }
  if (m_mode == Mode::PadTest || m_mode == Mode::WiiTest)
  {
    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat())
    {
      BeginTransition(-1);
      m_mode = m_mode_before_players;
    }
    return;
  }
  const bool repeat = event->isAutoRepeat();
  const auto once = [&](Action a) {
    if (!repeat)
      Press(a);
  };
  switch (event->key())
  {
  case Qt::Key_Up:
    Press(Action::Up);
    break;
  case Qt::Key_Down:
    Press(Action::Down);
    break;
  case Qt::Key_Left:
    Press(Action::Left);
    break;
  case Qt::Key_Right:
    Press(Action::Right);
    break;
  case Qt::Key_Return:
  case Qt::Key_Enter:
  case Qt::Key_Space:
    once(Action::Accept);
    break;
  case Qt::Key_Escape:
  case Qt::Key_Backspace:
    once(Action::Back);
    break;
  case Qt::Key_Tab:
  case Qt::Key_F1:
    once(Action::Menu);
    break;
  case Qt::Key_PageUp:
  case Qt::Key_Q:
    once(Action::PageUp);
    break;
  case Qt::Key_PageDown:
  case Qt::Key_E:
    once(Action::PageDown);
    break;
  case Qt::Key_F:
    once(Action::Favorite);
    break;
  case Qt::Key_S:
    once(Action::Sort);
    break;
  default:
    QWidget::keyPressEvent(event);
  }
}

void BigPictureWindow::Press(Action action)
{
  if (m_mode == Mode::Intro)
  {
    // Any button skips the intro.
    if (action == Action::Accept || action == Action::Back || action == Action::Menu)
      m_intro_t = std::max<qreal>(m_intro_t, 0.92);
    return;
  }
  HandleAction(action);
  update();
}

void BigPictureWindow::ToggleFavorite()
{
  const Entry* sel = Selected();
  if (!sel)
    return;
  const bool added = !m_favorites.contains(sel->path);
  if (added)
    m_favorites.insert(sel->path);
  else
    m_favorites.remove(sel->path);
  Settings::GetQSettings().setValue(QStringLiteral("bigpicture/favorites"),
                                    QStringList(m_favorites.begin(), m_favorites.end()));
  m_audio->Play(Sound::Toggle);
  Toast(added ? tr("Ajouté aux favoris") : tr("Retiré des favoris"));
  if (m_tab == Tab::Favorites)
    ApplyTab();
}

void BigPictureWindow::HandleAction(Action action)
{
  switch (m_mode)
  {
  case Mode::Library:
    if (m_tab == Tab::Settings)
      HandleSettings(action);
    else
      HandleLibrary(action);
    break;
  case Mode::Menu:
    HandleMenu(action);
    break;
  case Mode::GameSettings:
    HandleSettings(action);
    break;
  case Mode::Memcard:
    HandleMemcard(action);
    break;
  case Mode::WiiSaves:
    HandleWiiSaves(action);
    break;
  case Mode::Players:
    if (action == Action::Back || action == Action::Menu)
    {
      m_audio->Play(Sound::Back);
      BeginTransition(-1);
      SaveBigPictureSettings();
      m_mode = m_mode_before_players;
    }
    else if (action == Action::Options)
    {
      // Back to a single player: ports 2-4 off.
      for (int port = 1; port < 4; ++port)
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(port),
                                 SerialInterface::SIDEVICE_NONE);
      m_audio->Play(Sound::Erase);
    }
    break;
  case Mode::ChooseDisc:
  {
    const int n = static_cast<int>(m_choose_entry.discs.size());
    if (action == Action::Left || action == Action::Up)
    {
      m_choose_disc = std::max(0, m_choose_disc - 1);
      m_audio->Play(Sound::Move);
    }
    else if (action == Action::Right || action == Action::Down)
    {
      m_choose_disc = std::min(n - 1, m_choose_disc + 1);
      m_audio->Play(Sound::Move);
    }
    else if (action == Action::Accept)
    {
      Launch(m_choose_entry, m_choose_disc, false);
    }
    else if (action == Action::Back)
    {
      m_audio->Play(Sound::Cancel);
      m_mode = Mode::Library;
    }
    break;
  }
  case Mode::ConfirmDriver:
    if (action == Action::Accept && !m_driver_busy)
    {
      m_audio->Play(Sound::Confirm);
      InstallDrivers();
    }
    else if (action == Action::Back && !m_driver_busy)
    {
      m_audio->Play(Sound::Cancel);
      BeginTransition(0);
      m_mode = m_mode_before_driver;
    }
    break;
  case Mode::ConfirmUpdate:
    if (action == Action::Accept && !m_update_busy)
    {
      m_update_busy = true;
      m_audio->Play(Sound::Confirm);
      Toast(tr("Téléchargement de la mise à jour…"));
      BigPictureUpdater::Install(this, m_update_url, [this](bool ok, const QString& error) {
        m_update_busy = false;
        if (!ok)
        {
          m_audio->Play(Sound::Error);
          Toast(error);
          m_mode = Mode::Library;
          return;
        }
        Toast(tr("Installation… Dolphin va redémarrer"));
        QTimer::singleShot(800, this, [this] { emit QuitRequested(); });
      });
    }
    else if (action == Action::Back && !m_update_busy)
    {
      m_audio->Play(Sound::Cancel);
      BeginTransition(0);
      m_mode = Mode::Library;
    }
    break;
  case Mode::Calibrate:
    if (action == Action::Accept)
      FinishCalibration(true);
    else if (action == Action::Back)
      FinishCalibration(false);
    break;
  case Mode::Pause:
    if (m_pause_settings)
      HandleSettings(action);
    else
      HandlePause(action);
    break;
  case Mode::ConfirmQuit:
    if (action == Action::Accept)
    {
      m_audio->Play(Sound::Confirm);
      m_mode = Mode::Library;
      emit QuitRequested();
    }
    else if (action == Action::Back)
    {
      m_audio->Play(Sound::Cancel);
      BeginTransition(0);
      m_mode = Mode::Library;
    }
    break;
  default:
    break;
  }
}

void BigPictureWindow::HandleLibrary(Action action)
{
  const bool list = m_view == Prefs::View::List;
  switch (action)
  {
  case Action::Right:
  case Action::Left:
  {
    if (list)
      break;
    const int dir = action == Action::Right ? 1 : -1;
    if (m_hero_focus)
    {
      if (const Entry* sel = Selected())
      {
        const int n = static_cast<int>(HeroButtons(*sel).size());
        const int next = std::clamp(m_hero_button + dir, 0, n - 1);
        if (next != m_hero_button)
        {
          m_hero_button = next;
          m_audio->Play(Sound::Move);
        }
      }
      break;
    }
    SetSelected(m_selected + dir);
    break;
  }
  case Action::Down:
    if (!list)
    {
      // Back from the buttons to the covers
      if (m_hero_focus)
      {
        m_hero_focus = false;
        m_audio->Play(Sound::Move);
      }
      break;
    }
    SetSelected(m_selected + 1);
    break;
  case Action::Up:
    if (!list)
    {
      // Up: the game's buttons (Reprendre, Nouvelle partie, Réglages, Favori)
      if (!m_hero_focus && Selected())
      {
        m_hero_focus = true;
        m_hero_button = 0;
        m_audio->Play(Sound::Move);
      }
      break;
    }
    SetSelected(m_selected - 1);
    break;
  case Action::PageDown:
  case Action::PageUp:
  {
    const int dir = action == Action::PageDown ? 1 : -1;
    BeginTransition(dir);
    m_tab = static_cast<Tab>((static_cast<int>(m_tab) + 4 + dir) % 4);
    m_audio->Play(Sound::Page);
    m_settings_in_items = false;
    m_hero_focus = false;
    ApplyTab();
    break;
  }
  case Action::Accept:
    if (const Entry* sel = Selected())
    {
      if (m_hero_focus)
      {
        const auto buttons = HeroButtons(*sel);
        ActivateHero(buttons[std::clamp(m_hero_button, 0, int(buttons.size()) - 1)]);
      }
      else if (HasResume(*sel))
      {
        Launch(*sel, 0, true);
      }
      else
      {
        StartNewGame(*sel);
      }
    }
    else
    {
      m_audio->Play(Sound::Error);
    }
    break;
  case Action::Sort:
    CycleSort();
    break;
  case Action::Favorite:
    ToggleFavorite();
    break;
  case Action::Options:
    if (Selected())
      OpenGameSettings();
    break;
  case Action::Back:
    m_audio->Play(Sound::Back);
    SetMode(Mode::ConfirmQuit);
    break;
  case Action::Menu:
    m_audio->Play(Sound::MenuOpen);
    SetMode(Mode::Menu);
    m_menu_index = 0;
    break;
  default:
    break;
  }
}

void BigPictureWindow::HandleSettings(Action action)
{
  const int cats = static_cast<int>(Cats().size());
  m_settings_cat = std::clamp(m_settings_cat, 0, std::max(0, cats - 1));
  auto& items = Cats()[m_settings_cat].items;
  const int count = static_cast<int>(items.size());

  // L / R still switch tabs in the library.
  if ((action == Action::PageUp || action == Action::PageDown) && !m_pause_settings &&
      m_mode != Mode::GameSettings)
  {
    SaveBigPictureSettings();
    HandleLibrary(action);
    return;
  }

  if (!m_settings_in_items)
  {
    switch (action)
    {
    case Action::Down:
      if (m_settings_cat < cats - 1)
      {
        m_settings_cat++;
        m_settings_item = 0;
        m_settings_scroll = 0;
        m_audio->Play(Sound::Move);
      }
      break;
    case Action::Up:
      if (m_settings_cat > 0)
      {
        m_settings_cat--;
        m_settings_item = 0;
        m_settings_scroll = 0;
        m_audio->Play(Sound::Move);
      }
      break;
    case Action::Right:
    case Action::Accept:
      m_settings_in_items = true;
      m_settings_item = 0;
      m_audio->Play(Sound::Confirm);
      break;
    case Action::Back:
      SaveBigPictureSettings();
      m_audio->Play(Sound::Back);
      BeginTransition(-1);
      if (m_mode == Mode::GameSettings)
      {
        if (m_game_layer)
          m_game_layer->Save();
        m_game_layer.reset();
        m_game_settings.clear();
        m_settings_cat = 0;
        m_mode = m_mode_before_game_settings;
        Toast(tr("Réglages du jeu enregistrés"));
      }
      else if (m_pause_settings)
      {
        m_pause_settings = false;
      }
      else
      {
        m_tab = Tab::All;
        ApplyTab();
      }
      break;
    case Action::Menu:
      if (!m_pause_settings)
      {
        m_audio->Play(Sound::MenuOpen);
        SetMode(Mode::Menu);
        m_menu_index = 0;
      }
      break;
    default:
      break;
    }
    return;
  }

  BigPictureSettingItem& item = items[std::clamp(m_settings_item, 0, std::max(0, count - 1))];
  switch (action)
  {
  case Action::Down:
    if (m_settings_item < count - 1)
    {
      m_settings_item++;
      m_audio->Play(Sound::Move);
    }
    break;
  case Action::Up:
    if (m_settings_item > 0)
    {
      m_settings_item--;
      m_audio->Play(Sound::Move);
    }
    break;
  case Action::Left:
  case Action::Right:
    if (item.kind == BigPictureSettingItem::Kind::Action)
    {
      if (action == Action::Left)
      {
        m_settings_in_items = false;
        m_audio->Play(Sound::Back);
      }
      break;
    }
    item.change(action == Action::Right ? 1 : -1);
    m_audio->Play(Sound::Toggle);
    break;
  case Action::Accept:
    if (item.activate)
      item.activate();
    m_audio->Play(item.kind == BigPictureSettingItem::Kind::Action ? Sound::Confirm :
                                                                      Sound::Toggle);
    break;
  case Action::Back:
    m_settings_in_items = false;
    m_audio->Play(Sound::Back);
    SaveBigPictureSettings();
    if (m_pause_settings && m_quick)
    {
      BeginTransition(-1);
      m_pause_settings = false;
      m_quick = false;
    }
    break;
  case Action::Options:
    if (item.clear)
    {
      item.clear();
      m_audio->Play(Sound::Erase);
    }
    break;
  default:
    break;
  }
}

void BigPictureWindow::HandleMenu(Action action)
{
  constexpr int ITEMS = 9;
  switch (action)
  {
  case Action::Down:
    m_menu_index = (m_menu_index + 1) % ITEMS;
    m_audio->Play(Sound::Move);
    break;
  case Action::Up:
    m_menu_index = (m_menu_index + ITEMS - 1) % ITEMS;
    m_audio->Play(Sound::Move);
    break;
  case Action::Back:
  case Action::Menu:
    m_audio->Play(Sound::Back);
    BeginTransition(0);
    m_mode = Mode::Library;
    break;
  case Action::Accept:
    m_audio->Play(Sound::Confirm);
    BeginTransition(0);
    m_mode = Mode::Library;
    switch (m_menu_index)
    {
    case 1:
      m_tab = Tab::Settings;
      m_settings_in_items = false;
      break;
    case 2:
      m_mode_before_players = Mode::Library;
      m_mode = Mode::Players;
      break;
    case 3:
      LaunchWiiMenu();
      break;
    case 4:
      OpenMemcards();
      break;
    case 5:
      OpenWiiSaves();
      break;
    case 6:
      StartSetup(0);
      break;
    case 7:
      emit ClassicRequested();
      break;
    case 8:
      SetMode(Mode::ConfirmQuit);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

// ================================================================== pause menu

std::vector<int> BigPictureWindow::PauseItems() const
{
  std::vector<int> items = {PAUSE_RESUME, PAUSE_SAVE, PAUSE_LOAD, PAUSE_QUICK};
  if (m_running_entry.game)
    items.push_back(PAUSE_CHEATS);
  items.push_back(PAUSE_SETTINGS);
  if (m_running_entry.discs.size() > 1)
    items.push_back(PAUSE_DISC);
  items.push_back(PAUSE_SCREENSHOT);
  items.push_back(PAUSE_QUIT);
  return items;
}

QString BigPictureWindow::PauseShotPath() const
{
  return QString::fromStdString(File::GetUserPath(D_SCREENSHOTS_IDX) +
                                SConfig::GetInstance().GetGameID() + "/bigpicture_pause.png");
}

QString BigPictureWindow::SlotThumbPath(int slot) const
{
  return QString::fromStdString(File::GetUserPath(D_STATESAVES_IDX) +
                                SConfig::GetInstance().GetGameID()) +
         QStringLiteral(".s%1.png").arg(slot, 2, 10, QLatin1Char('0'));
}

void BigPictureWindow::LoadSlotThumbs()
{
  for (int s = 1; s <= SLOT_COUNT; ++s)
    m_slot_thumbs[s] = QPixmap(SlotThumbPath(s));
}

void BigPictureWindow::OpenPause()
{
  auto& system = Core::System::GetInstance();
  // Capture the current image for the save-state thumbnails, then pause.
  QFile::remove(PauseShotPath());
  Core::SaveScreenShot("bigpicture_pause");
  m_game_aspect = GameAspect();
  QTimer::singleShot(150, this, [this] {
    if (m_mode == Mode::Pause)
      Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  });
  (void)system;
  LoadSlotThumbs();
  m_mode = Mode::Pause;
  m_overlay_t = 0;
  m_pause_index = 0;
  m_pause_settings = false;
  m_prev_held = ~0u;
  m_last_tick = m_clock.elapsed();
  showFullScreen();
  raise();
  activateWindow();
  setFocus();
  m_poll_timer->start();
  m_anim_timer->start();
  m_audio->Play(Sound::MenuOpen);
}

void BigPictureWindow::Resume()
{
  SaveBigPictureSettings();
  m_mode = Mode::InGame;
  m_pause_settings = false;
  m_poll_timer->stop();
  m_anim_timer->stop();
  hide();
  m_home_was_down = true;
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  emit ResumeRequested();
}

void BigPictureWindow::HandlePause(Action action)
{
  const std::vector<int> items = PauseItems();
  const int n = static_cast<int>(items.size());
  m_pause_index = std::clamp(m_pause_index, 0, n - 1);
  const int item = items[m_pause_index];
  auto& system = Core::System::GetInstance();

  switch (action)
  {
  case Action::Down:
    m_pause_index = (m_pause_index + 1) % n;
    m_audio->Play(Sound::Move);
    break;
  case Action::Up:
    m_pause_index = (m_pause_index + n - 1) % n;
    m_audio->Play(Sound::Move);
    break;
  case Action::Left:
  case Action::Right:
    if (item == PAUSE_SAVE || item == PAUSE_LOAD)
    {
      m_slot = (m_slot - 1 + (action == Action::Right ? 1 : -1) + SLOT_COUNT) % SLOT_COUNT + 1;
      m_audio->Play(Sound::Toggle);
    }
    break;
  case Action::Back:
  case Action::Menu:
    m_audio->Play(Sound::Back);
    Resume();
    break;
  case Action::Accept:
    switch (item)
    {
    case PAUSE_RESUME:
      m_audio->Play(Sound::Confirm);
      Resume();
      break;
    case PAUSE_SAVE:
      State::Save(system, static_cast<u32>(m_slot));
      {
        const QString thumb = SlotThumbPath(m_slot);
        QFile::remove(thumb);
        QFile::copy(PauseShotPath(), thumb);
        m_slot_thumbs[m_slot] = QPixmap(thumb);
      }
      m_audio->Play(Sound::SaveState);
      Toast(tr("État sauvegardé dans l'emplacement %1").arg(m_slot));
      break;
    case PAUSE_LOAD:
      if (State::GetUnixTimeOfSlot(static_cast<u32>(m_slot)) == 0)
      {
        m_audio->Play(Sound::Error);
        Toast(tr("L'emplacement %1 est vide").arg(m_slot));
        break;
      }
      m_audio->Play(Sound::LoadState);
      State::Load(system, static_cast<u32>(m_slot));
      QTimer::singleShot(300, this, [this] {
        if (m_mode == Mode::Pause)
          Resume();
      });
      break;
    case PAUSE_SETTINGS:
      m_audio->Play(Sound::Confirm);
      BeginTransition(1);
      m_pause_settings = true;
      m_quick = false;
      m_settings_cat = 2;  // Graphics
      m_settings_in_items = false;
      break;
    case PAUSE_DISC:
    {
      m_disc = (m_disc + 1) % static_cast<int>(m_running_entry.discs.size());
      const std::string path = m_running_entry.discs[m_disc].toStdString();
      system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, path);
      m_audio->Play(Sound::Confirm);
      Toast(tr("Disque %1 inséré").arg(m_disc + 1));
      break;
    }
    case PAUSE_SCREENSHOT:
      Core::SaveScreenShot();
      m_audio->Play(Sound::Confirm);
      Toast(tr("Capture d'écran enregistrée"));
      break;
    case PAUSE_QUIT:
      m_audio->Play(Sound::Back);
      SaveResume();
      Config::SetCurrent(Config::MAIN_CONFIRM_ON_STOP, false);
      // Fade to black over the paused game, then stop: the menu comes back from black.
      m_mode = Mode::Closing;
      m_close_t = 0;
      m_stop_sent = false;
      break;
    case PAUSE_QUICK:
      m_audio->Play(Sound::Confirm);
      BeginTransition(1);
      m_pause_settings = true;
      m_quick = true;
      m_settings_cat = 0;
      m_settings_item = 0;
      m_settings_in_items = true;
      break;
    case PAUSE_CHEATS:
      if (m_running_entry.game)
        OpenGameSettingsFor(m_running_entry, true);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

// ================================================================== animation

void BigPictureWindow::BeginTransition(int direction)
{
  if (m_transition == Prefs::Transition::None || m_frame.isNull() || !isVisible())
    return;
  m_trans_from = m_frame;
  m_trans_dir = direction;
  m_trans_t = 0;
}

void BigPictureWindow::Tick()
{
  const qint64 now = m_clock.elapsed();
  const qreal dt = std::clamp((now - m_last_tick) / 1000.0, 0.0, 0.1);
  m_last_tick = now;
  m_time += dt;

  // Exponential smoothing, frame-rate independent.
  const auto approach = [dt](qreal& value, qreal target, qreal speed) {
    value += (target - value) * (1 - std::exp(-speed * dt));
    if (std::abs(target - value) < 0.0005)
      value = target;
  };

  if (m_mode == Mode::Intro)
  {
    m_intro_t += dt / (Prefs::GetIntroStyle() == Prefs::IntroStyle::GameCube ? 4.6 : 3.0);
    if (m_intro_t >= 1)
    {
      m_intro_done = true;
      m_mode = Mode::Library;
      m_trans_from = m_frame;
      m_trans_dir = 0;
      m_trans_t = m_frame.isNull() ? 1 : 0;
      m_audio->StartMusic();
      m_setup_check_timer->start();
    }
  }

  if (m_mode == Mode::FadingOut)
  {
    m_fade_out = std::min<qreal>(1, m_fade_out + dt / 0.45);
    setWindowOpacity(1.0 - EaseInOut(m_fade_out));
    if (m_fade_out >= 1)
    {
      m_mode = Mode::InGame;
      Suspend();
      setWindowOpacity(1.0);
      m_game_timer->start();
      emit ResumeRequested();
      return;
    }
  }
  if (m_mode == Mode::Remap && now - m_remap_started > 8000)
  {
    Toast(tr("Aucune touche détectée"));
    FinishRemap(false);
  }
  if (m_mode == Mode::Calibrate)
    UpdateCalibration();
  if (m_fade_in < 1)
    m_fade_in = std::min<qreal>(1, m_fade_in + dt / 0.6);
  if (m_mode == Mode::Closing)
  {
    m_close_t = std::min<qreal>(1, m_close_t + dt / 0.9);
    if (m_close_t >= 1 && !m_stop_sent)
    {
      m_stop_sent = true;
      emit StopRequested();
    }
  }
  if (Prefs::GetTheme() == Prefs::Theme::GameColor && m_ambient.isValid())
  {
    ACCENT = m_ambient;
    ACCENT_SOFT = m_ambient.lighter(150);
  }

  for (size_t i = 0; i < m_focus.size(); ++i)
    approach(m_focus[i], static_cast<int>(i) == m_selected ? 1.0 : 0.0, 14);
  approach(m_scroll, std::max(0, m_selected - 2) * 248.0, 10);
  approach(m_list_scroll, std::max(0, m_selected - 3), 10);
  m_hero_t = std::min<qreal>(1, m_hero_t + dt / 0.35);
  m_overlay_t = std::min<qreal>(1, m_overlay_t + dt / 0.28);
  if (m_trans_t < 1)
    m_trans_t = std::min<qreal>(1, m_trans_t + dt / 0.38);
  if (m_toast_t > 0)
    m_toast_t = std::max<qreal>(0, m_toast_t - dt);

  if (m_settings_in_items || m_pause_settings || m_tab == Tab::Settings)
    approach(m_settings_scroll, std::max(0, m_settings_item - 5), 12);

  // Ambient glow follows the selected game's cover colour.
  QColor target = SURFACE;
  if (const Entry* sel = Selected(); sel && m_tab != Tab::Settings)
    target = AmbientFor(*sel);
  if (!m_ambient.isValid())
    m_ambient = target;
  const qreal k = 1 - std::exp(-4 * dt);
  m_ambient = QColor::fromRgbF(
      static_cast<float>(m_ambient.redF() + (target.redF() - m_ambient.redF()) * k),
      static_cast<float>(m_ambient.greenF() + (target.greenF() - m_ambient.greenF()) * k),
      static_cast<float>(m_ambient.blueF() + (target.blueF() - m_ambient.blueF()) * k));

  update();
}

// ================================================================== assets

QPixmap BigPictureWindow::CoverFor(const Entry& entry, const QSize& size)
{
  const QString key = entry.path + QStringLiteral("@%1x%2").arg(size.width()).arg(size.height());
  const auto it = m_cover_cache.constFind(key);
  if (it != m_cover_cache.constEnd())
    return *it;

  QPixmap pixmap;
  const auto& cover = entry.game->GetCoverImage();
  if (!cover.empty() &&
      pixmap.loadFromData(cover.buffer.data(), static_cast<uint>(cover.buffer.size())))
  {
    pixmap = pixmap.scaled(size * devicePixelRatioF(), Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(devicePixelRatioF());
  }
  else
  {
    pixmap = QPixmap();
  }
  if (m_cover_cache.size() > 400)
    m_cover_cache.clear();
  m_cover_cache.insert(key, pixmap);
  return pixmap;
}

QPixmap BigPictureWindow::BannerFor(const Entry& entry, const QSize& size)
{
  const QString key = entry.path + QStringLiteral("@%1x%2").arg(size.width()).arg(size.height());
  const auto it = m_banner_cache.constFind(key);
  if (it != m_banner_cache.constEnd())
    return *it;

  QPixmap pixmap;
  const auto& banner = entry.game->GetBannerImage();
  if (!banner.empty())
  {
    pixmap = ToQPixmap(banner).scaled(size * devicePixelRatioF(), Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(devicePixelRatioF());
  }
  if (m_banner_cache.size() > 400)
    m_banner_cache.clear();
  m_banner_cache.insert(key, pixmap);
  return pixmap;
}

QColor BigPictureWindow::AmbientFor(const Entry& entry)
{
  const auto it = m_ambient_cache.constFind(entry.path);
  if (it != m_ambient_cache.constEnd())
    return *it;

  QColor color(47, 123, 216);  // default: GameCube indigo-blue
  QImage image;
  const auto& cover = entry.game->GetCoverImage();
  const auto& banner = entry.game->GetBannerImage();
  if (!cover.empty())
    image.loadFromData(cover.buffer.data(), static_cast<int>(cover.buffer.size()));
  else if (!banner.empty())
    image = ToQPixmap(banner).toImage();
  if (!image.isNull())
  {
    const QImage px = image.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    color = px.pixelColor(0, 0);
    int h, s, v;
    color.getHsv(&h, &s, &v);
    color = QColor::fromHsv(std::max(h, 0), std::clamp(s + 40, 90, 230), std::clamp(v, 120, 220));
  }
  m_ambient_cache.insert(entry.path, color);
  return color;
}

QString BigPictureWindow::PlayInfo(const Entry& entry) const
{
  QStringList parts;
  const qint64 last =
      Settings::GetQSettings().value(LastPlayedKey(entry.path), 0).toLongLong();
  if (last > 0)
  {
    const QDate day = QDateTime::fromSecsSinceEpoch(last).date();
    const qint64 days = day.daysTo(QDate::currentDate());
    if (days == 0)
      parts << tr("Dernière partie aujourd'hui");
    else if (days == 1)
      parts << tr("Dernière partie hier");
    else
      parts << tr("Dernière partie il y a %1 jours").arg(days);
  }
  else
  {
    parts << tr("Jamais lancé");
  }

  static TimePlayed s_time_played;
  static qint64 s_reloaded = -1;
  const qint64 now = QDateTime::currentSecsSinceEpoch();
  if (now - s_reloaded > 5)
  {
    s_time_played.Reload();
    s_reloaded = now;
  }
  const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
                           s_time_played.GetTimePlayed(entry.game->GetGameID()))
                           .count();
  if (minutes >= 60)
    parts << tr("%1 h %2 de jeu").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
  else if (minutes > 0)
    parts << tr("%1 min de jeu").arg(minutes);
  if (entry.discs.size() > 1)
    parts << tr("%1 disques").arg(entry.discs.size());
  return parts.join(QStringLiteral("  ·  "));
}

QString BigPictureWindow::ControllerName() const
{
  auto* pad = PadController(0);
  if (!pad)
    return {};
  const auto& dq = pad->GetDefaultDevice();
  if (!g_controller_interface.FindDevice(dq))
    return {};
  QString name = QString::fromStdString(dq.name);
  if (name.contains(QStringLiteral("GameCube"), Qt::CaseInsensitive))
    return tr("Manette GameCube");
  if (name.contains(QStringLiteral("Keyboard"), Qt::CaseInsensitive))
    return tr("Clavier");
  if (name.size() > 28)
    name = name.left(26) + QStringLiteral("…");
  return name;
}

// ================================================================== drawing helpers

QFont BigPictureWindow::Font(bool display, int px, int weight) const
{
  QFont font(display ? m_display_family : m_text_family);
  font.setPixelSize(std::max(1, px));
  font.setWeight(static_cast<QFont::Weight>(weight));
  font.setHintingPreference(QFont::PreferNoHinting);
  return font;
}

void BigPictureWindow::DrawIcon(QPainter& painter, const QString& name, const QRectF& rect)
{
  const auto it = m_icons.constFind(name);
  if (it == m_icons.constEnd())
    return;
  // Keep the icon's proportions inside the rectangle.
  const QSizeF size = QSizeF(it->size()).scaled(rect.size(), Qt::KeepAspectRatio);
  const QRectF target(rect.center().x() - size.width() / 2, rect.center().y() - size.height() / 2,
                      size.width(), size.height());
  painter.drawPixmap(target, *it, QRectF(it->rect()));
}

void BigPictureWindow::DrawHints(QPainter& painter,
                                 const std::vector<std::pair<QString, QString>>& left,
                                 const std::vector<std::pair<QString, QString>>& right)
{
  const qreal k = height() / 1080.0;
  const QFont font = Font(false, int(20 * k), 400);
  painter.setFont(font);
  const QFontMetricsF fm(font);
  const qreal icon = 38 * k;
  const qreal y = height() - 44 * k - icon / 2;

  qreal x = 80 * k;
  for (const auto& [btn, label] : left)
  {
    DrawIcon(painter, btn, QRectF(x, y - icon / 2, icon, icon));
    x += icon + 12 * k;
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(x, y - icon / 2, 600 * k, icon), Qt::AlignVCenter, label);
    x += fm.horizontalAdvance(label) + 36 * k;
  }

  x = width() - 80 * k;
  for (auto it = right.rbegin(); it != right.rend(); ++it)
  {
    const qreal tw = fm.horizontalAdvance(it->second);
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(x - tw, y - icon / 2, tw + 2, icon), Qt::AlignVCenter, it->second);
    x -= tw + 10 * k + icon;
    DrawIcon(painter, it->first, QRectF(x, y - icon / 2, icon, icon));
    x -= 36 * k;
  }
}

qreal BigPictureWindow::DrawPillButton(QPainter& painter, qreal x, qreal y, const QString& icon,
                                       const QString& label, bool primary, qreal alpha,
                                       qreal forced_width)
{
  const qreal k = height() / 1080.0;
  const QFont font = Font(false, int((primary ? 24 : 22) * k), primary ? 600 : 500);
  const QFontMetricsF fm(font);
  const qreal icon_size = (primary ? 40 : 36) * k;
  const qreal h = 68 * k;
  const qreal w = forced_width > 0 ?
                      forced_width :
                      16 * k + icon_size + 14 * k + fm.horizontalAdvance(label) + 32 * k;
  const QRectF r(x, y, w, h);
  painter.save();
  painter.setOpacity(painter.opacity() * alpha);
  painter.setPen(Qt::NoPen);
  if (primary)
  {
    // soft glow under the main button
    QColor glow = ACCENT;
    for (int i = 3; i >= 1; --i)
    {
      glow.setAlphaF(0.07f);
      painter.setBrush(glow);
      painter.drawRoundedRect(r.adjusted(-i * 5 * k, -i * 4 * k + 6 * k, i * 5 * k, i * 6 * k),
                              (16 + i * 5) * k, (16 + i * 5) * k);
    }
  }
  painter.setBrush(primary ? ACCENT : RAISED);
  painter.drawRoundedRect(r, 16 * k, 16 * k);
  if (!icon.isEmpty())
    DrawIcon(painter, icon, QRectF(x + 16 * k, y + (h - icon_size) / 2, icon_size, icon_size));
  painter.setFont(font);
  painter.setPen(primary ? BG : TEXT);
  painter.drawText(QRectF(x + 16 * k + icon_size + 14 * k, y, w, h), Qt::AlignVCenter, label);
  painter.restore();
  return w;
}

void BigPictureWindow::DrawToast(QPainter& painter)
{
  if (m_toast_t <= 0 || m_toast.isEmpty())
    return;
  const qreal k = height() / 1080.0;
  const qreal a = std::min<qreal>(1, std::min(m_toast_t / 0.3, (2.6 - m_toast_t) / 0.25));
  const QFont font = Font(false, int(22 * k), 500);
  const qreal w = QFontMetricsF(font).horizontalAdvance(m_toast) + 64 * k;
  const qreal y = height() - 150 * k + (1 - EaseOut(a)) * 24 * k;
  const QRectF r((width() - w) / 2, y, w, 60 * k);
  painter.save();
  painter.setOpacity(a);
  painter.setPen(Qt::NoPen);
  painter.setBrush(TEXT);
  painter.drawRoundedRect(r, 30 * k, 30 * k);
  painter.setFont(font);
  painter.setPen(BG);
  painter.drawText(r, Qt::AlignCenter, m_toast);
  painter.restore();
}

// ================================================================== frame composition

void BigPictureWindow::paintEvent(QPaintEvent*)
{
  const qreal dpr = devicePixelRatioF();
  const QSize px_size = size() * dpr;
  if (m_frame.size() != px_size)
  {
    m_frame = QPixmap(px_size);
    m_frame.setDevicePixelRatio(dpr);
  }
  m_frame.fill(Qt::transparent);
  {
    QPainter p(&m_frame);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);
    DrawScene(p);
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(rect(), Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  if (m_trans_t >= 1 || m_trans_from.isNull())
  {
    painter.drawPixmap(0, 0, m_frame);
    return;
  }

  const qreal e = EaseInOut(m_trans_t);
  const qreal w = width();
  switch (m_transition)
  {
  case Prefs::Transition::Slide:
  {
    const qreal dir = m_trans_dir == 0 ? 1 : m_trans_dir;
    painter.setOpacity(1 - e * 0.6);
    painter.drawPixmap(QPointF(-dir * w * e * 0.35, 0), m_trans_from);
    painter.setOpacity(e);
    painter.drawPixmap(QPointF(dir * w * (1 - e) * 0.35, 0), m_frame);
    break;
  }
  case Prefs::Transition::Zoom:
  {
    const QPointF c = rect().center();
    painter.save();
    painter.translate(c);
    painter.scale(1 + 0.06 * e, 1 + 0.06 * e);
    painter.translate(-c);
    painter.setOpacity(1 - e);
    painter.drawPixmap(0, 0, m_trans_from);
    painter.restore();
    painter.save();
    painter.translate(c);
    painter.scale(0.95 + 0.05 * e, 0.95 + 0.05 * e);
    painter.translate(-c);
    painter.setOpacity(e);
    painter.drawPixmap(0, 0, m_frame);
    painter.restore();
    break;
  }
  default:  // Fade
    painter.drawPixmap(0, 0, m_frame);
    painter.setOpacity(1 - e);
    painter.drawPixmap(0, 0, m_trans_from);
    break;
  }
}

void BigPictureWindow::DrawScene(QPainter& painter)
{
  if (m_mode == Mode::Intro)
  {
    painter.fillRect(rect(), BG);
    DrawIntro(painter);
    return;
  }
  if (m_mode == Mode::Pause || m_mode == Mode::Closing)
  {
    DrawPause(painter);
    DrawToast(painter);
    if (m_mode == Mode::Closing)
    {
      QColor veil = Qt::black;
      veil.setAlphaF(static_cast<float>(EaseInOut(m_close_t)));
      painter.fillRect(rect(), veil);
    }
    return;
  }

  painter.fillRect(rect(), BG);
  if (m_mode == Mode::Setup)
  {
    DrawBackground(painter);
    DrawSetup(painter);
    return;
  }
  if (m_mode == Mode::Remap)
  {
    DrawBackground(painter);
    DrawRemap(painter);
    return;
  }
  if (m_mode == Mode::Calibrate)
  {
    DrawBackground(painter);
    DrawCalibration(painter);
    return;
  }
  if (m_mode == Mode::Players)
  {
    DrawBackground(painter);
    DrawPlayers(painter);
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::PadTest)
  {
    DrawBackground(painter);
    DrawPadTest(painter);
    return;
  }
  if (m_mode == Mode::WiiTest)
  {
    DrawBackground(painter);
    DrawWiiTest(painter);
    return;
  }
  if (m_mode == Mode::WiiSaves)
  {
    DrawBackground(painter);
    DrawTopBar(painter);
    DrawWiiSaves(painter);
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::ChooseDisc)
  {
    DrawBackground(painter);
    DrawTopBar(painter);
    DrawChooseDisc(painter);
    return;
  }
  if (m_mode == Mode::ConfirmDriver)
  {
    DrawBackground(painter);
    DrawTopBar(painter);
    DrawDialog(painter,
               m_driver_busy ? tr("Installation du pilote… (confirme dans Windows)") :
                               tr("%1 branchée : installer son pilote USB ? (une seule fois)")
                                   .arg(m_driver_devices),
               {{QStringLiteral("A"), tr("Installer")}, {QStringLiteral("B"), tr("Plus tard")}});
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::ConfirmUpdate)
  {
    DrawBackground(painter);
    DrawTopBar(painter);
    DrawDialog(painter,
               m_update_busy ? tr("Téléchargement en cours…") :
                               tr("Nouvelle version disponible"),
               {{QStringLiteral("A"), tr("Installer")}, {QStringLiteral("B"), tr("Plus tard")}});
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::Memcard)
  {
    DrawBackground(painter);
    DrawMemcards(painter);
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::GameSettings)
  {
    DrawBackground(painter);
    DrawSettings(painter, 190 * height() / 1080.0);
    DrawToast(painter);
    return;
  }
  if (m_mode == Mode::Launching || m_mode == Mode::FadingOut)
  {
    // Loading screen: the cover in the middle, a spinner, then the game fades in behind.
    const qreal k = height() / 1080.0;
    DrawBackground(painter);
    const QSizeF cs(330 * k, 462 * k);
    const QRectF cr((width() - cs.width()) / 2, height() / 2.0 - cs.height() / 2 - 60 * k,
                    cs.width(), cs.height());
    const QPixmap cover =
        m_running_entry.game ? CoverFor(m_running_entry, cs.toSize()) : QPixmap();
    QPainterPath clip;
    clip.addRoundedRect(cr, 20 * k, 20 * k);
    if (!cover.isNull())
    {
      painter.save();
      painter.setClipPath(clip, Qt::IntersectClip);
      painter.drawPixmap(cr, cover, QRectF(cover.rect()));
      painter.restore();
    }
    else
    {
      painter.fillPath(clip, RAISED);
    }
    painter.setFont(Font(true, int(36 * k), 700));
    painter.setPen(TEXT);
    painter.drawText(QRectF(0, cr.bottom() + 30 * k, width(), 50 * k), Qt::AlignCenter,
                     m_running_entry.title);
    // spinner
    const QPointF sc(width() / 2.0, cr.bottom() + 130 * k);
    for (int i = 0; i < 12; ++i)
    {
      const qreal a = i * std::numbers::pi / 6 + m_time * 5;
      QColor c = TEXT;
      c.setAlphaF(static_cast<float>(0.15 + 0.85 * (i / 11.0)));
      painter.setPen(QPen(c, 4 * k, Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(QPointF(sc.x() + std::cos(a) * 12 * k, sc.y() + std::sin(a) * 12 * k),
                       QPointF(sc.x() + std::cos(a) * 22 * k, sc.y() + std::sin(a) * 22 * k));
    }
    return;
  }

  DrawBackground(painter);
  DrawTopBar(painter);
  if (m_tab == Tab::Settings)
    DrawSettings(painter, 170 * height() / 1080.0);
  else if (m_entries.empty())
    DrawEmpty(painter);
  else if (m_view == Prefs::View::List)
    DrawList(painter);
  else
    DrawCoverShelf(painter);

  if (m_mode == Mode::Menu)
    DrawMenu(painter);
  else if (m_mode == Mode::ConfirmQuit)
    DrawDialog(painter, tr("Quitter Dolphin ?"),
               {{QStringLiteral("A"), tr("Quitter")}, {QStringLiteral("B"), tr("Annuler")}});
  DrawToast(painter);
  if (m_fade_in < 1)
  {
    QColor veil = Qt::black;
    veil.setAlphaF(static_cast<float>(1 - EaseInOut(m_fade_in)));
    painter.fillRect(rect(), veil);
  }
}

// ================================================================== background effects

void BigPictureWindow::DrawBackground(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal t = m_time;
  QColor glow = m_ambient.isValid() ? m_ambient : ACCENT;
  if (m_tab == Tab::Settings && m_mode != Mode::Setup)
    glow = ACCENT;

  const auto halo = [&](qreal strength) {
    const qreal breathe = 1 + 0.05 * std::sin(t * 0.8);
    const QPointF c(width() - 430 * k, 290 * k);
    QRadialGradient g(c, 760 * k * breathe);
    QColor inner = glow;
    inner.setAlphaF(static_cast<float>(0.26 * strength));
    QColor mid = glow;
    mid.setAlphaF(static_cast<float>(0.10 * strength));
    QColor outer = glow;
    outer.setAlphaF(0.0f);
    g.setColorAt(0, inner);
    g.setColorAt(0.55, mid);
    g.setColorAt(1, outer);
    painter.fillRect(rect(), g);
  };

  switch (m_effect)
  {
  case Prefs::Effect::Halo:
    halo(1.0);
    break;

  case Prefs::Effect::Particles:
  {
    halo(0.55);
    painter.save();
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 90; ++i)
    {
      const qreal speed = 0.012 + Hash01(i, 1) * 0.03;
      const qreal x = std::fmod(Hash01(i, 2) + std::sin(t * 0.2 + i) * 0.01 + 1.0, 1.0) * width();
      const qreal y = (1 - std::fmod(Hash01(i, 3) + t * speed, 1.0)) * height();
      const qreal r = (1.5 + Hash01(i, 4) * 4.5) * k;
      const qreal twinkle = 0.5 + 0.5 * std::sin(t * (1 + Hash01(i, 5) * 2) + i);
      QColor c = Hash01(i, 6) > 0.5 ? glow.lighter(140) : TEXT;
      c.setAlphaF(static_cast<float>(0.08 + 0.22 * twinkle));
      painter.setBrush(c);
      painter.drawEllipse(QPointF(x, y), r, r);
    }
    painter.restore();
    break;
  }

  case Prefs::Effect::Cubes:
  {
    halo(0.5);
    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < 11; ++i)
    {
      const qreal size = (70 + Hash01(i, 7) * 170) * k;
      const qreal x = std::fmod(Hash01(i, 8) + t * (0.004 + Hash01(i, 9) * 0.008), 1.2) *
                          (width() + size * 2) -
                      size;
      const qreal y = (0.08 + Hash01(i, 10) * 0.84) * height() + std::sin(t * 0.5 + i) * 18 * k;
      const qreal angle = t * (6 + Hash01(i, 11) * 12) * (i % 2 ? 1 : -1);
      QColor c = i % 3 == 0 ? glow : TEXT;
      c.setAlphaF(static_cast<float>(0.05 + Hash01(i, 12) * 0.08));
      QPen pen(c, (1.5 + Hash01(i, 13)) * k);
      pen.setJoinStyle(Qt::RoundJoin);
      painter.setPen(pen);
      painter.save();
      painter.translate(x, y);
      painter.rotate(angle);
      painter.drawPath(CubePath(size));
      painter.restore();
    }
    painter.restore();
    break;
  }

  case Prefs::Effect::Waves:
  {
    halo(0.55);
    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < 5; ++i)
    {
      QPainterPath path;
      const qreal base = height() * (0.58 + i * 0.075);
      const qreal amp = (18 + i * 9) * k;
      const qreal freq = 0.0022 / k * (1 + i * 0.25);
      const qreal phase = t * (0.35 + i * 0.12) * (i % 2 ? 1 : -1);
      for (int xi = 0; xi <= 96; ++xi)
      {
        const qreal x = width() * xi / 96.0;
        const qreal y = base + std::sin(x * freq + phase) * amp +
                        std::sin(x * freq * 2.3 - phase * 0.7) * amp * 0.35;
        if (xi == 0)
          path.moveTo(x, y);
        else
          path.lineTo(x, y);
      }
      QColor c = i % 2 ? glow : TEXT;
      c.setAlphaF(static_cast<float>(0.07 + 0.035 * i));
      painter.setPen(QPen(c, (2 + i * 0.4) * k));
      painter.drawPath(path);
    }
    painter.restore();
    break;
  }

  case Prefs::Effect::None:
    break;

  case Prefs::Effect::Cube3D:
  {
    halo(0.5);
    // Big wireframe cube slowly turning, like the GameCube BIOS menu.
    const qreal ax = t * 0.21, ay = t * 0.33;
    const QPointF centre(width() * 0.72, height() * 0.42);
    const qreal size = 260 * k;
    const auto project = [&](qreal x, qreal y, qreal z, qreal s) {
      // rotate around Y then X
      const qreal x1 = x * std::cos(ay) + z * std::sin(ay);
      const qreal z1 = -x * std::sin(ay) + z * std::cos(ay);
      const qreal y1 = y * std::cos(ax) - z1 * std::sin(ax);
      const qreal z2 = y * std::sin(ax) + z1 * std::cos(ax);
      const qreal persp = 3.2 / (3.2 + z2);
      return QPointF(centre.x() + x1 * s * persp, centre.y() + y1 * s * persp);
    };
    static constexpr int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                         {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int pass = 0; pass < 2; ++pass)
    {
      const qreal s = pass == 0 ? size : size * 0.42;
      QPointF pts[8];
      for (int v = 0; v < 8; ++v)
        pts[v] = project(v & 1 ? 1 : -1, v & 2 ? 1 : -1, v & 4 ? 1 : -1, s);
      QColor c = pass == 0 ? TEXT : glow;
      c.setAlphaF(pass == 0 ? 0.10f : 0.22f);
      painter.setPen(QPen(c, (pass == 0 ? 2.0 : 3.0) * k));
      for (const auto& e : edges)
        painter.drawLine(pts[e[0]], pts[e[1]]);
    }
    break;
  }
  }
}

void BigPictureWindow::DrawIntro(QPainter& painter)
{
  if (Prefs::GetIntroStyle() == Prefs::IntroStyle::GameCube)
  {
    DrawIntroGameCube(painter);
    return;
  }
  const qreal k = height() / 1080.0;
  const qreal t = m_intro_t;
  const QPointF c(width() / 2.0, height() / 2.0 - 40 * k);

  // Soft glow growing behind the cube
  QRadialGradient g(c, 520 * k * (0.6 + 0.4 * EaseOut(t / 0.5)));
  QColor inner = ACCENT;
  inner.setAlphaF(static_cast<float>(0.30 * EaseOut(t / 0.4) * (1 - EaseInOut((t - 0.8) / 0.2))));
  QColor outer = ACCENT;
  outer.setAlphaF(0.0f);
  g.setColorAt(0, inner);
  g.setColorAt(1, outer);
  painter.fillRect(rect(), g);

  // The cube draws itself, then turns a quarter and settles.
  const qreal draw = EaseInOut(t / 0.45);
  const qreal spin = EaseOut((t - 0.25) / 0.35);
  const qreal size = 230 * k * (0.85 + 0.15 * spin);
  painter.save();
  painter.translate(c);
  painter.rotate(-90 * (1 - spin));
  QPen pen(TEXT, 7 * k);
  pen.setJoinStyle(Qt::RoundJoin);
  pen.setCapStyle(Qt::RoundCap);
  const QPainterPath cube = CubePath(size);
  const qreal len = cube.length() / pen.widthF();
  pen.setDashPattern({len * draw + 0.001, len + 1});
  painter.setPen(pen);
  painter.setOpacity(1 - EaseInOut((t - 0.85) / 0.15));
  painter.drawPath(cube);
  painter.restore();

  // Title
  const qreal title_a = EaseOut((t - 0.42) / 0.25) * (1 - EaseInOut((t - 0.85) / 0.15));
  QFont title = Font(true, int(84 * k), 700);
  title.setLetterSpacing(QFont::AbsoluteSpacing, -2 * k);
  painter.setFont(title);
  painter.setOpacity(title_a);
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, c.y() + 170 * k + (1 - title_a) * 20 * k, width(), 100 * k),
                   Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Dolphin"));
  QFont sub = Font(false, int(22 * k), 600);
  sub.setLetterSpacing(QFont::AbsoluteSpacing, 4 * k);
  painter.setFont(sub);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, c.y() + 272 * k, width(), 40 * k), Qt::AlignHCenter,
                   tr("MODE MANETTE"));
  painter.setOpacity(1);
}

// ================================================================== library screens

void BigPictureWindow::DrawTopBar(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal cy = 84 * k;

  // Logo: cube outline + name
  painter.save();
  painter.translate(100 * k, cy);
  QPen pen(TEXT, 2.5 * k);
  pen.setJoinStyle(Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(CubePath(38 * k));
  painter.restore();
  painter.setFont(Font(true, int(30 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(136 * k, cy - 30 * k, 400 * k, 60 * k), Qt::AlignVCenter,
                   QStringLiteral("Dolphin"));

  // Tabs, with a sliding highlight
  const QString tabs[] = {tr("Bibliothèque"), tr("Récents"), tr("Favoris"), tr("Réglages")};
  const QFontMetricsF fm(Font(false, int(20 * k), 600));
  qreal widths[4];
  qreal pill_w = 12 * k;
  for (int i = 0; i < 4; ++i)
  {
    widths[i] = fm.horizontalAdvance(tabs[i]) + 56 * k;
    pill_w += widths[i] + 6 * k;
  }
  pill_w -= 6 * k;
  const qreal pill_h = 58 * k;
  const qreal pill_x = (width() - pill_w) / 2;
  const QRectF pill(pill_x, cy - pill_h / 2, pill_w, pill_h);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(pill, pill_h / 2, pill_h / 2);

  static qreal s_hl_x = -1, s_hl_w = 0;
  qreal tx = pill_x + 6 * k;
  qreal target_x = tx, target_w = widths[0];
  for (int i = 0; i < 4; ++i)
  {
    if (static_cast<int>(m_tab) == i)
    {
      target_x = tx;
      target_w = widths[i];
    }
    tx += widths[i] + 6 * k;
  }
  if (s_hl_x < 0)
  {
    s_hl_x = target_x;
    s_hl_w = target_w;
  }
  s_hl_x += (target_x - s_hl_x) * 0.25;
  s_hl_w += (target_w - s_hl_w) * 0.25;
  const QRectF hl(s_hl_x, cy - pill_h / 2 + 6 * k, s_hl_w, pill_h - 12 * k);
  painter.setBrush(ACCENT);
  painter.drawRoundedRect(hl, hl.height() / 2, hl.height() / 2);

  tx = pill_x + 6 * k;
  for (int i = 0; i < 4; ++i)
  {
    const QRectF r(tx, cy - pill_h / 2 + 6 * k, widths[i], pill_h - 12 * k);
    const bool active = static_cast<int>(m_tab) == i;
    painter.setFont(Font(false, int(20 * k), active ? 600 : 500));
    painter.setPen(active ? BG : TEXT_SOFT);
    painter.drawText(r, Qt::AlignCenter, tabs[i]);
    tx += widths[i] + 6 * k;
  }
  const qreal icon = 46 * k;
  DrawIcon(painter, QStringLiteral("L"),
           QRectF(pill.left() - 14 * k - icon, cy - icon / 2, icon, icon));
  DrawIcon(painter, QStringLiteral("R"), QRectF(pill.right() + 14 * k, cy - icon / 2, icon, icon));

  // Clock
  const QString clock = QTime::currentTime().toString(QStringLiteral("HH:mm"));
  const QFont clock_font = Font(true, int(26 * k), 500);
  painter.setFont(clock_font);
  painter.setPen(TEXT);
  const qreal clock_w = QFontMetricsF(clock_font).horizontalAdvance(clock);
  const qreal clock_x = width() - 80 * k - clock_w;
  painter.drawText(QRectF(clock_x, cy - 30 * k, clock_w + 4, 60 * k), Qt::AlignVCenter, clock);

  DrawControllerChips(painter, clock_x - 28 * k, cy);
}

void BigPictureWindow::DrawHero(QPainter& painter, const Entry& sel, qreal left, qreal top,
                                qreal max_width)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_hero_t);
  painter.save();
  painter.setOpacity(e);
  painter.translate((1 - e) * 26 * k, 0);

  qreal y = top;
  QStringList meta;
  const auto platform = sel.game->GetPlatform();
  meta << ((platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD) ?
               QStringLiteral("WII") :
               QStringLiteral("GAMECUBE"));
  const QString region = RegionName(sel.game->GetRegion()).toUpper();
  if (!region.isEmpty())
    meta << region;
  if (!sel.game->GetGameID().empty())
    meta << QString::fromStdString(sel.game->GetGameID());
  QFont meta_font = Font(false, int(18 * k), 600);
  meta_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(meta_font);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(left, y, max_width, 28 * k), Qt::AlignVCenter,
                   meta.join(QStringLiteral("   ·   ")));
  y += 40 * k;

  // Title: up to two lines, shrinks if needed.
  const QRectF title_box(left, y, max_width, 170 * k);
  int title_px = int(76 * k);
  QFont title_font = Font(true, title_px, 700);
  title_font.setLetterSpacing(QFont::AbsoluteSpacing, -2 * k);
  QRectF needed;
  for (;;)
  {
    needed = QFontMetricsF(title_font)
                 .boundingRect(title_box, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                               sel.title);
    if (needed.height() <= title_box.height() || title_px <= int(44 * k))
      break;
    title_px -= std::max(1, int(4 * k));
    title_font.setPixelSize(title_px);
  }
  painter.setFont(title_font);
  painter.setPen(TEXT);
  painter.drawText(title_box, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, sel.title);
  y += std::min(needed.height(), title_box.height()) + 18 * k;

  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(left, y, max_width, 32 * k), Qt::AlignVCenter, PlayInfo(sel));
  y += 52 * k;

  qreal bx = left;
  const auto buttons = HeroButtons(sel);
  for (size_t i = 0; i < buttons.size(); ++i)
  {
    const int b = buttons[i];
    qreal w = 0;
    const qreal h = 68 * k;
    if (b == HERO_RESUME)
    {
      // Main button with the picture of the saved moment
      const QFont f1 = Font(false, int(22 * k), 600);
      const QFont f2 = Font(false, int(14 * k), 500);
      const QDateTime when = QFileInfo(ResumeStatePath(sel.game->GetGameID())).lastModified();
      const QString sub = tr("Sauvegarde auto · %1").arg(QLocale().toString(when, QStringLiteral("d MMM, HH:mm")));
      const qreal tw = std::max(QFontMetricsF(f1).horizontalAdvance(tr("Reprendre")),
                                QFontMetricsF(f2).horizontalAdvance(sub));
      w = 10 * k + 84 * k + 12 * k + 36 * k + 12 * k + tw + 26 * k;
      const QRectF r(bx, y, w, h);
      w = DrawPillButton(painter, bx, y, QString(), QString(), true, 1.0, w);
      const QRectF thumb(bx + 10 * k, y + 10 * k, 84 * k, h - 20 * k);
      painter.save();
      QPainterPath clip;
      clip.addRoundedRect(thumb, 10 * k, 10 * k);
      painter.setClipPath(clip);
      const QPixmap& shot = ResumeShot(sel);
      if (!shot.isNull())
        painter.drawPixmap(thumb, shot, QRectF(shot.rect()));
      else
        painter.fillRect(thumb, SURFACE);
      painter.restore();
      DrawIcon(painter, QStringLiteral("A"),
               QRectF(thumb.right() + 12 * k, y + (h - 36 * k) / 2, 36 * k, 36 * k));
      const qreal tx = thumb.right() + 12 * k + 36 * k + 12 * k;
      painter.setPen(BG);
      painter.setFont(f1);
      painter.drawText(QRectF(tx, y + 8 * k, tw + 4, 30 * k), Qt::AlignVCenter, tr("Reprendre"));
      painter.setFont(f2);
      QColor sub_col = BG;
      sub_col.setAlphaF(0.75f);
      painter.setPen(sub_col);
      painter.drawText(QRectF(tx, y + 36 * k, tw + 4, 22 * k), Qt::AlignVCenter, sub);
      (void)r;
    }
    else
    {
      const bool primary = b == HERO_PLAY;
      const QString icon = b == HERO_PLAY ? QStringLiteral("A") :
                           b == HERO_NEW  ? QStringLiteral("Start-Pause") :
                           b == HERO_SETTINGS ? QStringLiteral("X") :
                                                QStringLiteral("Y");
      const QString label = b == HERO_PLAY     ? tr("Jouer") :
                            b == HERO_NEW      ? tr("Nouvelle partie") :
                            b == HERO_SETTINGS ? tr("Réglages") :
                            m_favorites.contains(sel.path) ? tr("Retirer des favoris") :
                                                             tr("Favori");
      w = DrawPillButton(painter, bx, y, icon, label, primary);
    }
    if (m_hero_focus && static_cast<int>(i) == m_hero_button)
    {
      painter.setPen(QPen(ACCENT_SOFT, 3 * k));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(bx, y, w, h).adjusted(-6 * k, -6 * k, 6 * k, 6 * k), 21 * k,
                              21 * k);
    }
    bx += w + 14 * k;
  }
  painter.restore();
}

static void DrawStar(QPainter& painter, const QPointF& c, qreal k)
{
  QPainterPath star;
  for (int p = 0; p < 10; ++p)
  {
    const qreal rad = (p % 2 ? 4.5 : 10) * k;
    const qreal ang = -std::numbers::pi / 2 + p * std::numbers::pi / 5;
    const QPointF pt(c.x() + rad * std::cos(ang), c.y() + rad * std::sin(ang));
    if (p == 0)
      star.moveTo(pt);
    else
      star.lineTo(pt);
  }
  star.closeSubpath();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(13, 11, 26, 200));
  painter.drawEllipse(c, 16 * k, 16 * k);
  painter.setBrush(WARN);
  painter.drawPath(star);
}

void BigPictureWindow::DrawCoverShelf(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal left = 80 * k;
  const Entry* sel = Selected();
  if (!sel)
    return;

  const bool resume = HasResume(*sel);
  DrawHero(painter, *sel, left, 216 * k, resume ? 1000 * k : 1150 * k);
  if (resume)
  {
    const qreal chh = 388 * k, cw = chh * ResumeAspect(*sel);
    DrawResumeCard(painter, *sel, QRectF(width() - 80 * k - cw, 168 * k, cw, chh));
  }

  // ---- Shelf
  const qreal label_y = 576 * k;
  const QString tab_title = m_tab == Tab::Recent    ? tr("Récemment joués") :
                            m_tab == Tab::Favorites ? tr("Favoris") :
                                                      tr("Tous les jeux");
  const QFont label_font = Font(true, int(28 * k), 600);
  painter.setFont(label_font);
  painter.setPen(TEXT);
  painter.drawText(QRectF(left, label_y, 800 * k, 40 * k), Qt::AlignVCenter, tab_title);
  const qreal lw = QFontMetricsF(label_font).horizontalAdvance(tab_title);
  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_DIM);
  painter.drawText(QRectF(left + lw + 14 * k, label_y + 3 * k, 800 * k, 40 * k), Qt::AlignVCenter,
                   tr("%1 / %2").arg(m_selected + 1).arg(m_entries.size()));

  // Sort order, on the right
  if (m_tab != Tab::Recent)
  {
    const int sort = Settings::GetQSettings().value(QStringLiteral("bigpicture/sort"), 0).toInt();
    const QString names[] = {tr("Nom"), tr("Dernière partie"), tr("Temps de jeu")};
    const QString current = names[std::clamp(sort, 0, 2)];
    const QFont bold = Font(false, int(18 * k), 600);
    const qreal bw = QFontMetricsF(bold).horizontalAdvance(current) + 32 * k;
    const qreal right = width() - 80 * k;
    const QRectF chip(right - bw, label_y + 4 * k, bw, 34 * k);
    painter.setPen(QPen(LINE, 1 * k));
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(chip, 14 * k, 14 * k);
    painter.setFont(bold);
    painter.setPen(TEXT);
    painter.drawText(chip, Qt::AlignCenter, current);
    const QFont normal = Font(false, int(18 * k), 500);
    painter.setFont(normal);
    painter.setPen(TEXT_SOFT);
    const QString label = tr("Trier :");
    const qreal lw2 = QFontMetricsF(normal).horizontalAdvance(label);
    painter.drawText(QRectF(chip.left() - 12 * k - lw2, chip.top(), lw2 + 2, chip.height()),
                     Qt::AlignVCenter, label);
    DrawIcon(painter, QStringLiteral("Z"),
             QRectF(chip.left() - 12 * k - lw2 - 46 * k, chip.center().y() - 17 * k, 36 * k,
                    34 * k));
  }

  const qreal cw = 220 * k, ch = 308 * k;
  const qreal extra_w = 30 * k, extra_h = 42 * k;
  const qreal gap = 28 * k;
  const qreal step = cw + gap;
  const qreal bottom = 985 * k;
  const int count = static_cast<int>(m_entries.size());

  painter.save();
  painter.setClipRect(QRectF(0, 600 * k, width(), 400 * k));
  const int first = std::max(0, int(m_scroll * k / step) - 1);
  const int visible = int(width() / step) + 3;
  qreal offset = 0;  // extra width of focused covers before "first"
  for (int i = 0; i < first && i < count; ++i)
    offset += EaseOut(m_focus[i]) * extra_w;

  for (int i = first; i < std::min(count, first + visible); ++i)
  {
    const Entry& e = m_entries[i];
    const qreal f = EaseOut(m_focus[i]);
    const qreal w = cw + extra_w * f;
    const qreal h = ch + extra_h * f;
    const qreal x = left + i * step - m_scroll * k + offset;
    offset += f * extra_w;
    const QRectF r(x, bottom - h, w, h);
    if (r.right() < 0 || r.left() > width())
      continue;

    // Shadow under the focused cover
    if (f > 0.01)
    {
      QRadialGradient sh(QPointF(r.center().x(), r.bottom()), r.width() * 0.8);
      QColor sc = m_ambient.isValid() ? m_ambient : ACCENT;
      sc.setAlphaF(static_cast<float>(0.35 * f));
      QColor so = sc;
      so.setAlphaF(0.0f);
      sh.setColorAt(0, sc);
      sh.setColorAt(1, so);
      painter.fillRect(r.adjusted(-60 * k, r.height() * 0.5, 60 * k, 40 * k), sh);
    }

    QPainterPath clip;
    clip.addRoundedRect(r, 16 * k, 16 * k);
    painter.setOpacity(0.72 + 0.28 * f);
    const QPixmap cover = CoverFor(e, QSize(int(cw + extra_w), int(ch + extra_h)));
    if (!cover.isNull())
    {
      painter.save();
      painter.setClipPath(clip, Qt::IntersectClip);
      painter.drawPixmap(r, cover, QRectF(cover.rect()));
      painter.restore();
    }
    else
    {
      painter.fillPath(clip, RAISED);
      const QPixmap banner = BannerFor(e, QSize(int(w - 28 * k), int((w - 28 * k) / 3)));
      if (!banner.isNull())
        painter.drawPixmap(QPointF(r.left() + 14 * k, r.top() + 18 * k), banner);
      painter.setFont(Font(true, int(24 * k), 700));
      painter.setPen(TEXT);
      painter.drawText(r.adjusted(20 * k, 20 * k, -20 * k, -20 * k),
                       Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, e.title);
    }
    painter.setOpacity(1.0);
    if (m_favorites.contains(e.path))
      DrawStar(painter, QPointF(r.right() - 26 * k, r.top() + 26 * k), k);
    {
      // "HD" when a texture pack is installed for this game
      const QString id = QString::fromStdString(e.game->GetGameID());
      auto hd = m_hd_known.find(id);
      if (hd == m_hd_known.end())
        hd = m_hd_known.insert(id, HasTexturePack(e.game->GetGameID()));
      if (hd.value())
      {
        const QRectF chip(r.left() + 10 * k, r.top() + 10 * k, 44 * k, 26 * k);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(10, 8, 20, 200));
        painter.drawRoundedRect(chip, 8 * k, 8 * k);
        painter.setFont(Font(false, int(14 * k), 800));
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(chip, Qt::AlignCenter, QStringLiteral("HD"));
      }
    }
    if (f > 0.01)
    {
      const qreal pulse = 0.75 + 0.25 * std::sin(m_time * 3.2);
      QColor ring = TEXT;
      ring.setAlphaF(static_cast<float>(f * pulse));
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(ring, 4 * k));
      const qreal grow = 9 * k * f;
      painter.drawRoundedRect(r.adjusted(-grow, -grow, grow, grow), 22 * k, 22 * k);
    }
  }
  painter.restore();

  if (m_mode == Mode::Library)
  {
    const bool can_resume = HasResume(*sel);
    DrawHints(painter, {{QStringLiteral("Control_Stick"), tr("Naviguer")}},
              {{QStringLiteral("A"), m_hero_focus ? tr("Valider") :
                                     can_resume   ? tr("Reprendre") :
                                                    tr("Jouer")},
               {QStringLiteral("Y"), tr("Favori")},
               {QStringLiteral("Z"), tr("Trier")},
               {QStringLiteral("B"), tr("Quitter")},
               {QStringLiteral("Start-Pause"), tr("Menu")}});
  }
}

void BigPictureWindow::DrawList(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const Entry* sel = Selected();
  if (!sel)
    return;

  // ---- Left: list of banners
  const qreal left = 80 * k;
  const qreal top = 170 * k;
  const qreal list_w = 1000 * k;
  const qreal row_h = 112 * k;
  const qreal view_h = height() - top - 110 * k;
  const int count = static_cast<int>(m_entries.size());

  painter.save();
  painter.setClipRect(QRectF(0, top - 10 * k, left + list_w + 40 * k, view_h + 20 * k));
  const int first = std::max(0, int(m_list_scroll) - 1);
  const int last = std::min(count, first + int(view_h / row_h) + 3);
  for (int i = first; i < last; ++i)
  {
    const Entry& e = m_entries[i];
    const qreal f = EaseOut(m_focus[i]);
    const qreal y = top + (i - m_list_scroll) * row_h;
    const QRectF r(left + f * 14 * k, y + 6 * k, list_w - f * 14 * k, row_h - 12 * k);

    painter.setPen(Qt::NoPen);
    QColor bg = RAISED;
    bg.setAlphaF(static_cast<float>(0.45 + 0.55 * f));
    painter.setBrush(bg);
    painter.drawRoundedRect(r, 16 * k, 16 * k);

    // Banner (96x32 in the game, shown 3x)
    const QSizeF bsize(240 * k, 80 * k);
    const QRectF br(r.left() + 10 * k, r.center().y() - bsize.height() / 2, bsize.width(),
                    bsize.height());
    const QPixmap banner = BannerFor(e, bsize.toSize());
    QPainterPath bclip;
    bclip.addRoundedRect(br, 10 * k, 10 * k);
    painter.save();
    painter.setClipPath(bclip, Qt::IntersectClip);
    if (!banner.isNull())
      painter.drawPixmap(br, banner, QRectF(banner.rect()));
    else
      painter.fillRect(br, SURFACE_DEEP);
    painter.restore();

    painter.setOpacity(0.75 + 0.25 * f);
    painter.setFont(Font(true, int(26 * k), 600));
    painter.setPen(TEXT);
    const QFontMetricsF tfm(painter.font());
    const qreal text_x = br.right() + 26 * k;
    painter.drawText(QRectF(text_x, r.top() + 16 * k, r.right() - text_x - 70 * k, 40 * k),
                     Qt::AlignVCenter,
                     tfm.elidedText(e.title, Qt::ElideRight, r.right() - text_x - 70 * k));
    QStringList meta;
    const auto platform = e.game->GetPlatform();
    meta << ((platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD) ?
                 QStringLiteral("Wii") :
                 QStringLiteral("GameCube"));
    const QString region = RegionName(e.game->GetRegion());
    if (!region.isEmpty())
      meta << region;
    if (e.discs.size() > 1)
      meta << tr("%1 disques").arg(e.discs.size());
    painter.setFont(Font(false, int(19 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(text_x, r.top() + 56 * k, 600 * k, 30 * k), Qt::AlignVCenter,
                     meta.join(QStringLiteral("  ·  ")));
    painter.setOpacity(1);
    if (m_favorites.contains(e.path))
      DrawStar(painter, QPointF(r.right() - 34 * k, r.center().y()), k);

    if (f > 0.01)
    {
      const qreal pulse = 0.75 + 0.25 * std::sin(m_time * 3.2);
      QColor ring = TEXT;
      ring.setAlphaF(static_cast<float>(f * pulse));
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(ring, 3.5 * k));
      painter.drawRoundedRect(r.adjusted(-3 * k, -3 * k, 3 * k, 3 * k), 18 * k, 18 * k);
    }
  }
  painter.restore();

  // Scrollbar
  if (count > 1)
  {
    const qreal track_x = left + list_w + 22 * k;
    painter.setPen(Qt::NoPen);
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(QRectF(track_x, top, 6 * k, view_h), 3 * k, 3 * k);
    const qreal thumb_h = std::max<qreal>(40 * k, view_h * std::min(1.0, (view_h / row_h) / count));
    const qreal thumb_y = top + (view_h - thumb_h) * m_selected / std::max(1, count - 1);
    painter.setBrush(TEXT_DIM);
    painter.drawRoundedRect(QRectF(track_x, thumb_y, 6 * k, thumb_h), 3 * k, 3 * k);
  }

  // ---- Right: details of the selected game
  const qreal px = left + list_w + 80 * k;
  const qreal pw = width() - px - 80 * k;
  const qreal e = EaseOut(m_hero_t);
  painter.save();
  painter.setOpacity(e);
  painter.translate((1 - e) * 22 * k, 0);
  const QSizeF cover_size(300 * k, 420 * k);
  const QRectF cr(px, top + 10 * k, cover_size.width(), cover_size.height());
  const QPixmap cover = CoverFor(*sel, cover_size.toSize());
  QPainterPath cclip;
  cclip.addRoundedRect(cr, 18 * k, 18 * k);
  if (!cover.isNull())
  {
    painter.save();
    painter.setClipPath(cclip, Qt::IntersectClip);
    painter.drawPixmap(cr, cover, QRectF(cover.rect()));
    painter.restore();
  }
  else
  {
    painter.fillPath(cclip, RAISED);
    const QPixmap banner = BannerFor(*sel, QSize(int(cr.width() - 40 * k), int((cr.width() - 40 * k) / 3)));
    if (!banner.isNull())
      painter.drawPixmap(QPointF(cr.left() + 20 * k, cr.center().y() - banner.height() / banner.devicePixelRatioF() / 2), banner);
  }
  painter.restore();
  // Title / info / buttons under the cover
  painter.save();
  painter.setOpacity(e);
  qreal y = cr.bottom() + 30 * k;
  QFont tf = Font(true, int(36 * k), 700);
  painter.setFont(tf);
  painter.setPen(TEXT);
  const QRectF tbox(px, y, pw, 96 * k);
  painter.drawText(tbox, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, sel->title);
  y += std::min<qreal>(QFontMetricsF(tf)
                           .boundingRect(tbox, Qt::TextWordWrap | Qt::AlignLeft, sel->title)
                           .height(),
                       96 * k) +
       12 * k;
  painter.setFont(Font(false, int(19 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(px, y, pw, 60 * k), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                   PlayInfo(*sel));
  painter.restore();

  if (m_mode == Mode::Library)
  {
    DrawHints(painter,
              {{QStringLiteral("Control_Stick-U"), QString()},
               {QStringLiteral("Control_Stick-D"), tr("Naviguer")}},
              {{QStringLiteral("A"), HasResume(*sel) ? tr("Reprendre") : tr("Jouer")},
               {QStringLiteral("Y"), tr("Favori")},
               {QStringLiteral("Z"), tr("Trier")},
               {QStringLiteral("B"), tr("Quitter")},
               {QStringLiteral("Start-Pause"), tr("Menu")}});
  }
}

void BigPictureWindow::DrawEmpty(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  painter.setFont(Font(true, int(44 * k), 700));
  painter.setPen(TEXT);
  QString title, help;
  if (m_tab == Tab::Favorites)
  {
    title = tr("Aucun favori pour l'instant");
    help = tr("Dans la bibliothèque, appuie sur Y sur un jeu pour l'ajouter ici.");
  }
  else if (m_tab == Tab::Recent)
  {
    title = tr("Aucun jeu lancé récemment");
    help = tr("Les jeux que tu lances depuis le mode manette apparaîtront ici.");
  }
  else
  {
    title = tr("Aucun jeu");
    help = tr("Ajoute un dossier de jeux depuis l'interface classique (Start > Interface "
              "classique).");
  }
  painter.drawText(QRectF(0, height() / 2.0 - 80 * k, width(), 70 * k), Qt::AlignCenter, title);
  painter.setFont(Font(false, int(24 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, height() / 2.0, width(), 50 * k), Qt::AlignCenter, help);
  if (m_mode == Mode::Library)
  {
    DrawHints(painter, {{QStringLiteral("L"), QString()}, {QStringLiteral("R"), tr("Onglets")}},
              {{QStringLiteral("Start-Pause"), tr("Menu")}});
  }
}

// ================================================================== settings screen

void BigPictureWindow::DrawSettings(QPainter& painter, qreal top)
{
  const qreal k = height() / 1080.0;
  const qreal left = 80 * k;
  const qreal cats_w = 420 * k;
  const auto& all_cats = Cats();
  const int ncat = static_cast<int>(all_cats.size());
  m_settings_cat = std::clamp(m_settings_cat, 0, ncat - 1);

  if (m_pause_settings || m_mode == Mode::GameSettings)
  {
    QFont title = Font(true, int(48 * k), 700);
    painter.setFont(title);
    painter.setPen(TEXT);
    const QString text = m_mode == Mode::GameSettings ? m_game_settings_title : tr("Réglages");
    painter.drawText(QRectF(left, top - 110 * k, width() - 2 * left, 70 * k), Qt::AlignVCenter,
                     QFontMetricsF(title).elidedText(text, Qt::ElideRight, width() - 2 * left));
    if (m_mode == Mode::GameSettings)
    {
      painter.setFont(Font(false, int(20 * k), 400));
      painter.setPen(TEXT_SOFT);
      painter.drawText(QRectF(left, top - 46 * k, width(), 30 * k), Qt::AlignVCenter,
                       tr("« Global » suit les réglages généraux de Dolphin."));
    }
  }

  // Categories
  const qreal cat_h = 62 * k;
  for (int i = 0; i < ncat; ++i)
  {
    const QRectF r(left, top + 20 * k + i * (cat_h + 6 * k), cats_w, cat_h);
    const bool active = i == m_settings_cat;
    if (active)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(m_settings_in_items ? RAISED : TEXT);
      painter.drawRoundedRect(r, 14 * k, 14 * k);
    }
    painter.setFont(Font(false, int(23 * k), active ? 600 : 500));
    painter.setPen(active && !m_settings_in_items ? BG : (active ? TEXT : TEXT_SOFT));
    painter.drawText(r.adjusted(24 * k, 0, 0, 0), Qt::AlignVCenter, all_cats[i].name);
  }

  // Items
  const auto& items = all_cats[m_settings_cat].items;
  const int count = static_cast<int>(items.size());
  const qreal ix = left + cats_w + 60 * k;
  const qreal iw = width() - ix - 96 * k;
  const qreal row_h = 74 * k;
  const qreal list_top = top + 20 * k;
  const qreal view_h = height() - list_top - 230 * k;

  painter.save();
  painter.setClipRect(QRectF(ix - 20 * k, list_top - 6 * k, iw + 40 * k, view_h + 12 * k));
  const qreal scroll = m_settings_in_items ? m_settings_scroll : 0;
  for (int i = 0; i < count; ++i)
  {
    const BigPictureSettingItem& item = items[i];
    const qreal y = list_top + (i - scroll) * (row_h + 6 * k);
    if (y + row_h < list_top - 10 * k || y > list_top + view_h)
      continue;
    const QRectF r(ix, y, iw, row_h);
    const bool focused = m_settings_in_items && i == m_settings_item;
    painter.setPen(Qt::NoPen);
    painter.setBrush(focused ? TEXT : SURFACE);
    painter.drawRoundedRect(r, 16 * k, 16 * k);
    const QColor fg = focused ? BG : TEXT;
    painter.setFont(Font(false, int(23 * k), focused ? 600 : 500));
    painter.setPen(fg);
    painter.drawText(r.adjusted(28 * k, 0, -360 * k, 0), Qt::AlignVCenter, item.label);

    switch (item.kind)
    {
    case BigPictureSettingItem::Kind::Toggle:
    {
      const bool on = item.is_on && item.is_on();
      const QRectF sw(r.right() - 28 * k - 64 * k, r.center().y() - 18 * k, 64 * k, 36 * k);
      painter.setPen(Qt::NoPen);
      painter.setBrush(on ? ACCENT : (focused ? QColor(200, 196, 220) : LINE));
      painter.drawRoundedRect(sw, 18 * k, 18 * k);
      painter.setBrush(Qt::white);
      const qreal kx = on ? sw.right() - 32 * k : sw.left() + 4 * k;
      painter.drawEllipse(QRectF(kx, sw.top() + 4 * k, 28 * k, 28 * k));
      break;
    }
    case BigPictureSettingItem::Kind::Choice:
    {
      const QString value = item.value ? item.value() : QString();
      const QRectF vr(r.right() - 28 * k - 300 * k, r.top(), 300 * k, r.height());
      painter.setFont(Font(false, int(22 * k), 600));
      painter.setPen(fg);
      painter.drawText(vr.adjusted(30 * k, 0, -30 * k, 0), Qt::AlignCenter, value);
      QPen arrow(fg, 2.5 * k);
      arrow.setCapStyle(Qt::RoundCap);
      arrow.setJoinStyle(Qt::RoundJoin);
      painter.setPen(arrow);
      painter.setBrush(Qt::NoBrush);
      const qreal cy = r.center().y();
      QPolygonF l, rr;
      l << QPointF(vr.left() + 12 * k, cy - 8 * k) << QPointF(vr.left() + 4 * k, cy)
        << QPointF(vr.left() + 12 * k, cy + 8 * k);
      rr << QPointF(vr.right() - 12 * k, cy - 8 * k) << QPointF(vr.right() - 4 * k, cy)
         << QPointF(vr.right() - 12 * k, cy + 8 * k);
      painter.drawPolyline(l);
      painter.drawPolyline(rr);
      break;
    }
    case BigPictureSettingItem::Kind::Action:
    {
      if (item.value)
      {
        painter.setFont(Font(false, int(21 * k), 500));
        painter.setPen(focused ? BG : TEXT_SOFT);
        painter.drawText(QRectF(r.right() - 400 * k, r.top(), 340 * k, r.height()),
                         Qt::AlignVCenter | Qt::AlignRight, item.value());
      }
      QPen arrow(fg, 2.5 * k);
      arrow.setCapStyle(Qt::RoundCap);
      painter.setPen(arrow);
      const qreal cx = r.right() - 36 * k, cy = r.center().y();
      QPolygonF chev;
      chev << QPointF(cx - 6 * k, cy - 10 * k) << QPointF(cx + 4 * k, cy)
           << QPointF(cx - 6 * k, cy + 10 * k);
      painter.drawPolyline(chev);
      break;
    }
    }
  }
  painter.restore();

  // Help for the focused option
  QString help;
  if (m_settings_in_items && m_settings_item < count)
    help = items[m_settings_item].help;
  if (!help.isEmpty())
  {
    painter.setFont(Font(false, int(21 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(ix, height() - 190 * k, iw, 70 * k),
                     Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter, help);
  }

  if (m_mode == Mode::Library || m_mode == Mode::Pause || m_mode == Mode::GameSettings)
  {
    if (m_settings_in_items)
    {
      std::vector<std::pair<QString, QString>> right = {
          {QStringLiteral("D-Pad-L"), QString()},
          {QStringLiteral("D-Pad-R"), tr("Modifier")},
          {QStringLiteral("A"), tr("Valider")}};
      if (m_settings_item < count && items[m_settings_item].clear)
        right.push_back({QStringLiteral("X"), tr("Effacer")});
      right.push_back({QStringLiteral("B"), tr("Catégories")});
      DrawHints(painter,
                {{QStringLiteral("D-Pad-U"), QString()}, {QStringLiteral("D-Pad-D"), tr("Choisir")}},
                right);
    }
    else
    {
      DrawHints(painter,
                {{QStringLiteral("D-Pad-U"), QString()},
                 {QStringLiteral("D-Pad-D"), tr("Catégorie")}},
                {{QStringLiteral("A"), tr("Ouvrir")}, {QStringLiteral("B"), tr("Retour")}});
    }
  }
}

// ================================================================== overlays

void BigPictureWindow::DrawMenu(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_overlay_t);
  QColor shade(9, 7, 20);
  shade.setAlphaF(static_cast<float>(0.72 * e));
  painter.fillRect(rect(), shade);

  painter.save();
  painter.translate(-(1 - e) * 120 * k, 0);
  painter.setOpacity(e);
  const QRectF panel(0, 0, 700 * k, height());
  QColor panel_color = SURFACE;
  panel_color.setAlpha(245);
  painter.fillRect(panel, panel_color);

  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(64 * k, 80 * k, 600 * k, 30 * k), Qt::AlignVCenter, tr("MENU"));
  painter.setFont(Font(true, int(48 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(64 * k, 116 * k, 600 * k, 64 * k), Qt::AlignVCenter,
                   QStringLiteral("Dolphin"));

  const QString items[] = {tr("Retour à la bibliothèque"), tr("Réglages"),
                           tr("Joueurs"),                 tr("Menu Wii"),
                           tr("Cartes mémoire"),          tr("Sauvegardes Wii"),
                           tr("Configurer la manette"),   tr("Interface classique"),
                           tr("Quitter Dolphin")};
  qreal y = 205 * k;
  for (int i = 0; i < 9; ++i)
  {
    if (i == 8)
    {
      painter.fillRect(QRectF(64 * k, y + 4 * k, 572 * k, 1 * k), LINE);
      y += 20 * k;
    }
    // items slide in one after the other
    const qreal ie = EaseOut((m_overlay_t - i * 0.06) / 0.7);
    const QRectF r(64 * k - (1 - ie) * 40 * k, y, 572 * k, 62 * k);
    const bool active = i == m_menu_index;
    painter.setOpacity(e * ie);
    if (active)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(ACCENT);
      painter.drawRoundedRect(r, 16 * k, 16 * k);
      DrawIcon(painter, QStringLiteral("A"),
               QRectF(r.right() - 28 * k - 38 * k, r.center().y() - 19 * k, 38 * k, 38 * k));
    }
    painter.setFont(Font(false, int(24 * k), active ? 600 : 500));
    painter.setPen(active ? BG : (i == 8 ? DANGER : TEXT));
    painter.drawText(r.adjusted(28 * k, 0, 0, 0), Qt::AlignVCenter, items[i]);
    y += 68 * k;
  }
  painter.setOpacity(e);

  const qreal icon = 36 * k;
  const qreal hy = height() - 80 * k - icon;
  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_SOFT);
  DrawIcon(painter, QStringLiteral("D-Pad-U"), QRectF(64 * k, hy, icon, icon));
  DrawIcon(painter, QStringLiteral("D-Pad-D"), QRectF(64 * k + icon + 4 * k, hy, icon, icon));
  painter.drawText(QRectF(64 * k + 2 * icon + 16 * k, hy, 200 * k, icon), Qt::AlignVCenter,
                   tr("Choisir"));
  DrawIcon(painter, QStringLiteral("A"), QRectF(300 * k, hy, icon, icon));
  painter.drawText(QRectF(300 * k + icon + 10 * k, hy, 200 * k, icon), Qt::AlignVCenter,
                   tr("Valider"));
  DrawIcon(painter, QStringLiteral("B"), QRectF(470 * k, hy, icon, icon));
  painter.drawText(QRectF(470 * k + icon + 10 * k, hy, 200 * k, icon), Qt::AlignVCenter,
                   tr("Retour"));
  painter.restore();
}

void BigPictureWindow::DrawDialog(QPainter& painter, const QString& title,
                                  const std::vector<std::pair<QString, QString>>& hints)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_overlay_t);
  QColor shade(9, 7, 20);
  shade.setAlphaF(static_cast<float>(0.75 * e));
  painter.fillRect(rect(), shade);

  painter.save();
  const QPointF c = rect().center();
  painter.translate(c);
  const qreal s = 0.92 + 0.08 * e;
  painter.scale(s, s);
  painter.translate(-c);
  painter.setOpacity(e);
  const QRectF box((width() - 760 * k) / 2, (height() - 280 * k) / 2, 760 * k, 280 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(box, 28 * k, 28 * k);

  painter.setFont(Font(true, int(40 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(box.left(), box.top() + 50 * k, box.width(), 70 * k), Qt::AlignCenter,
                   title);

  qreal total = 0;
  for (const auto& [btn, label] : hints)
    total += QFontMetricsF(Font(false, int(22 * k), 500)).horizontalAdvance(label) + 120 * k;
  qreal x = box.center().x() - (total - 16 * k) / 2;
  for (size_t i = 0; i < hints.size(); ++i)
  {
    x += DrawPillButton(painter, x, box.bottom() - 110 * k, hints[i].first, hints[i].second,
                        i == 0) +
         16 * k;
  }
  painter.restore();
}

void BigPictureWindow::DrawPause(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_overlay_t);

  // The game stays visible behind a dark veil.
  QColor shade(9, 7, 20);
  shade.setAlphaF(static_cast<float>(0.72 * e));
  painter.fillRect(rect(), shade);

  if (m_pause_settings)
  {
    QColor deep(13, 11, 26);
    deep.setAlphaF(0.94f);
    painter.fillRect(rect(), deep);
    DrawSettings(painter, 190 * k);
    return;
  }

  painter.save();
  painter.translate(-(1 - e) * 120 * k, 0);
  painter.setOpacity(e);
  painter.fillRect(QRectF(0, 0, 700 * k, height()), QColor(23, 20, 43, 240));

  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(64 * k, 80 * k, 600 * k, 30 * k), Qt::AlignVCenter,
                   tr("JEU EN PAUSE"));
  QFont title = Font(true, int(44 * k), 700);
  title.setLetterSpacing(QFont::AbsoluteSpacing, -1 * k);
  painter.setFont(title);
  painter.setPen(TEXT);
  painter.drawText(QRectF(64 * k, 116 * k, 572 * k, 120 * k),
                   Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                   m_running_entry.title.isEmpty() ? tr("Jeu en cours") : m_running_entry.title);

  const std::vector<int> items = PauseItems();
  qreal y = 280 * k;
  for (int i = 0; i < static_cast<int>(items.size()); ++i)
  {
    QString text;
    switch (items[i])
    {
    case PAUSE_RESUME:
      text = tr("Reprendre");
      break;
    case PAUSE_SAVE:
      text = tr("Sauvegarder l'état");
      break;
    case PAUSE_LOAD:
      text = tr("Charger un état");
      break;
    case PAUSE_SETTINGS:
      text = tr("Tous les réglages");
      break;
    case PAUSE_QUICK:
      text = tr("Réglages rapides");
      break;
    case PAUSE_CHEATS:
      text = tr("Codes de triche");
      break;
    case PAUSE_DISC:
      text = tr("Changer de disque (%1 / %2)").arg(m_disc + 1).arg(m_running_entry.discs.size());
      break;
    case PAUSE_SCREENSHOT:
      text = tr("Capture d'écran");
      break;
    case PAUSE_QUIT:
      text = tr("Quitter le jeu");
      break;
    default:
      break;
    }
    if (items[i] == PAUSE_QUIT)
    {
      painter.fillRect(QRectF(64 * k, y + 2 * k, 572 * k, 1 * k), LINE);
      y += 16 * k;
    }
    const qreal ie = EaseOut((m_overlay_t - i * 0.05) / 0.7);
    const QRectF r(64 * k - (1 - ie) * 40 * k, y, 572 * k, 74 * k);
    const bool active = i == m_pause_index;
    painter.setOpacity(e * ie);
    if (active)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(TEXT);
      painter.drawRoundedRect(r, 16 * k, 16 * k);
    }
    painter.setFont(Font(false, int(25 * k), active ? 600 : 500));
    painter.setPen(active ? BG : (items[i] == PAUSE_QUIT ? DANGER : TEXT));
    painter.drawText(r.adjusted(28 * k, 0, 0, 0), Qt::AlignVCenter, text);
    if (active && (items[i] == PAUSE_SAVE || items[i] == PAUSE_LOAD))
    {
      DrawIcon(painter, QStringLiteral("D-Pad-L"),
               QRectF(r.right() - 150 * k, r.center().y() - 17 * k, 34 * k, 34 * k));
      painter.drawText(QRectF(r.right() - 114 * k, r.top(), 50 * k, r.height()), Qt::AlignCenter,
                       QString::number(m_slot));
      DrawIcon(painter, QStringLiteral("D-Pad-R"),
               QRectF(r.right() - 62 * k, r.center().y() - 17 * k, 34 * k, 34 * k));
    }
    else if (active)
    {
      DrawIcon(painter, QStringLiteral("A"),
               QRectF(r.right() - 24 * k - 36 * k, r.center().y() - 18 * k, 36 * k, 36 * k));
    }
    y += 80 * k;
  }
  painter.setOpacity(e);
  painter.restore();

  // Save slots
  painter.save();
  painter.setOpacity(e);
  const qreal sx = 780 * k;
  const qreal sw = width() - sx - 80 * k;
  painter.setFont(Font(true, int(30 * k), 600));
  painter.setPen(TEXT);
  painter.drawText(QRectF(sx, 80 * k, sw, 44 * k), Qt::AlignVCenter, tr("États sauvegardés"));
  const qreal gap = 22 * k;
  const qreal cw = (sw - gap) / 2;
  const qreal ch = 150 * k;
  for (int s = 1; s <= SLOT_COUNT; ++s)
  {
    const int col = (s - 1) % 2, row = (s - 1) / 2;
    const qreal ce = EaseOut((m_overlay_t - 0.1 - s * 0.03) / 0.6);
    const QRectF r(sx + col * (cw + gap), 150 * k + row * (ch + gap) + (1 - ce) * 20 * k, cw, ch);
    const bool current = s == m_slot;
    painter.setOpacity(e * ce);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(23, 20, 43, 235));
    painter.drawRoundedRect(r, 18 * k, 18 * k);
    if (current)
    {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(TEXT, 3.5 * k));
      painter.drawRoundedRect(r.adjusted(-4 * k, -4 * k, 4 * k, 4 * k), 20 * k, 20 * k);
    }
    const u64 when = State::GetUnixTimeOfSlot(static_cast<u32>(s));
    // Thumbnail on the left of the card, with the game's proportions (4:3 or 16:9)
    const QRectF tr_(r.left() + 12 * k, r.top() + 12 * k, (ch - 24 * k) * m_game_aspect,
                     ch - 24 * k);
    QPainterPath tclip;
    tclip.addRoundedRect(tr_, 10 * k, 10 * k);
    painter.save();
    painter.setClipPath(tclip, Qt::IntersectClip);
    if (when != 0 && !m_slot_thumbs[s].isNull())
    {
      // The capture is the game's picture: shown at the game's proportions
      painter.drawPixmap(tr_, m_slot_thumbs[s], QRectF(m_slot_thumbs[s].rect()));
    }
    else
    {
      painter.fillRect(tr_, SURFACE_DEEP);
      painter.setFont(Font(true, int(40 * k), 700));
      painter.setPen(current ? TEXT : TEXT_DIM);
      painter.drawText(tr_, Qt::AlignCenter, QString::number(s));
    }
    painter.restore();
    const qreal text_x = tr_.right() + 18 * k;
    painter.setFont(Font(false, int(22 * k), 600));
    painter.setPen(TEXT);
    painter.drawText(QRectF(text_x, r.top() + 26 * k, r.right() - text_x - 10 * k, 34 * k),
                     Qt::AlignVCenter, tr("Emplacement %1").arg(s));
    painter.setFont(Font(false, int(19 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(text_x, r.top() + 64 * k, r.right() - text_x - 10 * k, 34 * k),
                     Qt::AlignVCenter,
                     when == 0 ? tr("Libre") :
                                 QDateTime::fromSecsSinceEpoch(static_cast<qint64>(when))
                                     .toString(QStringLiteral("dd/MM/yyyy  HH:mm")));
  }
  painter.restore();

  DrawHints(painter, {}, {{QStringLiteral("A"), tr("Valider")},
                          {QStringLiteral("B"), tr("Reprendre")}});
}

// ================================================================== controller setup

void BigPictureWindow::DrawSetup(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const SetupStep& step = SETUP_STEPS[std::min(m_setup_step, SETUP_STEP_COUNT - 1)];

  QFont label_font = Font(false, int(18 * k), 600);
  label_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label_font);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 110 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("CONFIGURATION DE LA MANETTE — PORT %1").arg(m_setup_port + 1));

  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  QString device_line;
  if (m_setup_device_chosen)
  {
    device_line = tr("Manette détectée : %1")
                      .arg(QString::fromStdString(PadController(m_setup_port)->GetDefaultDevice().name));
  }
  else
  {
    device_line = tr("Appuie sur A sur la manette que tu veux utiliser");
  }
  painter.drawText(QRectF(0, 152 * k, width(), 34 * k), Qt::AlignCenter, device_line);

  // Button to press, gently pulsing
  const QPointF c(width() / 2.0, 420 * k);
  const qreal pulse = 1 + 0.03 * std::sin(m_time * 4);
  painter.setPen(QPen(LINE, 2 * k));
  painter.setBrush(SURFACE_DEEP);
  painter.drawEllipse(c, 166 * k * pulse, 166 * k * pulse);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawEllipse(c, 150 * k, 150 * k);
  DrawIcon(painter, QString::fromLatin1(step.icon),
           QRectF(c.x() - 95 * k, c.y() - 95 * k, 190 * k, 190 * k));

  QFont prompt_font = Font(true, int(64 * k), 700);
  prompt_font.setLetterSpacing(QFont::AbsoluteSpacing, -1.5 * k);
  painter.setFont(prompt_font);
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 634 * k, width(), 80 * k), Qt::AlignCenter,
                   QString::fromUtf8(step.prompt));
  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, 724 * k, width(), 36 * k), Qt::AlignCenter,
                   QString::fromUtf8(step.help));

  // Progress dots
  const qreal dot = 14 * k;
  const qreal wide = 40 * k;
  const qreal dgap = 10 * k;
  const qreal total = (SETUP_STEP_COUNT - 1) * (dot + dgap) + wide;
  qreal x = (width() - total) / 2;
  const qreal dy = 830 * k;
  painter.setPen(Qt::NoPen);
  for (int i = 0; i < SETUP_STEP_COUNT; ++i)
  {
    const bool current = i == m_setup_step;
    painter.setBrush(i < m_setup_step ? ACCENT_SOFT : (current ? TEXT : LINE));
    const qreal w = current ? wide : dot;
    painter.drawRoundedRect(QRectF(x, dy, w, dot), dot / 2, dot / 2);
    x += w + dgap;
  }
  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_DIM);
  painter.drawText(QRectF(0, dy + 30 * k, width(), 34 * k), Qt::AlignCenter,
                   tr("Étape %1 sur %2").arg(m_setup_step + 1).arg(SETUP_STEP_COUNT));

  if (!m_setup_device_chosen)
  {
    painter.drawText(
        QRectF(240 * k, height() - 150 * k, width() - 480 * k, 70 * k),
        Qt::AlignCenter | Qt::TextWordWrap,
        tr("Manette GameCube Switch 2 en USB : si rien ne se passe, installe une fois le pilote "
           "WinUSB avec Zadig sur « Nintendo GameCube Controller (Interface 1) »."));
  }
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, height() - 70 * k, width() - 80 * k, 34 * k),
                   Qt::AlignVCenter | Qt::AlignRight, tr("Échap (clavier) : annuler"));
}

bool BigPictureWindow::NeedsSetup() const
{
  auto* pad = PadController(0);
  if (!pad)
    return false;
  const auto& dq = pad->GetDefaultDevice();
  if (dq.source == "DSUClient")
    return true;
  // With automatic switching, a missing controller is replaced by the next one pressed.
  if (Prefs::AutoSwitchController())
    return false;
  return g_controller_interface.FindDevice(dq) == nullptr;
}

void BigPictureWindow::StartSetup(int port)
{
  auto* pad = PadController(port);
  if (!pad)
    return;
  if (m_mode != Mode::Setup)
    m_mode_before_setup = m_mode == Mode::Menu ? Mode::Library : m_mode;
  BeginTransition(0);
  m_mode = Mode::Setup;
  m_setup_port = port;
  m_setup_step = 0;
  m_setup_device_chosen = false;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    for (PadGroup group : {PadGroup::Buttons, PadGroup::MainStick, PadGroup::CStick,
                           PadGroup::Triggers, PadGroup::DPad})
    {
      for (auto& control : Pad::GetGroup(port, group)->controls)
        (void)control->control_ref->SetExpression("");
    }
  }
  StartSetupStep();
  m_setup_timer->start();
  update();
}

void BigPictureWindow::StartSetupStep()
{
  std::vector<std::string> devices;
  if (m_setup_device_chosen)
  {
    devices.push_back(PadController(m_setup_port)->GetDefaultDevice().ToString());
  }
  else
  {
    // First button: listen to every controller except the keyboard and DSU clients.
    for (const std::string& dev : g_controller_interface.GetAllDeviceStrings())
    {
      if (dev.find("Keyboard") != std::string::npos || dev.rfind("DSUClient", 0) == 0)
        continue;
      devices.push_back(dev);
    }
  }
  m_detector = std::make_unique<ciface::Core::InputDetector>();
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  m_detector->Start(g_controller_interface, devices);
}

void BigPictureWindow::UpdateSetup()
{
  if (m_mode != Mode::Setup || !m_detector)
    return;

  m_detector->Update(std::chrono::seconds(10), std::chrono::milliseconds(0),
                     std::chrono::seconds(10));
  if (!m_detector->IsComplete())
    return;

  auto results = m_detector->TakeResults();
  m_detector.reset();

  if (!ciface::MappingCommon::ContainsCompleteDetection(results))
  {
    // Nothing pressed (or no controller yet): keep waiting.
    StartSetupStep();
    return;
  }

  auto* pad = PadController(m_setup_port);
  if (!m_setup_device_chosen)
  {
    ciface::Core::DeviceQualifier dq;
    dq.FromDevice(results.front().device.get());
    pad->SetDefaultDevice(dq);
    m_setup_device_chosen = true;
  }

  ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
  const std::string expr = ciface::MappingCommon::BuildExpression(
      results, pad->GetDefaultDevice(), ciface::MappingCommon::Quote::On);

  const SetupStep& step = SETUP_STEPS[m_setup_step];
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    auto* group = Pad::GetGroup(m_setup_port, step.group);
    (void)group->controls[step.index]->control_ref->SetExpression(expr);
    if (step.mirror_index >= 0)
    {
      (void)group->controls[step.mirror_index]->control_ref->SetExpression(
          step.flip ? FlipAxis(expr) : expr);
    }
  }

  m_audio->Play(Sound::Confirm);
  if (++m_setup_step >= SETUP_STEP_COUNT)
  {
    FinishSetup(true);
    return;
  }
  // The button has been released (detection waits for it): next step right away.
  StartSetupStep();
}

void BigPictureWindow::FinishSetup(bool save)
{
  m_setup_timer->stop();
  m_detector.reset();
  if (auto* pad = PadController(m_setup_port))
  {
    if (save)
    {
      pad->UpdateReferences(g_controller_interface);
      SaveBigPicturePads();
    }
    else
    {
      // Cancelled: reload what was saved before.
      Pad::LoadConfig();
    }
  }
  BeginTransition(0);
  m_mode = m_mode_before_setup == Mode::Setup ? Mode::Library : m_mode_before_setup;
  m_prev_held = ~0u;
  if (save)
    Toast(tr("Manette configurée"));
  update();
}

// ================================================================== settings sources

std::vector<BigPictureSettingCategory>& BigPictureWindow::Cats()
{
  if (m_mode == Mode::GameSettings)
    return m_game_settings;
  if (m_pause_settings && m_quick)
    return m_quick_settings;
  return m_settings;
}

void BigPictureWindow::OpenGameSettings()
{
  if (const Entry* sel = Selected())
    OpenGameSettingsFor(*sel, false);
}

void BigPictureWindow::OpenGameSettingsFor(const Entry& entry, bool cheats_only)
{
  const std::string id = entry.game->GetGameID();
  const u16 rev = entry.game->GetRevision();
  m_game_layer = std::make_shared<Config::Layer>(
      ConfigLoaders::GenerateLocalGameConfigLoader(id, rev));
  m_game_settings.clear();
  if (!cheats_only)
  {
    m_game_settings = BuildBigPictureGameSettings(m_game_layer);
    for (auto& cat : BuildBigPictureGameExtras(m_game_layer, id, entry.title,
                                               [this](const QString& t) { Toast(t); }))
      m_game_settings.push_back(std::move(cat));
  }
  for (auto& cat : BuildBigPictureCheatSettings(id, rev))
    m_game_settings.push_back(std::move(cat));
  m_game_settings_title = cheats_only ? tr("Codes de triche — %1").arg(entry.title) :
                                        tr("Réglages de %1").arg(entry.title);
  m_mode_before_game_settings = m_mode == Mode::GameSettings ? Mode::Library : m_mode;
  m_settings_cat = 0;
  m_settings_item = 0;
  m_settings_in_items = false;
  m_audio->Play(Sound::Confirm);
  BeginTransition(1);
  m_mode = Mode::GameSettings;
}

// ================================================================== remap one control

void BigPictureWindow::StartRemap(int port, int group, int index, int mirror, bool flip,
                                  const QString& prompt)
{
  if (m_mode != Mode::Remap)
    m_mode_before_remap = m_mode;
  m_remap_port = port;
  m_remap_group = group;
  m_remap_index = index;
  m_remap_mirror = mirror;
  m_remap_flip = flip;
  m_remap_prompt = prompt;
  m_remap_started = m_clock.elapsed();
  BeginTransition(0);
  m_mode = Mode::Remap;
  // Wait for every button to be released (the A used to get here).
  QTimer::singleShot(300, this, [this] {
    if (m_mode == Mode::Remap)
      StartRemapDetection();
  });
  m_setup_timer->stop();
}

void BigPictureWindow::StartRemapDetection()
{
  std::vector<std::string> devices;
  auto* pad = BigPictureController(m_remap_port, m_remap_group);
  if (g_controller_interface.FindDevice(pad->GetDefaultDevice()))
  {
    devices.push_back(pad->GetDefaultDevice().ToString());
  }
  else
  {
    for (const std::string& dev : g_controller_interface.GetAllDeviceStrings())
    {
      if (dev.rfind("DSUClient", 0) != 0)
        devices.push_back(dev);
    }
  }
  m_detector = std::make_unique<ciface::Core::InputDetector>();
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    m_detector->Start(g_controller_interface, devices);
  }
  m_remap_started = m_clock.elapsed();
  QTimer::singleShot(10, this, &BigPictureWindow::UpdateRemap);
}

void BigPictureWindow::UpdateRemap()
{
  if (m_mode != Mode::Remap || !m_detector)
    return;
  m_detector->Update(std::chrono::seconds(8), std::chrono::milliseconds(0),
                     std::chrono::seconds(8));
  if (!m_detector->IsComplete())
  {
    QTimer::singleShot(10, this, &BigPictureWindow::UpdateRemap);
    return;
  }
  auto results = m_detector->TakeResults();
  m_detector.reset();
  if (!ciface::MappingCommon::ContainsCompleteDetection(results))
  {
    FinishRemap(false);
    return;
  }

  auto* pad = BigPictureController(m_remap_port, m_remap_group);
  if (!g_controller_interface.FindDevice(pad->GetDefaultDevice()))
  {
    ciface::Core::DeviceQualifier dq;
    dq.FromDevice(results.front().device.get());
    pad->SetDefaultDevice(dq);
  }
  ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
  const std::string expr = ciface::MappingCommon::BuildExpression(
      results, pad->GetDefaultDevice(), ciface::MappingCommon::Quote::On);
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    auto* group = BigPictureGroup(m_remap_port, m_remap_group);
    (void)group->controls[m_remap_index]->control_ref->SetExpression(expr);
    if (m_remap_mirror >= 0)
    {
      (void)group->controls[m_remap_mirror]->control_ref->SetExpression(
          m_remap_flip ? FlipAxis(expr) : expr);
    }
    pad->UpdateReferences(g_controller_interface);
  }
  SaveBigPicturePads();
  FinishRemap(true);
}

void BigPictureWindow::FinishRemap(bool ok)
{
  m_detector.reset();
  m_audio->Play(ok ? Sound::Confirm : Sound::Cancel);
  BeginTransition(0);
  m_mode = m_mode_before_remap;
  m_prev_held = ~0u;
}

void BigPictureWindow::DrawRemap(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 200 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("MANETTE DU PORT %1").arg(m_remap_port + 1));

  // Countdown ring
  const QPointF c(width() / 2.0, 470 * k);
  const qreal left = std::clamp(1.0 - (m_clock.elapsed() - m_remap_started) / 8000.0, 0.0, 1.0);
  painter.setPen(QPen(LINE, 8 * k));
  painter.setBrush(SURFACE);
  painter.drawEllipse(c, 150 * k, 150 * k);
  QPen ring(TEXT, 8 * k);
  ring.setCapStyle(Qt::RoundCap);
  painter.setPen(ring);
  painter.setBrush(Qt::NoBrush);
  painter.drawArc(QRectF(c.x() - 150 * k, c.y() - 150 * k, 300 * k, 300 * k), 90 * 16,
                  static_cast<int>(360 * 16 * left));
  const qreal pulse = 1 + 0.06 * std::sin(m_time * 6);
  painter.setPen(Qt::NoPen);
  QColor dot = ACCENT;
  dot.setAlphaF(0.35f);
  painter.setBrush(dot);
  painter.drawEllipse(c, 60 * k * pulse, 60 * k * pulse);

  QFont prompt = Font(true, int(56 * k), 700);
  prompt.setLetterSpacing(QFont::AbsoluteSpacing, -1.5 * k);
  painter.setFont(prompt);
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 680 * k, width(), 80 * k), Qt::AlignCenter, m_remap_prompt);
  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, 770 * k, width(), 36 * k), Qt::AlignCenter,
                   tr("Appuie sur la touche, le stick ou la gâchette à utiliser."));
  painter.drawText(QRectF(0, height() - 70 * k, width() - 80 * k, 34 * k),
                   Qt::AlignVCenter | Qt::AlignRight, tr("Échap (clavier) : annuler"));
}

// ================================================================== stick calibration

void BigPictureWindow::StartCalibration(int port, int group)
{
  m_cal_port = port;
  m_cal_group = group;
  m_calibrator = std::make_unique<ciface::MappingCommon::CalibrationBuilder>();
  m_mode_before_remap = m_mode;
  BeginTransition(0);
  m_mode = Mode::Calibrate;
  m_prev_held = ~0u;
}

void BigPictureWindow::UpdateCalibration()
{
  if (!m_calibrator)
    return;
  auto* stick = static_cast<ControllerEmu::ReshapableInput*>(
      BigPictureGroup(m_cal_port, m_cal_group));
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  const auto point = stick->GetReshapableState(false);
  m_cal_x = point.x;
  m_cal_y = point.y;
  m_calibrator->Update(point);
  if (m_calibrator->IsComplete())
  {
    m_calibrator->ApplyResults(stick);
    SaveBigPicturePads();
    m_calibrator.reset();
    m_audio->Play(Sound::Confirm);
    Toast(tr("Stick étalonné"));
    BeginTransition(0);
    m_mode = m_mode_before_remap;
  }
}

void BigPictureWindow::FinishCalibration(bool apply)
{
  if (!m_calibrator)
    return;
  if (apply && m_calibrator->IsCalibrationDataSensible())
  {
    auto* stick = static_cast<ControllerEmu::ReshapableInput*>(
        BigPictureGroup(m_cal_port, m_cal_group));
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    m_calibrator->ApplyResults(stick);
    SaveBigPicturePads();
    Toast(tr("Stick étalonné"));
    m_audio->Play(Sound::Confirm);
  }
  else
  {
    m_audio->Play(apply ? Sound::Error : Sound::Cancel);
    if (apply)
    {
      Toast(tr("Fais encore quelques tours complets"));
      return;
    }
  }
  m_calibrator.reset();
  BeginTransition(0);
  m_mode = m_mode_before_remap;
  m_prev_held = ~0u;
}

void BigPictureWindow::DrawCalibration(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const bool is_main = m_cal_group == static_cast<int>(PadGroup::MainStick);
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 110 * k, width(), 30 * k), Qt::AlignCenter,
                   m_cal_group >= 100 ? tr("ÉTALONNAGE DU STICK") :
                   is_main            ? tr("ÉTALONNAGE DU STICK PRINCIPAL") :
                                        tr("ÉTALONNAGE DU STICK C"));

  const QPointF c(width() / 2.0, 470 * k);
  const qreal radius = 230 * k;
  painter.setPen(QPen(LINE, 2 * k));
  painter.setBrush(SURFACE);
  painter.drawEllipse(c, radius, radius);
  // Octagonal GameCube gate, for reference
  QPolygonF gate;
  for (int i = 0; i < 8; ++i)
  {
    // GameCube gate: corners on the axes and the diagonals.
    const qreal a = i * std::numbers::pi / 4;
    gate << QPointF(c.x() + std::cos(a) * radius, c.y() - std::sin(a) * radius);
  }
  painter.setPen(QPen(TEXT_DIM, 2 * k, Qt::DashLine));
  painter.setBrush(Qt::NoBrush);
  painter.drawPolygon(gate);

  if (m_calibrator)
  {
    QPolygonF shape;
    for (int i = 0; i <= 64; ++i)
    {
      const qreal a = i * 2 * std::numbers::pi / 64;
      const qreal r = m_calibrator->GetCalibrationRadiusAtAngle(a) * radius;
      shape << QPointF(c.x() + std::cos(a) * r, c.y() - std::sin(a) * r);
    }
    QColor fill = ACCENT;
    fill.setAlphaF(0.30f);
    painter.setPen(QPen(ACCENT_SOFT, 3 * k));
    painter.setBrush(fill);
    painter.drawPolygon(shape);
  }
  // Current position
  const QPointF p(c.x() + std::clamp<qreal>(m_cal_x, -1.2, 1.2) * radius,
                  c.y() - std::clamp<qreal>(m_cal_y, -1.2, 1.2) * radius);
  painter.setPen(Qt::NoPen);
  painter.setBrush(TEXT);
  painter.drawEllipse(p, 12 * k, 12 * k);

  const bool sensible = m_calibrator && m_calibrator->IsCalibrationDataSensible();
  QFont prompt = Font(true, int(48 * k), 700);
  painter.setFont(prompt);
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 740 * k, width(), 70 * k), Qt::AlignCenter,
                   sensible ? tr("Relâche le stick pour terminer") :
                              tr("Fais tourner le stick le long du bord"));
  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, 820 * k, width(), 36 * k), Qt::AlignCenter,
                   tr("Plusieurs tours complets, lentement, en poussant à fond."));
  DrawHints(painter, {}, {{QStringLiteral("A"), tr("Terminer")}, {QStringLiteral("B"), tr("Annuler")}});
}

// ================================================================== memory card manager

void BigPictureWindow::OpenMemcards()
{
  if (!Core::IsUninitialized(Core::System::GetInstance()))
  {
    m_audio->Play(Sound::Error);
    Toast(tr("Quitte le jeu pour gérer les cartes mémoire"));
    return;
  }
  LoadCard(0);
  LoadCard(1);
  m_mc_card = 0;
  m_mc_index = 0;
  m_mc_actions_open = false;
  m_mc_confirm_erase = false;
  if (m_mode != Mode::Memcard)
    m_mode_before_memcard = m_mode == Mode::Menu ? Mode::Library : m_mode;
  m_audio->Play(Sound::Confirm);
  BeginTransition(1);
  m_mode = Mode::Memcard;
}

void BigPictureWindow::LoadCard(int slot)
{
  CardView& view = m_cards[slot];
  view = CardView{};
  const auto exi_slot = slot == 0 ? ExpansionInterface::Slot::A : ExpansionInterface::Slot::B;
  const auto type = Config::Get(slot == 0 ? Config::MAIN_SLOT_A : Config::MAIN_SLOT_B);
  if (type == ExpansionInterface::EXIDeviceType::MemoryCardFolder)
  {
    // GCI folder: one file per save. Build a card in memory to show them like the others.
    const std::string folder =
        Config::GetGCIFolderPath(exi_slot, Config::Get(Config::MAIN_FALLBACK_REGION));
    view.folder = true;
    view.path = QString::fromStdString(folder);
    File::CreateFullPath(folder + DIR_SEP);
    auto card = Memcard::GCMemcard::Create(std::string(), CardFlashId{},
                                           Memcard::MBIT_SIZE_MEMORY_CARD_2043, false, 0, 0, 0);
    if (!card)
    {
      view.error = tr("Dossier GCI illisible");
      return;
    }
    view.card = std::make_unique<Memcard::GCMemcard>(std::move(*card));
    for (const QFileInfo& fi : QDir(view.path).entryInfoList({QStringLiteral("*.gci")}, QDir::Files,
                                                             QDir::Name))
    {
      const std::string file = fi.absoluteFilePath().toStdString();
      auto read = Memcard::ReadSavefile(file);
      auto* save = std::get_if<Memcard::Savefile>(&read);
      if (!save || view.card->ImportFile(*save) != Memcard::GCMemcardImportFileRetVal::SUCCESS)
        continue;
      for (u8 f = 0; f < view.card->GetNumFiles(); ++f)
      {
        const u8 index = view.card->GetFileIndex(f);
        const auto entry = view.card->GetDEntry(index);
        if (entry && Memcard::HasSameIdentity(*entry, save->dir_entry))
          view.files[index] = file;
      }
    }
  }
  else if (type != ExpansionInterface::EXIDeviceType::MemoryCard)
  {
    view.error = tr("Pas de carte mémoire dans cet emplacement");
    return;
  }
  else
  {
    const std::string path =
        Config::GetMemcardPath(exi_slot, Config::Get(Config::MAIN_FALLBACK_REGION));
    view.path = QString::fromStdString(path);
    auto opened = Memcard::GCMemcard::Open(path);
    auto& card = opened.second;
    if (!card || !card->IsValid())
    {
      view.error = QFileInfo::exists(view.path) ? tr("Carte mémoire illisible") :
                                                  tr("Carte mémoire vide (pas encore créée)");
      return;
    }
    view.card = std::make_unique<Memcard::GCMemcard>(std::move(*card));
  }
  view.free_blocks = view.card->GetFreeBlocks();
  const u8 n = view.card->GetNumFiles();
  for (u8 f = 0; f < n; ++f)
  {
    const u8 index = view.card->GetFileIndex(f);
    SaveIcon icon;
    icon.index = index;
    icon.blocks = view.card->DEntry_BlockCount(index);
    if (const auto comments = view.card->GetSaveComments(index))
    {
      icon.title = QString::fromStdString(comments->first).trimmed();
      icon.comment = QString::fromStdString(comments->second).trimmed();
    }
    if (auto banner = view.card->ReadBannerRGBA8(index))
    {
      const QImage img(reinterpret_cast<const uchar*>(banner->data()),
                       Memcard::MEMORY_CARD_BANNER_WIDTH, Memcard::MEMORY_CARD_BANNER_HEIGHT,
                       QImage::Format_ARGB32);
      icon.banner = QPixmap::fromImage(img.copy());
    }
    if (auto frames = view.card->ReadAnimRGBA8(index))
    {
      for (const auto& frame : *frames)
      {
        const QImage img(reinterpret_cast<const uchar*>(frame.image_data.data()),
                         Memcard::MEMORY_CARD_ICON_WIDTH, Memcard::MEMORY_CARD_ICON_HEIGHT,
                         QImage::Format_ARGB32);
        icon.frames.push_back(QPixmap::fromImage(img.copy()));
        icon.delays.push_back(std::max<int>(1, frame.delay) * 4);
      }
    }
    view.saves.push_back(std::move(icon));
  }
}

void BigPictureWindow::HandleMemcard(Action action)
{
  constexpr int COLS = 5;
  CardView& card = m_cards[m_mc_card];
  const int count = static_cast<int>(card.saves.size());

  if (m_mc_actions_open)
  {
    const int items = m_mc_confirm_erase ? 2 : 4;
    switch (action)
    {
    case Action::Down:
      m_mc_action = (m_mc_action + 1) % items;
      m_audio->Play(Sound::Move);
      break;
    case Action::Up:
      m_mc_action = (m_mc_action + items - 1) % items;
      m_audio->Play(Sound::Move);
      break;
    case Action::Back:
      m_mc_actions_open = false;
      m_mc_confirm_erase = false;
      m_audio->Play(Sound::Cancel);
      break;
    case Action::Accept:
    {
      if (count == 0)
        break;
      const SaveIcon& save = card.saves[std::clamp(m_mc_index, 0, count - 1)];
      CardView& other = m_cards[1 - m_mc_card];
      if (m_mc_confirm_erase)
      {
        if (m_mc_action == 0)
        {
          RemoveSave(card, save.index);
          m_audio->Play(Sound::Erase);
          Toast(tr("Sauvegarde effacée"));
          LoadCard(m_mc_card);
          m_mc_index = std::max(0, m_mc_index - 1);
        }
        else
        {
          m_audio->Play(Sound::Cancel);
        }
        m_mc_actions_open = false;
        m_mc_confirm_erase = false;
        break;
      }
      if (m_mc_action == 0 || m_mc_action == 1)  // copy / move
      {
        if (!other.card)
        {
          m_audio->Play(Sound::Error);
          Toast(tr("L'autre emplacement n'a pas de carte mémoire utilisable"));
          m_mc_actions_open = false;
          break;
        }
        const auto result =
            static_cast<Memcard::GCMemcardImportFileRetVal>(ImportSave(other, save.index, card));
        if (result != Memcard::GCMemcardImportFileRetVal::SUCCESS)
        {
          m_audio->Play(Sound::Error);
          Toast(result == Memcard::GCMemcardImportFileRetVal::TITLEPRESENT ?
                    tr("Cette sauvegarde existe déjà sur l'autre carte") :
                    tr("Pas assez de place sur l'autre carte"));
          m_mc_actions_open = false;
          break;
        }
        if (m_mc_action == 1)
        {
          RemoveSave(card, save.index);
          m_audio->Play(Sound::LoadState);  // "Memory Card Move"
          Toast(tr("Sauvegarde déplacée"));
        }
        else
        {
          m_audio->Play(Sound::SaveState);  // "Memory Card Copy"
          Toast(tr("Sauvegarde copiée"));
        }
        LoadCard(0);
        LoadCard(1);
        m_mc_index = std::clamp(m_mc_index, 0,
                                std::max(0, static_cast<int>(m_cards[m_mc_card].saves.size()) - 1));
        m_mc_actions_open = false;
      }
      else if (m_mc_action == 2)  // erase -> confirm
      {
        m_mc_confirm_erase = true;
        m_mc_action = 1;  // "Annuler" by default
        m_audio->Play(Sound::Toggle);
      }
      else
      {
        m_mc_actions_open = false;
        m_audio->Play(Sound::Cancel);
      }
      break;
    }
    default:
      break;
    }
    return;
  }

  switch (action)
  {
  case Action::Right:
    if (count > 0 && m_mc_index % COLS < COLS - 1 && m_mc_index + 1 < count)
    {
      m_mc_index++;
      m_audio->Play(Sound::Move);
    }
    else if (m_mc_card == 0)
    {
      m_mc_card = 1;
      m_mc_index = 0;
      m_audio->Play(Sound::Page);
    }
    break;
  case Action::Left:
    if (count > 0 && m_mc_index % COLS > 0)
    {
      m_mc_index--;
      m_audio->Play(Sound::Move);
    }
    else if (m_mc_card == 1)
    {
      m_mc_card = 0;
      m_mc_index = std::min<int>(COLS - 1, std::max(0, static_cast<int>(m_cards[0].saves.size()) - 1));
      m_audio->Play(Sound::Page);
    }
    break;
  case Action::Down:
    if (m_mc_index + COLS < count)
    {
      m_mc_index += COLS;
      m_audio->Play(Sound::Move);
    }
    break;
  case Action::Up:
    if (m_mc_index - COLS >= 0)
    {
      m_mc_index -= COLS;
      m_audio->Play(Sound::Move);
    }
    break;
  case Action::PageUp:
  case Action::PageDown:
    m_mc_card = action == Action::PageDown ? 1 : 0;
    m_mc_index = 0;
    m_audio->Play(Sound::Page);
    break;
  case Action::Accept:
    if (count > 0 && card.card)
    {
      m_mc_actions_open = true;
      m_mc_confirm_erase = false;
      m_mc_action = 0;
      m_audio->Play(Sound::Confirm);
    }
    else
    {
      m_audio->Play(Sound::Error);
    }
    break;
  case Action::Back:
    m_audio->Play(Sound::Back);
    BeginTransition(-1);
    m_mode = m_mode_before_memcard;
    break;
  default:
    break;
  }
}

void BigPictureWindow::DrawMemcards(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  constexpr int COLS = 5;
  const qreal top = 120 * k;

  painter.setFont(Font(true, int(48 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(80 * k, 40 * k, width(), 70 * k), Qt::AlignVCenter,
                   tr("Cartes mémoire"));

  const qreal gap = 60 * k;
  const qreal pw = (width() - 160 * k - gap) / 2;
  for (int slot = 0; slot < 2; ++slot)
  {
    const CardView& view = m_cards[slot];
    const bool active = slot == m_mc_card;
    const QRectF panel(80 * k + slot * (pw + gap), top + 20 * k, pw, 640 * k);
    painter.setPen(active ? QPen(TEXT, 3 * k) : Qt::NoPen);
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(panel, 26 * k, 26 * k);

    painter.setFont(Font(true, int(28 * k), 700));
    painter.setPen(TEXT);
    painter.drawText(QRectF(panel.left() + 30 * k, panel.top() + 22 * k, pw, 40 * k),
                     Qt::AlignVCenter, slot == 0 ? tr("Emplacement A") : tr("Emplacement B"));
    painter.setFont(Font(false, int(19 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(panel.left(), panel.top() + 22 * k, pw - 30 * k, 40 * k),
                     Qt::AlignVCenter | Qt::AlignRight,
                     view.card ? tr("%1 blocs libres").arg(view.free_blocks) : QString());

    if (!view.card)
    {
      painter.setFont(Font(false, int(22 * k), 500));
      painter.setPen(TEXT_SOFT);
      painter.drawText(panel, Qt::AlignCenter, view.error);
      continue;
    }
    if (view.saves.empty())
    {
      painter.setFont(Font(false, int(22 * k), 500));
      painter.setPen(TEXT_SOFT);
      painter.drawText(panel, Qt::AlignCenter, tr("Aucune sauvegarde"));
      continue;
    }

    // Grid of animated save icons, like the GameCube's memory card screen
    const qreal cell = (pw - 60 * k) / COLS;
    const qreal icon = std::min<qreal>(cell - 24 * k, 96 * k);
    const qreal grid_top = panel.top() + 90 * k;
    const int rows_visible = int((panel.height() - 110 * k) / cell);
    const int sel_row = active ? m_mc_index / COLS : 0;
    const int first_row = std::max(0, sel_row - rows_visible + 1);
    for (int i = 0; i < static_cast<int>(view.saves.size()); ++i)
    {
      const int row = i / COLS - first_row;
      if (row < 0 || row >= rows_visible)
        continue;
      const SaveIcon& save = view.saves[i];
      const QRectF cellr(panel.left() + 30 * k + (i % COLS) * cell, grid_top + row * cell, cell,
                         cell);
      const bool focused = active && i == m_mc_index;
      if (focused)
      {
        painter.setPen(Qt::NoPen);
        painter.setBrush(RAISED);
        painter.drawRoundedRect(cellr.adjusted(6 * k, 6 * k, -6 * k, -6 * k), 16 * k, 16 * k);
        const qreal pulse = 0.75 + 0.25 * std::sin(m_time * 3.2);
        QColor ring = TEXT;
        ring.setAlphaF(static_cast<float>(pulse));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ring, 3 * k));
        painter.drawRoundedRect(cellr.adjusted(6 * k, 6 * k, -6 * k, -6 * k), 16 * k, 16 * k);
      }
      if (!save.frames.empty())
      {
        int total = 0;
        for (int d : save.delays)
          total += d;
        int tick = static_cast<int>(m_time * 60) % std::max(1, total);
        size_t f = 0;
        while (f + 1 < save.frames.size() && tick >= save.delays[f])
          tick -= save.delays[f++];
        const qreal sz = focused ? icon * 1.12 : icon;
        painter.drawPixmap(QRectF(cellr.center().x() - sz / 2, cellr.center().y() - sz / 2, sz, sz),
                           save.frames[f], QRectF(save.frames[f].rect()));
      }
    }

    // Details of the focused save
    if (active)
    {
      const SaveIcon& save =
          view.saves[std::clamp(m_mc_index, 0, static_cast<int>(view.saves.size()) - 1)];
      const QRectF info(panel.left(), panel.bottom() + 24 * k, pw, 180 * k);
      painter.setPen(Qt::NoPen);
      painter.setBrush(SURFACE);
      painter.drawRoundedRect(info, 22 * k, 22 * k);
      const QRectF br(info.left() + 24 * k, info.top() + 24 * k, 288 * k, 96 * k);
      if (!save.banner.isNull())
        painter.drawPixmap(br, save.banner, QRectF(save.banner.rect()));
      const qreal tx = br.right() + 24 * k;
      painter.setFont(Font(true, int(24 * k), 700));
      painter.setPen(TEXT);
      painter.drawText(QRectF(tx, info.top() + 24 * k, info.right() - tx - 20 * k, 36 * k),
                       Qt::AlignVCenter, save.title);
      painter.setFont(Font(false, int(19 * k), 400));
      painter.setPen(TEXT_SOFT);
      painter.drawText(QRectF(tx, info.top() + 64 * k, info.right() - tx - 20 * k, 32 * k),
                       Qt::AlignVCenter, save.comment);
      painter.drawText(QRectF(tx, info.top() + 100 * k, info.right() - tx - 20 * k, 32 * k),
                       Qt::AlignVCenter, tr("%n bloc(s)", "", save.blocks));
    }
  }

  // Action list
  if (m_mc_actions_open)
  {
    painter.fillRect(rect(), QColor(9, 7, 20, 170));
    const QString other = m_mc_card == 0 ? tr("B") : tr("A");
    std::vector<QString> items;
    if (m_mc_confirm_erase)
      items = {tr("Effacer définitivement"), tr("Annuler")};
    else
      items = {tr("Copier vers l'emplacement %1").arg(other),
               tr("Déplacer vers l'emplacement %1").arg(other), tr("Effacer"), tr("Annuler")};
    const qreal bw = 640 * k;
    const qreal bh = 90 * k + items.size() * 84 * k;
    const QRectF box((width() - bw) / 2, (height() - bh) / 2, bw, bh);
    painter.setPen(Qt::NoPen);
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(box, 26 * k, 26 * k);
    painter.setFont(Font(true, int(28 * k), 700));
    painter.setPen(TEXT);
    painter.drawText(QRectF(box.left(), box.top() + 20 * k, bw, 50 * k), Qt::AlignCenter,
                     m_mc_confirm_erase ? tr("Effacer cette sauvegarde ?") : tr("Sauvegarde"));
    qreal y = box.top() + 84 * k;
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
    {
      const QRectF r(box.left() + 28 * k, y, bw - 56 * k, 72 * k);
      const bool sel = i == m_mc_action;
      if (sel)
      {
        painter.setBrush(TEXT);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(r, 14 * k, 14 * k);
      }
      painter.setFont(Font(false, int(23 * k), sel ? 600 : 500));
      const bool danger = (m_mc_confirm_erase && i == 0) || (!m_mc_confirm_erase && i == 2);
      painter.setPen(sel ? BG : (danger ? DANGER : TEXT));
      painter.drawText(r.adjusted(24 * k, 0, 0, 0), Qt::AlignVCenter, items[i]);
      y += 84 * k;
    }
  }

  DrawHints(painter,
            {{QStringLiteral("L"), QString()}, {QStringLiteral("R"), tr("Emplacement")}},
            {{QStringLiteral("A"), tr("Choisir")}, {QStringLiteral("B"), tr("Retour")}});
}

// ================================================================== controllers

namespace
{
bool IsIgnoredDevice(const std::string& d)
{
  return d.find("Keyboard") != std::string::npos || d.find("Mouse") != std::string::npos ||
         d.rfind("DSUClient", 0) == 0 || d.rfind("Android", 0) == 0;
}

int SourcePriority(const std::string& d)
{
  if (d.rfind("SDL/", 0) == 0)
    return 0;
  if (d.rfind("XInput/", 0) == 0)
    return 1;
  if (d.rfind("WGInput/", 0) == 0)
    return 2;
  return 3;
}

bool IsPressInput(const std::string& name)
{
  for (const char* prefix : {"Button", "Pad ", "Shoulder", "Start", "Back", "Guide", "Thumb",
                             "Trigger", "Home", "Select", "Plus", "Minus", "Misc", "Paddle"})
  {
    if (name.rfind(prefix, 0) == 0)
      return true;
  }
  return false;
}

bool DeviceHasPress(const std::string& device_string)
{
  ciface::Core::DeviceQualifier dq;
  dq.FromString(device_string);
  const auto device = g_controller_interface.FindDevice(dq);
  if (!device)
    return false;
  for (const auto* input : device->Inputs())
  {
    if (IsPressInput(input->GetName()) && input->GetState() > 0.6)
      return true;
  }
  return false;
}

QString DeviceDisplayName(const std::string& device_string)
{
  ciface::Core::DeviceQualifier dq;
  dq.FromString(device_string);
  QString name = QString::fromStdString(dq.name.empty() ? device_string : dq.name);
  if (dq.source == "Switch2")
    name += QStringLiteral(" (Bluetooth)");
  return name;
}

bool LooksLikeGameCube(const std::string& d)
{
  std::string lower = d;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lower.find("gamecube") != std::string::npos;
}

bool PortActive(int port)
{
  return Config::Get(Config::GetInfoForSIDevice(port)) != SerialInterface::SIDEVICE_NONE;
}
}  // namespace

std::string BigPictureWindow::PressedDevice(const std::vector<std::string>& exclude) const
{
  std::string best;
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
  {
    if (IsIgnoredDevice(d) || std::find(exclude.begin(), exclude.end(), d) != exclude.end())
      continue;
    if (!DeviceHasPress(d))
      continue;
    if (best.empty() || SourcePriority(d) < SourcePriority(best))
      best = d;
  }
  return best;
}

void BigPictureWindow::PollDevices()
{
  if (!m_active)
    return;
#ifdef _WIN32
  // Bluetooth of the Switch 2 controllers: show connections and failures.
  {
    const QString bt = QString::fromStdString(ciface::Switch2BLE::GetStatus());
    if (bt != m_bt_status)
    {
      const bool first = m_bt_status.isEmpty();
      m_bt_status = bt;
      if (!first && (bt.startsWith(QStringLiteral("Échec")) ||
                     bt.startsWith(QStringLiteral("Connectée")) ||
                     bt.startsWith(QStringLiteral("Manette trouvée"))))
        Notify(QStringLiteral("Bluetooth : ") + bt);
    }
  }
#endif
  switch (m_mode)
  {
  case Mode::Setup:
  case Mode::Remap:
  case Mode::Calibrate:
  case Mode::PadTest:
  case Mode::WiiTest:
  case Mode::Launching:
  case Mode::Closing:
    return;
  default:
    break;
  }
  const qint64 now = m_clock.elapsed();

  // Devices already in use on some port.
  std::vector<std::string> used;
  bool used_pressed = false;
  for (int port = 0; port < 4; ++port)
  {
    if (port > 0 && !PortActive(port))
      continue;
    if (auto* pad = PadController(port))
    {
      const std::string d = pad->GetDefaultDevice().ToString();
      used.push_back(d);
      used_pressed |= DeviceHasPress(d);
    }
  }
  // A controller in use is being pressed: its twin seen through another backend
  // (XInput / WGInput / SDL) must not be taken for a new controller.
  if (used_pressed)
    return;

  if (m_mode == Mode::Players)
  {
    const std::string d = PressedDevice(used);
    if (d.empty())
      return;
    for (int port = 1; port < 4; ++port)
    {
      if (PortActive(port))
        continue;
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(port),
                               SerialInterface::SIDEVICE_GC_CONTROLLER);
      SwitchPort(port, d, false);
      m_audio->Play(Sound::Confirm);
      return;
    }
    m_audio->Play(Sound::Error);
    return;
  }

  if (!Prefs::AutoSwitchController() || now - m_last_switch < 1200)
    return;
  const std::string d = PressedDevice(used);
  if (!d.empty())
    SwitchPort(0, d, true);
}

void BigPictureWindow::SwitchPort(int port, const std::string& device, bool announce)
{
  auto* pad = PadController(port);
  if (!pad)
    return;
  m_last_switch = m_clock.elapsed();
  // Keep the settings of the controller being replaced.
  if (port == 0 || PortActive(port))
    SaveBigPictureDeviceProfile(port);

  bool needs_setup = false;
  if (!LoadBigPictureDeviceProfile(port, device) && !ApplyGameCubeProfile(port, device))
  {
    if (LooksLikeGameCube(device) || !ApplyStandardGamepadProfile(port, device))
    {
      const auto lock = ControllerEmu::EmulatedController::GetStateLock();
      ciface::Core::DeviceQualifier dq;
      dq.FromString(device);
      pad->SetDefaultDevice(dq);
      pad->UpdateReferences(g_controller_interface);
      needs_setup = true;
    }
  }
  SaveBigPicturePads();
  m_prev_held = ~0u;  // the press that caused the switch must not trigger anything

  const QString name = DeviceDisplayName(device);
  if (announce)
    Notify(tr("Joueur %1 : %2").arg(port + 1).arg(name));
  if (needs_setup && port == 0 && Core::IsUninitialized(Core::System::GetInstance()) &&
      (m_mode == Mode::Library || m_mode == Mode::Menu) && isVisible())
  {
    // A new GameCube controller: its buttons are learned once, then remembered.
    StartSetup(0);
  }
  else if (needs_setup && announce)
  {
    Notify(tr("%1 : configure-la dans Réglages › Manettes").arg(name));
  }
}

void BigPictureWindow::OnDevicesChanged()
{
  QStringList now;
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
    now << QString::fromStdString(d);
  QString added, removed;
  for (const QString& d : now)
  {
    if (!m_known_devices.contains(d) && !IsIgnoredDevice(d.toStdString()) && added.isEmpty())
      added = d;
  }
  for (const QString& d : m_known_devices)
  {
    if (!now.contains(d) && !IsIgnoredDevice(d.toStdString()) && removed.isEmpty())
      removed = d;
  }
  m_known_devices = now;
  if (!m_active)
    return;
  QTimer::singleShot(800, this, [this] { CheckDrivers(); });

  if (!added.isEmpty())
    Notify(tr("Manette connectée : %1").arg(DeviceDisplayName(added.toStdString())));
  else if (!removed.isEmpty())
    Notify(tr("Manette déconnectée : %1").arg(DeviceDisplayName(removed.toStdString())));

  // Player 1 lost its controller: take another one that is plugged in.
  auto* pad = PadController(0);
  if (!pad || !Prefs::AutoSwitchController() || g_controller_interface.FindDevice(pad->GetDefaultDevice()))
    return;
  std::string best;
  for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
  {
    if (!IsIgnoredDevice(d) && (best.empty() || SourcePriority(d) < SourcePriority(best)))
      best = d;
  }
  if (!best.empty())
    SwitchPort(0, best, true);
}

void BigPictureWindow::Notify(const QString& text)
{
  const bool over_game = m_mode == Mode::InGame || !isVisible();
  if (!over_game)
  {
    Toast(text);
    return;
  }
  // Over the game: a small click-through window, top right.
  QScreen* screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
  if (!screen)
    return;
  const QRect g = screen->geometry();
  const qreal k = g.height() / 1080.0;
  m_notify_text = text;
  m_notify_t = 0;
  QFont font(m_text_family);
  font.setPixelSize(int(64 * k * 0.34));
  font.setWeight(QFont::DemiBold);
  const int w = int(QFontMetricsF(font).horizontalAdvance(text) + 140 * k);
  const int h = int(80 * k);
  m_notifier->setGeometry(g.right() - w - int(40 * k), g.top() + int(40 * k), w, h);
  m_notifier->show();
  m_notifier->raise();
  if (!m_notify_timer)
  {
    m_notify_timer = new QTimer(this);
    m_notify_timer->setInterval(16);
    connect(m_notify_timer, &QTimer::timeout, this, [this] {
      m_notify_t += 0.016;
      if (m_notify_t >= 3.0)
      {
        m_notify_timer->stop();
        m_notifier->hide();
        return;
      }
      m_notifier->update();
    });
  }
  m_notify_timer->start();
}

void BigPictureWindow::DrawPlayers(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 120 * k, width(), 30 * k), Qt::AlignCenter, tr("MULTIJOUEUR"));
  painter.setFont(Font(true, int(52 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 160 * k, width(), 80 * k), Qt::AlignCenter, tr("Qui joue ?"));
  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, 250 * k, width(), 36 * k), Qt::AlignCenter,
                   tr("Appuie sur un bouton d'une autre manette pour la faire rejoindre."));

  const qreal cw = 380 * k, ch = 420 * k, gap = 36 * k;
  const qreal x0 = (width() - (4 * cw + 3 * gap)) / 2;
  const qreal y = 340 * k;
  for (int port = 0; port < 4; ++port)
  {
    const QRectF r(x0 + port * (cw + gap), y, cw, ch);
    const bool active = port == 0 || PortActive(port);
    const GCPadStatus st = active ? Pad::GetStatus(port) : GCPadStatus{};
    const bool pressed = active && (st.button & 0x1fff) != 0;
    painter.setPen(QPen(active ? (pressed ? ACCENT_SOFT : ACCENT) : LINE, (active ? 3 : 2) * k,
                        active ? Qt::SolidLine : Qt::DashLine));
    painter.setBrush(active ? RAISED : SURFACE_DEEP);
    painter.drawRoundedRect(r, 28 * k, 28 * k);

    painter.setFont(Font(true, int(96 * k), 700));
    painter.setPen(active ? TEXT : TEXT_DIM);
    painter.drawText(QRectF(r.left(), r.top() + 40 * k, cw, 120 * k), Qt::AlignCenter,
                     QString::number(port + 1));
    painter.setFont(Font(false, int(18 * k), 600));
    painter.setPen(active ? ACCENT_SOFT : TEXT_DIM);
    painter.drawText(QRectF(r.left(), r.top() + 170 * k, cw, 30 * k), Qt::AlignCenter,
                     tr("JOUEUR %1").arg(port + 1));

    QString device;
    if (active)
    {
      if (auto* pad = PadController(port))
        device = DeviceDisplayName(pad->GetDefaultDevice().ToString());
    }
    DrawIcon(painter, active ? QStringLiteral("Control_Stick") : QStringLiteral("A"),
             QRectF(r.center().x() - 36 * k, r.top() + 225 * k, 72 * k, 72 * k));
    painter.setFont(Font(false, int(21 * k), 500));
    painter.setPen(active ? TEXT : TEXT_DIM);
    painter.drawText(QRectF(r.left() + 24 * k, r.top() + 310 * k, cw - 48 * k, 90 * k),
                     Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                     active ? device : tr("Appuie sur un bouton pour rejoindre"));
  }
  DrawHints(painter, {}, {{QStringLiteral("X"), tr("Joueur seul")}, {QStringLiteral("B"), tr("Retour")}});
}

void BigPictureWindow::DrawPadTest(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const GCPadStatus st = Pad::GetStatus(m_test_port);
  const auto on = [&](u16 mask) { return (st.button & mask) != 0; };

  // Header
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 90 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("TEST DE LA MANETTE — JOUEUR %1").arg(m_test_port + 1));
  QString device;
  if (auto* pad = PadController(m_test_port))
    device = DeviceDisplayName(pad->GetDefaultDevice().ToString());
  painter.setFont(Font(true, int(40 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 125 * k, width(), 60 * k), Qt::AlignCenter, device);

  // One panel shaped like the controller, everything at its place on it.
  const QRectF panel(width() / 2.0 - 760 * k, 220 * k, 1520 * k, 660 * k);
  painter.setPen(QPen(LINE, 2 * k));
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(panel, 56 * k, 56 * k);

  // Shoulders: analog value + click
  const auto trigger = [&](bool left, u8 v, bool click) {
    const qreal w = 430 * k, h = 34 * k;
    const qreal x = left ? panel.left() + 150 * k : panel.right() - 150 * k - w;
    const QRectF bar(x, panel.top() + 46 * k, w, h);
    painter.setPen(Qt::NoPen);
    painter.setBrush(SURFACE_DEEP);
    painter.drawRoundedRect(bar, h / 2, h / 2);
    const qreal fill = bar.width() * v / 255.0;
    if (fill > 1)
    {
      painter.setBrush(click ? ACCENT_SOFT : ACCENT);
      painter.drawRoundedRect(left ? QRectF(bar.left(), bar.top(), fill, h) :
                                     QRectF(bar.right() - fill, bar.top(), fill, h),
                              h / 2, h / 2);
    }
    const QRectF icon(left ? bar.left() - 82 * k : bar.right() + 22 * k, bar.center().y() - 30 * k,
                      60 * k, 60 * k);
    DrawIcon(painter, left ? QStringLiteral("L") : QStringLiteral("R"), icon);
    painter.setFont(Font(false, int(17 * k), 600));
    painter.setPen(TEXT_DIM);
    painter.drawText(QRectF(bar.left(), bar.bottom() + 6 * k, bar.width(), 24 * k),
                     (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                     click ? tr("Clic") : QStringLiteral("%1 %").arg(int(v * 100 / 255)));
  };
  trigger(true, st.triggerLeft, on(PAD_TRIGGER_L));
  trigger(false, st.triggerRight, on(PAD_TRIGGER_R));

  // Sticks, scaled to the GameCube gate (full tilt touches the octagon)
  const auto stick = [&](QPointF c, qreal r, u8 sx, u8 sy, qreal gate, const QString& icon,
                         QColor col) {
    QPolygonF oct;
    for (int i = 0; i < 8; ++i)
    {
      const qreal a = i * std::numbers::pi / 4;
      oct << QPointF(c.x() + std::cos(a) * r, c.y() - std::sin(a) * r);
    }
    painter.setPen(QPen(LINE, 2 * k));
    painter.setBrush(SURFACE_DEEP);
    painter.drawPolygon(oct);
    painter.setPen(QPen(LINE, 1 * k, Qt::DashLine));
    painter.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
    painter.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
    const qreal x = (sx - 128) / (127.0 * gate), y = (sy - 128) / (127.0 * gate);
    const QPointF p(c.x() + std::clamp<qreal>(x, -1.15, 1.15) * r,
                    c.y() - std::clamp<qreal>(y, -1.15, 1.15) * r);
    painter.setPen(QPen(col, 3 * k));
    painter.drawLine(c, p);
    painter.setPen(Qt::NoPen);
    painter.setBrush(col);
    painter.drawEllipse(p, 15 * k, 15 * k);
    DrawIcon(painter, icon, QRectF(c.x() - r - 70 * k, c.y() + r - 52 * k, 52 * k, 52 * k));
    painter.setFont(Font(false, int(16 * k), 500));
    painter.setPen(TEXT_DIM);
    painter.drawText(QRectF(c.x() - r, c.y() + r + 8 * k, 2 * r, 24 * k), Qt::AlignCenter,
                     QStringLiteral("X %1   Y %2").arg(int(sx) - 128).arg(int(sy) - 128));
  };
  stick(QPointF(panel.left() + 330 * k, panel.top() + 285 * k), 135 * k, st.stickX, st.stickY,
        0.7937125, QStringLiteral("Control_Stick"), TEXT);
  stick(QPointF(panel.right() - 560 * k, panel.top() + 505 * k), 100 * k, st.substickX,
        st.substickY, 0.7221375, QStringLiteral("C-Stick"), QColor(255, 210, 60));

  // D-pad: the icon variant shows the pressed direction
  QString dir;
  if (on(PAD_BUTTON_UP))
    dir += QLatin1Char('U');
  if (on(PAD_BUTTON_DOWN))
    dir += QLatin1Char('D');
  if (on(PAD_BUTTON_LEFT))
    dir += QLatin1Char('L');
  if (on(PAD_BUTTON_RIGHT))
    dir += QLatin1Char('R');
  QString dpad = QStringLiteral("D-Pad");
  if (!dir.isEmpty() && dir.size() <= 2)
    dpad += QLatin1Char('-') + dir;
  const QPointF dc(panel.left() + 560 * k, panel.top() + 505 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(dir.isEmpty() ? SURFACE_DEEP : ACCENT);
  painter.drawEllipse(dc, 78 * k, 78 * k);
  DrawIcon(painter, dpad, QRectF(dc.x() - 58 * k, dc.y() - 58 * k, 116 * k, 116 * k));

  // Buttons
  const auto button = [&](QPointF c, qreal r, const QString& icon, bool pressed) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(pressed ? ACCENT : SURFACE_DEEP);
    painter.drawEllipse(c, r, r);
    painter.setOpacity(pressed ? 1.0 : 0.6);
    DrawIcon(painter, icon, QRectF(c.x() - r * 0.78, c.y() - r * 0.78, r * 1.56, r * 1.56));
    painter.setOpacity(1.0);
  };
  button(QPointF(panel.center().x(), panel.top() + 285 * k), 36 * k,
         QStringLiteral("Start-Pause"), on(PAD_BUTTON_START));
  const QPointF a(panel.right() - 330 * k, panel.top() + 285 * k);
  button(a, 66 * k, QStringLiteral("A"), on(PAD_BUTTON_A));
  button(a + QPointF(-120 * k, 72 * k), 42 * k, QStringLiteral("B"), on(PAD_BUTTON_B));
  button(a + QPointF(122 * k, -22 * k), 46 * k, QStringLiteral("X"), on(PAD_BUTTON_X));
  button(a + QPointF(-24 * k, -122 * k), 46 * k, QStringLiteral("Y"), on(PAD_BUTTON_Y));
  button(QPointF(panel.right() - 200 * k, panel.top() + 140 * k), 34 * k, QStringLiteral("Z"),
         on(PAD_TRIGGER_Z));

  // Footer: hold START to leave
  const qreal cx = width() / 2.0;
  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, panel.bottom() + 40 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("Maintiens START pour quitter"));
  const qreal hold = m_test_start_since < 0 ?
                         0 :
                         std::min<qreal>(1, (m_clock.elapsed() - m_test_start_since) / 1000.0);
  const QRectF bar(cx - 150 * k, panel.bottom() + 84 * k, 300 * k, 8 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(bar, 4 * k, 4 * k);
  if (hold > 0)
  {
    painter.setBrush(ACCENT);
    painter.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * hold, bar.height()), 4 * k,
                            4 * k);
  }
}

// ================================================================== updates

void BigPictureWindow::CheckUpdates(bool silent)
{
  if (!silent)
    Toast(tr("Recherche d'une mise à jour…"));
  BigPictureUpdater::Check(this, [this, silent](bool available, const QString& version,
                                                const QString& url, const QString& error) {
    if (!error.isEmpty())
    {
      if (!silent)
        Toast(error);
      return;
    }
    if (!available)
    {
      if (!silent)
        Toast(tr("Ton Dolphin est à jour"));
      return;
    }
    m_update_url = url;
    m_update_version = version;
    if (BigPictureUpdater::BuildId() == QLatin1String("dev"))
    {
      if (!silent)
        Toast(tr("Version locale : mise à jour automatique désactivée"));
      return;
    }
    if (m_mode == Mode::Library || m_mode == Mode::Menu)
    {
      m_audio->Play(Sound::MenuOpen);
      m_overlay_t = 0;
      m_mode = Mode::ConfirmUpdate;
    }
    else
    {
      Notify(tr("Une mise à jour de Dolphin est disponible"));
    }
  });
}

// ================================================================== GameCube intro

void BigPictureWindow::DrawIntroGameCube(QPainter& painter)
{
  // A small cube rolls along a "G" drawn on a 3x3 grid, leaving its trail,
  // then the big cube appears with the title (like the console's boot).
  const qreal k = height() / 1080.0;
  const qreal t = m_intro_t * 4.6;
  painter.fillRect(rect(), Qt::black);

  const qreal cell = 70 * k;
  const QPointF origin(width() / 2.0 - 1.5 * cell, height() / 2.0 - 1.5 * cell - 60 * k);
  // Path through the cells of a 3x3 grid forming a G (col,row)
  static constexpr int path[][2] = {{2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2},
                                    {1, 2}, {2, 2}, {2, 1}, {1, 1}};
  constexpr int steps = static_cast<int>(std::size(path));
  const qreal roll_time = 2.2;
  const qreal progress = std::clamp<qreal>(t / roll_time, 0, 1) * (steps - 1);
  const int done = static_cast<int>(progress);

  const QColor indigo(110, 86, 230);
  const QColor indigo_light(170, 150, 255);
  const qreal assemble = std::clamp<qreal>((t - roll_time) / 0.6, 0, 1);

  // Trail: cells already visited light up.
  for (int i = 0; i <= done && i < steps; ++i)
  {
    const QRectF r(origin.x() + path[i][0] * cell, origin.y() + path[i][1] * cell, cell, cell);
    QColor c = indigo;
    c.setAlphaF(static_cast<float>(0.85 * (1 - assemble * 0.4)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(c);
    painter.drawRect(r.adjusted(3 * k, 3 * k, -3 * k, -3 * k));
  }

  if (assemble <= 0)
  {
    // The rolling cube, rotating a quarter turn per cell.
    const int i = std::min(done, steps - 2);
    const qreal f = EaseInOut(progress - i);
    const QPointF a(origin.x() + (path[i][0] + 0.5) * cell, origin.y() + (path[i][1] + 0.5) * cell);
    const QPointF b(origin.x() + (path[i + 1][0] + 0.5) * cell,
                    origin.y() + (path[i + 1][1] + 0.5) * cell);
    const QPointF pos = a + (b - a) * f;
    painter.save();
    painter.translate(pos);
    painter.rotate(90 * f * ((path[i + 1][0] - path[i][0]) + (path[i + 1][1] - path[i][1])));
    painter.setBrush(indigo_light);
    painter.setPen(QPen(Qt::white, 2 * k));
    painter.drawRect(QRectF(-cell * 0.42, -cell * 0.42, cell * 0.84, cell * 0.84));
    painter.restore();
  }
  else
  {
    // The G closes into the big cube outline.
    const QRectF box(origin.x(), origin.y(), 3 * cell, 3 * cell);
    painter.setBrush(Qt::NoBrush);
    QColor edge = indigo_light;
    edge.setAlphaF(static_cast<float>(assemble));
    painter.setPen(QPen(edge, 5 * k));
    painter.drawRect(box.adjusted(-6 * k, -6 * k, 6 * k, 6 * k));
    // Isometric top/side faces
    const qreal d = 40 * k * assemble;
    QPolygonF top, side;
    top << box.topLeft() + QPointF(-6 * k, -6 * k) << box.topLeft() + QPointF(d - 6 * k, -d - 6 * k)
        << box.topRight() + QPointF(d + 6 * k, -d - 6 * k) << box.topRight() + QPointF(6 * k, -6 * k);
    side << box.topRight() + QPointF(6 * k, -6 * k) << box.topRight() + QPointF(d + 6 * k, -d - 6 * k)
         << box.bottomRight() + QPointF(d + 6 * k, -d + 6 * k)
         << box.bottomRight() + QPointF(6 * k, 6 * k);
    painter.drawPolygon(top);
    painter.drawPolygon(side);
  }

  // Title
  const qreal title = std::clamp<qreal>((t - roll_time - 0.3) / 0.8, 0, 1);
  if (title > 0)
  {
    painter.setOpacity(EaseOut(title));
    painter.setFont(Font(true, int(76 * k), 700));
    painter.setPen(Qt::white);
    const qreal ty = origin.y() + 3 * cell + 70 * k + (1 - EaseOut(title)) * 20 * k;
    painter.drawText(QRectF(0, ty, width(), 100 * k), Qt::AlignCenter, QStringLiteral("Dolphin"));
    QFont sub = Font(false, int(20 * k), 600);
    sub.setLetterSpacing(QFont::AbsoluteSpacing, 6 * k);
    painter.setFont(sub);
    painter.setPen(indigo_light);
    painter.drawText(QRectF(0, ty + 100 * k, width(), 36 * k), Qt::AlignCenter, tr("MODE MANETTE"));
    painter.setOpacity(1);
  }
}

// ================================================================== USB driver (no Zadig)

void BigPictureWindow::CheckDrivers()
{
  if (!m_active || !isVisible() || m_driver_busy)
    return;
  if (m_mode != Mode::Library && m_mode != Mode::Menu)
    return;
  QStringList names;
  bool new_device = false;
  for (const auto& t : BigPictureDriver::DevicesNeedingDriver())
  {
    const QString id = QString::fromStdWString(t.instance_id);
    names << QString::fromStdString(t.description);
    if (!m_driver_asked.contains(id))
    {
      m_driver_asked << id;
      new_device = true;
    }
  }
  if (!new_device)
    return;
  names.removeDuplicates();
  m_driver_devices = names.join(QStringLiteral(", "));
  m_mode_before_driver = m_mode == Mode::Menu ? Mode::Library : m_mode;
  m_audio->Play(Sound::MenuOpen);
  m_overlay_t = 0;
  m_mode = Mode::ConfirmDriver;
}

void BigPictureWindow::InstallDrivers()
{
  m_driver_busy = true;
  if (m_mode != Mode::ConfirmDriver)
  {
    m_overlay_t = 0;
    m_mode = Mode::ConfirmDriver;
  }
  BigPictureDriver::InstallAsync(this, [this](bool ok) {
    m_driver_busy = false;
    BeginTransition(0);
    m_mode = m_mode_before_driver == Mode::ConfirmDriver ? Mode::Library : m_mode_before_driver;
    if (ok)
    {
      m_audio->Play(Sound::Confirm);
      Toast(tr("Pilote installé : la manette est prête"));
      // Let Windows restart the device, then look again.
      QTimer::singleShot(1500, this, [] { g_controller_interface.RefreshDevices(); });
    }
    else
    {
      m_audio->Play(Sound::Error);
      Toast(tr("Pilote non installé"));
    }
  });
}

// ================================================================== launch, resume, sorting

std::vector<int> BigPictureWindow::HeroButtons(const Entry& entry) const
{
  if (HasResume(entry))
    return {HERO_RESUME, HERO_NEW, HERO_SETTINGS, HERO_FAVORITE};
  return {HERO_PLAY, HERO_SETTINGS, HERO_FAVORITE};
}

void BigPictureWindow::ActivateHero(int button)
{
  const Entry* sel = Selected();
  if (!sel)
    return;
  switch (button)
  {
  case HERO_RESUME:
    Launch(*sel, 0, true);
    break;
  case HERO_PLAY:
  case HERO_NEW:
    StartNewGame(*sel);
    break;
  case HERO_SETTINGS:
    m_audio->Play(Sound::Confirm);
    OpenGameSettings();
    break;
  case HERO_FAVORITE:
    ToggleFavorite();
    break;
  default:
    break;
  }
}

void BigPictureWindow::StartNewGame(const Entry& entry)
{
  // Multi-disc games start on disc 1: the game asks for the next one and Dolphin swaps it.
  Launch(entry, 0, false);
}

void BigPictureWindow::Launch(const Entry& entry, int disc, bool resume)
{
  const std::string id = entry.game->GetGameID();
  if (resume)
  {
    disc = Settings::GetQSettings()
               .value(QStringLiteral("bigpicture/resume_disc/") + QString::fromStdString(id), 0)
               .toInt();
  }
  disc = std::clamp(disc, 0, std::max(0, static_cast<int>(entry.discs.size()) - 1));
  const QString path = entry.discs.isEmpty() ? entry.path : entry.discs[disc];
  const QString state = resume ? ResumeStatePath(id) : QString();

  m_audio->Play(Sound::Launch);
  SyncSaves(false);  // latest saves from the other machine
  {
    const QScreen* scr = screen();
    const int h = scr ? int(scr->geometry().height() * scr->devicePixelRatio()) : 1080;
    ApplyBigPictureDisplayPreset(h);
  }
  Settings::GetQSettings().setValue(LastPlayedKey(entry.path), QDateTime::currentSecsSinceEpoch());
  m_running_entry = entry;
  m_disc = disc;
  m_hero_focus = false;
  setCursor(Qt::BlankCursor);
  m_audio->StopMusic();
  BeginTransition(0);
  m_mode = Mode::Launching;
  m_fade_in = 1;
  // Let the select sound play before the game takes over.
  QTimer::singleShot(350, this, [this, path, state] { emit LaunchRequested(path, state); });
}

QString BigPictureWindow::ResumeStatePath(const std::string& game_id) const
{
  return QString::fromStdString(File::GetUserPath(D_STATESAVES_IDX) + game_id) +
         QStringLiteral(".manette-reprise.sav");
}

QString BigPictureWindow::ResumeShotPath(const std::string& game_id) const
{
  return QString::fromStdString(File::GetUserPath(D_STATESAVES_IDX) + game_id) +
         QStringLiteral(".manette-reprise.png");
}

bool BigPictureWindow::HasResume(const Entry& entry) const
{
  if (!entry.game ||
      !Settings::GetQSettings().value(QStringLiteral("bigpicture/resume"), true).toBool())
    return false;
  const QString id = QString::fromStdString(entry.game->GetGameID());
  if (id.isEmpty())
    return false;
  auto it = m_resume_known.find(id);
  if (it == m_resume_known.end())
    it = m_resume_known.insert(id, QFileInfo::exists(ResumeStatePath(id.toStdString())));
  return it.value();
}

const QPixmap& BigPictureWindow::ResumeShot(const Entry& entry)
{
  const QString path = ResumeShotPath(entry.game->GetGameID());
  if (path != m_resume_shot_path)
  {
    m_resume_shot_path = path;
    m_resume_shot = QPixmap(path);
  }
  return m_resume_shot;
}

void BigPictureWindow::SaveResume()
{
  const std::string id = SConfig::GetInstance().GetGameID();
  if (id.empty() || !m_running_entry.game ||
      !Settings::GetQSettings().value(QStringLiteral("bigpicture/resume"), true).toBool())
    return;
  Settings::GetQSettings().setValue(
      QStringLiteral("bigpicture/resume_aspect/") + QString::fromStdString(id), m_game_aspect);
  auto& system = Core::System::GetInstance();
  // The game is paused: the state is taken now and written in the background.
  State::SaveAs(system, ResumeStatePath(id).toStdString());
  const QString shot = ResumeShotPath(id);
  QFile::remove(shot);
  QFile::copy(PauseShotPath(), shot);
  Settings::GetQSettings().setValue(
      QStringLiteral("bigpicture/resume_disc/") + QString::fromStdString(id), m_disc);
  m_resume_known.clear();
  m_resume_shot_path.clear();
}

void BigPictureWindow::CycleSort()
{
  if (m_tab == Tab::Recent || m_tab == Tab::Settings)
  {
    m_audio->Play(Sound::Error);
    return;
  }
  auto& settings = Settings::GetQSettings();
  const int sort = (settings.value(QStringLiteral("bigpicture/sort"), 0).toInt() + 1) % 3;
  settings.setValue(QStringLiteral("bigpicture/sort"), sort);
  m_audio->Play(Sound::Toggle);
  BeginTransition(0);
  ApplyTab();
}

void BigPictureWindow::DrawResumeCard(QPainter& painter, const Entry& sel, const QRectF& r)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_hero_t);
  painter.save();
  painter.setOpacity(e);
  QPainterPath clip;
  clip.addRoundedRect(r, 26 * k, 26 * k);

  // shadow
  painter.setPen(Qt::NoPen);
  for (int i = 1; i <= 4; ++i)
  {
    painter.setBrush(QColor(0, 0, 0, 22));
    painter.drawRoundedRect(r.adjusted(-i * 4 * k, -i * 2 * k + 10 * k, i * 4 * k, i * 6 * k),
                            (26 + i * 4) * k, (26 + i * 4) * k);
  }

  painter.setClipPath(clip);
  const QPixmap& shot = ResumeShot(sel);
  if (!shot.isNull())
  {
    // The card has the game's proportions (4:3 or 16:9)
    painter.drawPixmap(r, shot, QRectF(shot.rect()));
  }
  else
  {
    painter.fillRect(r, SURFACE);
  }
  QLinearGradient shade(r.left(), r.bottom() - 180 * k, r.left(), r.bottom());
  shade.setColorAt(0, QColor(0, 0, 0, 0));
  shade.setColorAt(1, QColor(8, 4, 2, 235));
  painter.fillRect(r, shade);
  painter.setClipping(false);
  painter.setPen(QPen(LINE, 1.5 * k));
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(clip);

  const auto tag = [&](const QString& text, bool left, QColor color) {
    const QFont f = Font(false, int(14 * k), 700);
    QFont spaced = f;
    spaced.setLetterSpacing(QFont::AbsoluteSpacing, 1 * k);
    const qreal tw = QFontMetricsF(spaced).horizontalAdvance(text) + 28 * k;
    const QRectF t(left ? r.left() + 20 * k : r.right() - 20 * k - tw, r.top() + 20 * k, tw,
                   34 * k);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(10, 5, 2, 190));
    painter.drawRoundedRect(t, 11 * k, 11 * k);
    painter.setFont(spaced);
    painter.setPen(color);
    painter.drawText(t, Qt::AlignCenter, text);
  };
  tag(tr("●  REPRISE AUTOMATIQUE"), true, QColor(255, 246, 236));

  painter.setFont(Font(true, int(28 * k), 700));
  painter.setPen(QColor(255, 246, 236));
  painter.drawText(QRectF(r.left() + 28 * k, r.bottom() - 92 * k, r.width() - 56 * k, 40 * k),
                   Qt::AlignVCenter, tr("Reprends là où tu t'étais arrêté"));
  const QDateTime when = QFileInfo(ResumeStatePath(sel.game->GetGameID())).lastModified();
  const qint64 days = when.date().daysTo(QDate::currentDate());
  const QString day = days == 0 ? tr("Aujourd'hui") :
                      days == 1 ? tr("Hier") :
                                  QLocale().toString(when.date(), QStringLiteral("d MMMM"));
  painter.setFont(Font(false, int(17 * k), 500));
  painter.setPen(QColor(232, 200, 168));
  painter.drawText(QRectF(r.left() + 28 * k, r.bottom() - 52 * k, r.width() - 56 * k, 28 * k),
                   Qt::AlignVCenter,
                   tr("%1 à %2 · Sauvegarde automatique").arg(day, when.toString(QStringLiteral("HH:mm"))));
  painter.restore();
}

void BigPictureWindow::DrawChooseDisc(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal e = EaseOut(m_overlay_t);
  QColor shade(0, 0, 0);
  shade.setAlphaF(static_cast<float>(0.6 * e));
  painter.fillRect(rect(), shade);

  painter.setOpacity(e);
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 280 * k, width(), 30 * k), Qt::AlignCenter, tr("QUEL DISQUE ?"));
  painter.setFont(Font(true, int(46 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 316 * k, width(), 70 * k), Qt::AlignCenter, m_choose_entry.title);

  const int n = static_cast<int>(m_choose_entry.discs.size());
  const qreal cw = 300 * k, ch = 340 * k, gap = 40 * k;
  const qreal x0 = (width() - (n * cw + (n - 1) * gap)) / 2;
  for (int i = 0; i < n; ++i)
  {
    const QRectF c(x0 + i * (cw + gap), 430 * k, cw, ch);
    const bool on = i == m_choose_disc;
    painter.setPen(on ? QPen(ACCENT, 4 * k) : QPen(LINE, 2 * k));
    painter.setBrush(on ? RAISED : SURFACE);
    painter.drawRoundedRect(c, 28 * k, 28 * k);
    // a mini disc
    const QPointF dc(c.center().x(), c.top() + 140 * k);
    QRadialGradient g(dc, 95 * k);
    g.setColorAt(0, QColor(250, 250, 255));
    g.setColorAt(0.6, QColor(200, 205, 220));
    g.setColorAt(1, on ? ACCENT_SOFT : QColor(150, 150, 170));
    painter.setPen(Qt::NoPen);
    painter.setBrush(g);
    painter.drawEllipse(dc, 95 * k, 95 * k);
    painter.setBrush(on ? RAISED : SURFACE);
    painter.drawEllipse(dc, 16 * k, 16 * k);
    painter.setFont(Font(true, int(30 * k), 700));
    painter.setPen(on ? TEXT : TEXT_SOFT);
    painter.drawText(QRectF(c.left(), c.bottom() - 90 * k, cw, 50 * k), Qt::AlignCenter,
                     tr("Disque %1").arg(i + 1));
  }
  painter.setOpacity(1);
  DrawHints(painter, {{QStringLiteral("Control_Stick"), tr("Choisir")}},
            {{QStringLiteral("A"), tr("Lancer")}, {QStringLiteral("B"), tr("Retour")}});
}

void BigPictureWindow::DrawControllerChips(QPainter& painter, qreal right, qreal cy)
{
  const qreal k = height() / 1080.0;
  struct Chip
  {
    QString player, name;
    bool wireless = false;
    qreal battery = -1;  // percent, -1 = unknown
    bool missing = false;
  };
  std::vector<Chip> chips;
  for (int port = 0; port < 4; ++port)
  {
    if (port > 0 && Config::Get(Config::GetInfoForSIDevice(port)) == SerialInterface::SIDEVICE_NONE)
      continue;
    auto* pad = PadController(port);
    if (!pad)
      continue;
    Chip c;
    c.player = QStringLiteral("J%1").arg(port + 1);
    const auto& dq = pad->GetDefaultDevice();
    const auto device = g_controller_interface.FindDevice(dq);
    if (!device)
    {
      if (port > 0)
        continue;
      c.missing = true;
      c.name = tr("Manette non configurée");
      chips.push_back(c);
      continue;
    }
    const QString n = QString::fromStdString(dq.name);
    if (n.contains(QStringLiteral("GameCube"), Qt::CaseInsensitive))
      c.name = tr("GameCube");
    else if (n.contains(QStringLiteral("Pro Controller"), Qt::CaseInsensitive))
      c.name = dq.source == "Switch2" ? tr("Pro 2") : tr("Pro");
    else if (n.contains(QStringLiteral("Joy-Con"), Qt::CaseInsensitive))
      c.name = n;
    else if (n.contains(QStringLiteral("Xbox"), Qt::CaseInsensitive) || dq.source == "XInput")
      c.name = tr("Xbox");
    else if (n.contains(QStringLiteral("DualSense"), Qt::CaseInsensitive) ||
             n.contains(QStringLiteral("PS5"), Qt::CaseInsensitive))
      c.name = tr("DualSense");
    else if (n.contains(QStringLiteral("Keyboard"), Qt::CaseInsensitive))
      c.name = tr("Clavier");
    else
      c.name = n.size() > 18 ? n.left(16) + QStringLiteral("…") : n;
    c.wireless = dq.source == "Switch2";
    if (const auto* battery = device->FindInput("Battery"))
    {
      const ControlState level = battery->GetState();
      if (level > 0 && (c.wireless || level < 99.5))
        c.battery = level;
    }
    chips.push_back(c);
  }

  qreal x = right;
  for (auto it = chips.rbegin(); it != chips.rend(); ++it)
  {
    const Chip& c = *it;
    const QFont bold = Font(false, int(18 * k), 700);
    const QFont normal = Font(false, int(18 * k), 500);
    const qreal pw = c.missing ? 0 : QFontMetricsF(bold).horizontalAdvance(c.player) + 10 * k;
    const qreal nw = QFontMetricsF(normal).horizontalAdvance(c.name);
    const qreal extra = (c.wireless ? 20 * k : 0) + (c.battery >= 0 ? 44 * k : 0);
    const qreal w = 22 * k + (c.missing ? 22 * k : 0) + pw + nw + extra + 22 * k;
    const QRectF r(x - w, cy - 24 * k, w, 48 * k);
    painter.setPen(QPen(LINE, 1 * k));
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(r, 24 * k, 24 * k);
    qreal tx = r.left() + 22 * k;
    if (c.missing)
    {
      QColor dot = WARN;
      dot.setAlphaF(static_cast<float>(0.55 + 0.45 * std::sin(m_time * 4)));
      painter.setPen(Qt::NoPen);
      painter.setBrush(dot);
      painter.drawEllipse(QPointF(tx + 4 * k, cy), 6 * k, 6 * k);
      tx += 22 * k;
    }
    else
    {
      painter.setFont(bold);
      painter.setPen(TEXT);
      painter.drawText(QRectF(tx, r.top(), pw, r.height()), Qt::AlignVCenter, c.player);
      tx += pw;
    }
    painter.setFont(normal);
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(tx, r.top(), nw + 2, r.height()), Qt::AlignVCenter, c.name);
    tx += nw + 8 * k;
    if (c.wireless)
    {
      // Bluetooth rune
      QPainterPath bt;
      const qreal bx = tx + 4 * k, top = cy - 9 * k, bot = cy + 9 * k;
      bt.moveTo(bx - 5 * k, cy - 4.5 * k);
      bt.lineTo(bx + 5 * k, cy + 4.5 * k);
      bt.lineTo(bx, bot);
      bt.lineTo(bx, top);
      bt.lineTo(bx + 5 * k, cy - 4.5 * k);
      bt.lineTo(bx - 5 * k, cy + 4.5 * k);
      painter.setPen(QPen(TEXT_SOFT, 1.6 * k));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(bt);
      tx += 20 * k;
    }
    if (c.battery >= 0)
    {
      const QRectF body(tx + 4 * k, cy - 7.5 * k, 28 * k, 15 * k);
      painter.setPen(QPen(TEXT_SOFT, 1.8 * k));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(body, 3.5 * k, 3.5 * k);
      painter.setPen(Qt::NoPen);
      painter.setBrush(TEXT_SOFT);
      painter.drawRect(QRectF(body.right() + 1 * k, cy - 3 * k, 2.5 * k, 6 * k));
      const qreal level = std::clamp<qreal>(c.battery / 100.0, 0.08, 1.0);
      painter.setBrush(c.battery <= 25 ? WARN : OK_GREEN);
      painter.drawRoundedRect(QRectF(body.left() + 2.5 * k, body.top() + 2.5 * k,
                                     (body.width() - 5 * k) * level, body.height() - 5 * k),
                              1.5 * k, 1.5 * k);
    }
    x = r.left() - 12 * k;
  }
}

// ================================================================== memory card writes

void BigPictureWindow::RemoveSave(CardView& card, u8 index)
{
  if (card.folder)
  {
    if (const auto it = card.files.find(index); it != card.files.end())
      File::Delete(it->second);
    return;
  }
  (void)card.card->RemoveFile(index);
  (void)card.card->Save();
}

int BigPictureWindow::ImportSave(CardView& card, u8 src_index, CardView& from)
{
  using Ret = Memcard::GCMemcardImportFileRetVal;
  const auto file = from.card->ExportFile(src_index);
  if (!file || !card.card)
    return static_cast<int>(Ret::NOMEMCARD);
  // Same checks as a real card (space, duplicate) on the in-memory copy
  const Ret ret = card.card->ImportFile(*file);
  if (ret != Ret::SUCCESS)
    return static_cast<int>(ret);
  if (card.folder)
  {
    const std::string path = card.path.toStdString() + DIR_SEP +
                             Memcard::GenerateFilename(file->dir_entry) +
                             Memcard::GetDefaultExtension(Memcard::SavefileFormat::GCI);
    if (!Memcard::WriteSavefile(path, *file, Memcard::SavefileFormat::GCI))
      return static_cast<int>(Ret::NOMEMCARD);
  }
  else
  {
    (void)card.card->Save();
  }
  return static_cast<int>(ret);
}

qreal BigPictureWindow::GameAspect() const
{
  switch (Config::Get(Config::GFX_ASPECT_RATIO))
  {
  case AspectMode::ForceWide:
    return 16.0 / 9.0;
  case AspectMode::ForceStandard:
    return 4.0 / 3.0;
  case AspectMode::Stretch:
    return height() > 0 ? qreal(width()) / height() : 16.0 / 9.0;
  default:
    // Automatic: what the game itself uses
    return (g_widescreen && g_widescreen->IsGameWidescreen()) ? 16.0 / 9.0 : 4.0 / 3.0;
  }
}

qreal BigPictureWindow::ResumeAspect(const Entry& entry) const
{
  const qreal a = Settings::GetQSettings()
                      .value(QStringLiteral("bigpicture/resume_aspect/") +
                                 QString::fromStdString(entry.game->GetGameID()),
                             4.0 / 3.0)
                      .toDouble();
  return std::clamp<qreal>(a, 1.0, 2.5);
}

// ================================================================== Wii

namespace
{
constexpr int WGr_code(WiimoteEmu::WiimoteGroup g)
{
  return 100 + static_cast<int>(g);
}
constexpr int NGr_code(WiimoteEmu::NunchukGroup g)
{
  return 200 + static_cast<int>(g);
}
constexpr int CGr_code(WiimoteEmu::ClassicGroup g)
{
  return 300 + static_cast<int>(g);
}
}  // namespace

bool BigPictureWindow::WiiMenuInstalled() const
{
  return QFileInfo::exists(QString::fromStdString(File::GetUserPath(D_WIIROOT_IDX)) +
                           QStringLiteral("/title/00000001/00000002/content/title.tmd"));
}

void BigPictureWindow::LaunchWiiMenu()
{
  if (!WiiMenuInstalled())
  {
    m_audio->Play(Sound::Error);
    Toast(tr("Menu Wii non installé (interface classique › Outils › Mettre à jour la console)"));
    return;
  }
  m_audio->Play(Sound::Launch);
  m_running_entry = Entry{};
  m_disc = 0;
  setCursor(Qt::BlankCursor);
  m_audio->StopMusic();
  BeginTransition(0);
  m_mode = Mode::Launching;
  m_fade_in = 1;
  QTimer::singleShot(350, this, [this] { emit LaunchRequested(QStringLiteral("wii-menu:"), {}); });
}

void BigPictureWindow::OpenWiiSaves()
{
  if (!Core::IsUninitialized(Core::System::GetInstance()))
  {
    m_audio->Play(Sound::Error);
    Toast(tr("Quitte le jeu pour gérer les sauvegardes Wii"));
    return;
  }
  m_wii_saves.clear();
  const QString root =
      QString::fromStdString(File::GetUserPath(D_WIIROOT_IDX)) + QStringLiteral("/title/");
  for (const QString& hi : {QStringLiteral("00010000"), QStringLiteral("00010001"),
                            QStringLiteral("00010004"), QStringLiteral("00010005")})
  {
    const QDir dir(root + hi);
    for (const QString& lo : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
      const QString save_dir = dir.filePath(lo) + QStringLiteral("/data");
      if (!QFileInfo::exists(save_dir + QStringLiteral("/banner.bin")))
        continue;
      bool ok1 = false, ok2 = false;
      const u64 tid = (u64(hi.toUInt(&ok1, 16)) << 32) | lo.toUInt(&ok2, 16);
      if (!ok1 || !ok2)
        continue;
      DiscIO::WiiSaveBanner banner(tid);
      if (!banner.IsValid())
        continue;
      WiiSaveEntry e;
      e.title_id = tid;
      e.path = save_dir;
      e.name = QString::fromStdString(banner.GetName()).trimmed();
      e.description = QString::fromStdString(banner.GetDescription()).trimmed();
      u32 w = 0, h = 0;
      const std::vector<u32> pixels = banner.GetBanner(&w, &h);
      if (!pixels.empty())
      {
        const QImage img(reinterpret_cast<const uchar*>(pixels.data()), int(w), int(h),
                         QImage::Format_ARGB32);
        e.banner = QPixmap::fromImage(img.copy());
      }
      QDirIterator it(save_dir, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
      {
        it.next();
        e.bytes += it.fileInfo().size();
      }
      m_wii_saves.push_back(std::move(e));
    }
  }
  std::sort(m_wii_saves.begin(), m_wii_saves.end(),
            [](const WiiSaveEntry& a, const WiiSaveEntry& b) {
              return a.name.localeAwareCompare(b.name) < 0;
            });
  m_ws_index = 0;
  m_ws_actions = false;
  m_ws_confirm = false;
  BeginTransition(0);
  m_mode = Mode::WiiSaves;
}

void BigPictureWindow::HandleWiiSaves(Action action)
{
  const int count = static_cast<int>(m_wii_saves.size());
  if (m_ws_actions)
  {
    const int items = m_ws_confirm ? 2 : 3;
    switch (action)
    {
    case Action::Up:
    case Action::Down:
      m_ws_action = (m_ws_action + (action == Action::Down ? 1 : items - 1)) % items;
      m_audio->Play(Sound::Move);
      break;
    case Action::Back:
      m_ws_actions = false;
      m_ws_confirm = false;
      m_audio->Play(Sound::Back);
      break;
    case Action::Accept:
    {
      if (count == 0)
        break;
      const WiiSaveEntry& e = m_wii_saves[std::clamp(m_ws_index, 0, count - 1)];
      if (m_ws_confirm)
      {
        if (m_ws_action == 0)
        {
          // Empty the save folder through the Wii's file system (keeps it consistent)
          IOS::HLE::Kernel ios;
          const auto fs = ios.GetFS();
          const std::string path = fmt::format("/title/{:08x}/{:08x}/data",
                                               static_cast<u32>(e.title_id >> 32),
                                               static_cast<u32>(e.title_id));
          if (const auto entries = fs->ReadDirectory(0, 0, path))
          {
            for (const std::string& name : *entries)
              (void)fs->Delete(0, 0, path + "/" + name);
          }
          m_audio->Play(Sound::Erase);
          Toast(tr("Sauvegarde effacée"));
          m_ws_actions = false;
          m_ws_confirm = false;
          const int keep = m_ws_index;
          OpenWiiSaves();
          m_ws_index = std::clamp(keep, 0, std::max(0, int(m_wii_saves.size()) - 1));
        }
        else
        {
          m_audio->Play(Sound::Cancel);
          m_ws_actions = false;
          m_ws_confirm = false;
        }
        break;
      }
      if (m_ws_action == 0)
      {
        const QString out =
            QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                .filePath(QStringLiteral("Sauvegardes Wii"));
        QDir().mkpath(out);
        const auto result = WiiSave::Export(e.title_id, out.toStdString());
        m_audio->Play(result == WiiSave::CopyResult::Success ? Sound::SaveState : Sound::Error);
        Toast(result == WiiSave::CopyResult::Success ?
                  tr("Exportée dans Documents › Sauvegardes Wii") :
                  tr("Export impossible"));
        m_ws_actions = false;
      }
      else if (m_ws_action == 1)
      {
        m_ws_confirm = true;
        m_ws_action = 1;  // "Annuler" selected by default
        m_audio->Play(Sound::MenuOpen);
      }
      else
      {
        m_ws_actions = false;
        m_audio->Play(Sound::Back);
      }
      break;
    }
    default:
      break;
    }
    return;
  }
  switch (action)
  {
  case Action::Up:
  case Action::Down:
  {
    const int next = std::clamp(m_ws_index + (action == Action::Down ? 1 : -1), 0,
                                std::max(0, count - 1));
    if (next != m_ws_index)
    {
      m_ws_index = next;
      m_audio->Play(Sound::Move);
    }
    break;
  }
  case Action::Accept:
    if (count > 0)
    {
      m_ws_actions = true;
      m_ws_action = 0;
      m_audio->Play(Sound::MenuOpen);
    }
    break;
  case Action::Back:
  case Action::Menu:
    m_audio->Play(Sound::Back);
    BeginTransition(0);
    m_mode = Mode::Library;
    break;
  default:
    break;
  }
}

void BigPictureWindow::DrawWiiSaves(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(80 * k, 170 * k, 1200 * k, 30 * k), Qt::AlignVCenter,
                   tr("SAUVEGARDES WII · %1").arg(m_wii_saves.size()));
  if (m_wii_saves.empty())
  {
    painter.setFont(Font(true, int(36 * k), 700));
    painter.setPen(TEXT);
    painter.drawText(QRectF(0, 400 * k, width(), 60 * k), Qt::AlignCenter,
                     tr("Aucune sauvegarde Wii pour l'instant"));
    DrawHints(painter, {}, {{QStringLiteral("B"), tr("Retour")}});
    return;
  }
  const qreal row_h = 96 * k, top = 220 * k;
  const int visible = std::max(1, int((height() - top - 140 * k) / row_h));
  const int first = std::clamp(m_ws_index - visible / 2, 0,
                               std::max(0, int(m_wii_saves.size()) - visible));
  for (int i = first; i < std::min<int>(first + visible, int(m_wii_saves.size())); ++i)
  {
    const WiiSaveEntry& e = m_wii_saves[i];
    const QRectF r(80 * k, top + (i - first) * row_h, width() - 160 * k, row_h - 12 * k);
    const bool on = i == m_ws_index;
    painter.setPen(on ? QPen(ACCENT, 3 * k) : QPen(LINE, 1 * k));
    painter.setBrush(on ? RAISED : SURFACE);
    painter.drawRoundedRect(r, 18 * k, 18 * k);
    const QRectF br(r.left() + 14 * k, r.top() + 10 * k, (r.height() - 20 * k) * 3, r.height() - 20 * k);
    if (!e.banner.isNull())
      painter.drawPixmap(br, e.banner, QRectF(e.banner.rect()));
    painter.setFont(Font(false, int(22 * k), 600));
    painter.setPen(TEXT);
    painter.drawText(QRectF(br.right() + 20 * k, r.top() + 12 * k, r.width() - br.width() - 260 * k,
                            34 * k),
                     Qt::AlignVCenter, e.name);
    painter.setFont(Font(false, int(17 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(br.right() + 20 * k, r.top() + 46 * k, r.width() - br.width() - 260 * k,
                            28 * k),
                     Qt::AlignVCenter, e.description);
    painter.setPen(TEXT_DIM);
    painter.drawText(QRectF(r.right() - 220 * k, r.top(), 200 * k, r.height()),
                     Qt::AlignVCenter | Qt::AlignRight,
                     e.bytes >= 1024 * 1024 ? tr("%1 Mo").arg(e.bytes / (1024.0 * 1024.0), 0, 'f', 1) :
                                              tr("%1 Ko").arg(std::max<qint64>(1, e.bytes / 1024)));
  }

  if (m_ws_actions)
  {
    const QStringList items = m_ws_confirm ?
                                  QStringList{tr("Effacer définitivement"), tr("Annuler")} :
                                  QStringList{tr("Exporter (Documents)"), tr("Effacer"), tr("Annuler")};
    const qreal w = 520 * k, h = 90 * k + items.size() * 70 * k;
    const QRectF panel((width() - w) / 2, (height() - h) / 2, w, h);
    painter.fillRect(rect(), QColor(0, 0, 0, 140));
    painter.setPen(QPen(LINE, 2 * k));
    painter.setBrush(SURFACE);
    painter.drawRoundedRect(panel, 26 * k, 26 * k);
    painter.setFont(Font(false, int(20 * k), 600));
    painter.setPen(ACCENT_SOFT);
    painter.drawText(QRectF(panel.left(), panel.top() + 20 * k, w, 40 * k), Qt::AlignCenter,
                     m_wii_saves[m_ws_index].name);
    for (int i = 0; i < items.size(); ++i)
    {
      const QRectF r(panel.left() + 30 * k, panel.top() + 76 * k + i * 70 * k, w - 60 * k, 58 * k);
      const bool on = i == m_ws_action;
      painter.setPen(Qt::NoPen);
      painter.setBrush(on ? ACCENT : RAISED);
      painter.drawRoundedRect(r, 14 * k, 14 * k);
      painter.setFont(Font(false, int(22 * k), on ? 600 : 500));
      painter.setPen(on ? BG : (m_ws_confirm && i == 0 ? DANGER : TEXT));
      painter.drawText(r, Qt::AlignCenter, items[i]);
    }
  }
  DrawHints(painter, {{QStringLiteral("Control_Stick"), tr("Choisir")}},
            {{QStringLiteral("A"), tr("Options")}, {QStringLiteral("B"), tr("Retour")}});
}

void BigPictureWindow::DrawWiiTest(QPainter& painter)
{
  using WGr = WiimoteEmu::WiimoteGroup;
  using NGr = WiimoteEmu::NunchukGroup;
  using CGr = WiimoteEmu::ClassicGroup;
  const qreal k = height() / 1080.0;
  const int port = m_test_port;
  const int ext = BigPictureWiiExtension(port);

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  const auto pressed = [port](int code, int index) {
    auto* g = BigPictureGroup(port, code);
    return index < int(g->controls.size()) &&
           g->controls[index]->control_ref->GetState<ControlState>() > 0.5;
  };
  const auto value = [port](int code, int index) {
    auto* g = BigPictureGroup(port, code);
    return index < int(g->controls.size()) ?
               std::clamp<ControlState>(g->controls[index]->control_ref->GetState<ControlState>(),
                                        0, 1) :
               0.0;
  };
  const auto stick_state = [port](int code) {
    return static_cast<ControllerEmu::ReshapableInput*>(BigPictureGroup(port, code))
        ->GetReshapableState(true);
  };

  QFont label = Font(false, int(18 * k), 600);
  label.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 90 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("TEST DE LA WIIMOTE — JOUEUR %1").arg(port + 1));
  painter.setFont(Font(true, int(40 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(0, 125 * k, width(), 60 * k), Qt::AlignCenter,
                   ext == 1 ? tr("Wiimote + Nunchuk") :
                   ext == 2 ? tr("Manette classique") :
                              tr("Wiimote"));

  const auto button = [&](QPointF c, qreal r, const QString& icon, bool on) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? ACCENT : SURFACE_DEEP);
    painter.drawEllipse(c, r, r);
    painter.setOpacity(on ? 1.0 : 0.65);
    DrawIcon(painter, icon, QRectF(c.x() - r * 0.8, c.y() - r * 0.8, r * 1.6, r * 1.6));
    painter.setOpacity(1.0);
  };
  const auto stick = [&](QPointF c, qreal r, const QString& icon, qreal x, qreal y) {
    QPolygonF oct;
    for (int i = 0; i < 8; ++i)
    {
      const qreal a = i * std::numbers::pi / 4;
      oct << QPointF(c.x() + std::cos(a) * r, c.y() - std::sin(a) * r);
    }
    painter.setPen(QPen(LINE, 2 * k));
    painter.setBrush(SURFACE_DEEP);
    painter.drawPolygon(oct);
    const QPointF p(c.x() + std::clamp<qreal>(x, -1.1, 1.1) * r,
                    c.y() - std::clamp<qreal>(y, -1.1, 1.1) * r);
    painter.setPen(QPen(TEXT, 3 * k));
    painter.drawLine(c, p);
    painter.setPen(Qt::NoPen);
    painter.setBrush(TEXT);
    painter.drawEllipse(p, 13 * k, 13 * k);
    DrawIcon(painter, icon, QRectF(c.x() - r - 56 * k, c.y() + r - 44 * k, 44 * k, 44 * k));
  };
  const auto dpad_icon = [&](QPointF c, qreal size, int code) {
    const bool any = pressed(code, 0) || pressed(code, 1) || pressed(code, 2) || pressed(code, 3);
    painter.setPen(Qt::NoPen);
    painter.setBrush(any ? ACCENT : SURFACE_DEEP);
    painter.drawEllipse(c, size * 0.62, size * 0.62);
    DrawIcon(painter, QStringLiteral("Wii-D-Pad"),
             QRectF(c.x() - size / 2, c.y() - size / 2, size, size));
  };

  const QRectF panel(width() / 2.0 - 780 * k, 220 * k, 1560 * k, 660 * k);
  painter.setPen(QPen(LINE, 2 * k));
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(panel, 56 * k, 56 * k);

  if (ext == 2)
  {
    // Classic Controller
    const int b = CGr_code(CGr::Buttons), t = CGr_code(CGr::Triggers);
    const auto bar = [&](QRectF r, const QString& icon, qreal v, bool click) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(SURFACE_DEEP);
      painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
      painter.setBrush(click ? ACCENT_SOFT : ACCENT);
      painter.drawRoundedRect(QRectF(r.left(), r.top(), r.width() * v, r.height()), r.height() / 2,
                              r.height() / 2);
      DrawIcon(painter, icon, QRectF(r.left() - 90 * k, r.center().y() - 22 * k, 80 * k, 44 * k));
    };
    bar(QRectF(panel.left() + 170 * k, panel.top() + 50 * k, 380 * k, 30 * k),
        QStringLiteral("WCC-L"), value(t, 2), pressed(t, 0));
    bar(QRectF(panel.right() - 550 * k, panel.top() + 50 * k, 380 * k, 30 * k),
        QStringLiteral("WCC-R"), value(t, 3), pressed(t, 1));
    button(QPointF(panel.left() + 300 * k, panel.top() + 140 * k), 34 * k, QStringLiteral("WCC-ZL"),
           pressed(b, 4));
    button(QPointF(panel.right() - 300 * k, panel.top() + 140 * k), 34 * k,
           QStringLiteral("WCC-ZR"), pressed(b, 5));
    dpad_icon(QPointF(panel.left() + 260 * k, panel.top() + 300 * k), 120 * k, CGr_code(CGr::DPad));
    const QPointF face(panel.right() - 260 * k, panel.top() + 300 * k);
    button(face + QPointF(72 * k, 0), 40 * k, QStringLiteral("WCC-A"), pressed(b, 0));
    button(face + QPointF(0, 72 * k), 40 * k, QStringLiteral("WCC-B"), pressed(b, 1));
    button(face + QPointF(0, -72 * k), 40 * k, QStringLiteral("WCC-X"), pressed(b, 2));
    button(face + QPointF(-72 * k, 0), 40 * k, QStringLiteral("WCC-Y"), pressed(b, 3));
    const QPointF mid(panel.center().x(), panel.top() + 300 * k);
    button(mid + QPointF(-90 * k, 0), 30 * k, QStringLiteral("Wii-Minus"), pressed(b, 6));
    button(mid, 30 * k, QStringLiteral("Wii-Home"), pressed(b, 8));
    button(mid + QPointF(90 * k, 0), 30 * k, QStringLiteral("Wii-Plus"), pressed(b, 7));
    const auto ls = stick_state(CGr_code(CGr::LeftStick));
    const auto rs = stick_state(CGr_code(CGr::RightStick));
    stick(QPointF(panel.center().x() - 240 * k, panel.top() + 500 * k), 100 * k,
          QStringLiteral("Wii-Control_Stick"), ls.x, ls.y);
    stick(QPointF(panel.center().x() + 240 * k, panel.top() + 500 * k), 100 * k,
          QStringLiteral("Wii-Control_Stick"), rs.x, rs.y);
  }
  else
  {
    // Wii Remote, standing, on the left
    const int b = WGr_code(WGr::Buttons);
    const QRectF remote(panel.left() + 90 * k, panel.top() + 40 * k, 200 * k, 580 * k);
    painter.setPen(QPen(LINE, 2 * k));
    painter.setBrush(SURFACE_DEEP);
    painter.drawRoundedRect(remote, 90 * k, 90 * k);
    const qreal cx = remote.center().x();
    dpad_icon(QPointF(cx, remote.top() + 90 * k), 100 * k, WGr_code(WGr::DPad));
    button(QPointF(cx, remote.top() + 200 * k), 44 * k, QStringLiteral("Wii-A"), pressed(b, 0));
    button(QPointF(cx - 60 * k, remote.top() + 300 * k), 24 * k, QStringLiteral("Wii-Minus"),
           pressed(b, 4));
    button(QPointF(cx, remote.top() + 300 * k), 24 * k, QStringLiteral("Wii-Home"), pressed(b, 6));
    button(QPointF(cx + 60 * k, remote.top() + 300 * k), 24 * k, QStringLiteral("Wii-Plus"),
           pressed(b, 5));
    button(QPointF(cx, remote.top() + 420 * k), 32 * k, QStringLiteral("Wii-1"), pressed(b, 2));
    button(QPointF(cx, remote.top() + 500 * k), 32 * k, QStringLiteral("Wii-2"), pressed(b, 3));
    // B is underneath: shown beside the remote
    button(QPointF(remote.right() + 60 * k, remote.top() + 200 * k), 34 * k, QStringLiteral("Wii-B"),
           pressed(b, 1));

    // Pointer
    const int pt = WGr_code(WGr::Point);
    const qreal px = value(pt, 3) - value(pt, 2), py = value(pt, 0) - value(pt, 1);
    const QRectF screen_r(panel.center().x() - 300 * k, panel.top() + 80 * k, 600 * k, 340 * k);
    painter.setPen(QPen(LINE, 2 * k));
    painter.setBrush(SURFACE_DEEP);
    painter.drawRoundedRect(screen_r, 18 * k, 18 * k);
    const QPointF dot(screen_r.center().x() + px * screen_r.width() / 2.2,
                      screen_r.center().y() - py * screen_r.height() / 2.2);
    painter.setPen(QPen(TEXT, 3 * k));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(dot, 18 * k, 18 * k);
    painter.drawLine(dot - QPointF(28 * k, 0), dot + QPointF(28 * k, 0));
    painter.drawLine(dot - QPointF(0, 28 * k), dot + QPointF(0, 28 * k));
    painter.setFont(Font(false, int(17 * k), 500));
    painter.setPen(TEXT_DIM);
    painter.drawText(QRectF(screen_r.left(), screen_r.bottom() + 10 * k, screen_r.width(), 26 * k),
                     Qt::AlignCenter, tr("Pointeur"));
    const bool shake = pressed(WGr_code(WGr::Shake), 0) || pressed(WGr_code(WGr::Shake), 1) ||
                       pressed(WGr_code(WGr::Shake), 2);
    painter.setFont(Font(false, int(20 * k), 600));
    painter.setPen(shake ? ACCENT_SOFT : TEXT_DIM);
    painter.drawText(QRectF(screen_r.left(), screen_r.bottom() + 60 * k, screen_r.width(), 30 * k),
                     Qt::AlignCenter, shake ? tr("Secoué !") : tr("Secouer : —"));

    if (ext == 1)
    {
      const int nb = NGr_code(NGr::Buttons);
      const QPointF nc(panel.right() - 260 * k, panel.top() + 330 * k);
      const auto ns = stick_state(NGr_code(NGr::Stick));
      stick(nc, 120 * k, QStringLiteral("Wii-Control_Stick"), ns.x, ns.y);
      button(nc + QPointF(-60 * k, -200 * k), 34 * k, QStringLiteral("Wii-C"), pressed(nb, 0));
      button(nc + QPointF(60 * k, -200 * k), 34 * k, QStringLiteral("Wii-Z"), pressed(nb, 1));
    }
  }

  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(0, panel.bottom() + 40 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("Maintiens START pour quitter"));
}

void BigPictureWindow::SyncSaves(bool announce)
{
  if (BigPictureSync::Folder().isEmpty())
  {
    if (announce)
      Toast(tr("Choisis d'abord un dossier partagé dans Réglages › Mode manette"));
    return;
  }
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;
  const int n = BigPictureSync::SyncNow();
  if (n < 0)
    Toast(tr("Dossier de synchronisation introuvable"));
  else if (n > 0)
    Toast(tr("%1 sauvegarde(s) synchronisée(s)").arg(n));
  else if (announce)
    Toast(tr("Sauvegardes déjà à jour"));
}

// ================================================================== sleep

void BigPictureWindow::OnSystemSuspend()
{
  if (!m_active || m_slept_in_game ||
      !Settings::GetQSettings().value(QStringLiteral("bigpicture/sleep_save"), true).toBool())
    return;
  auto& system = Core::System::GetInstance();
  if (Core::IsUninitialized(system))
    return;
  m_slept_in_game = true;
  // Pause right away (no sound during sleep), and keep a resume point in case of power loss.
  Core::SetState(system, Core::State::Paused);
  if (m_running_entry.game)
    SaveResume();
}

void BigPictureWindow::OnSystemResume()
{
  if (!m_slept_in_game)
    return;
  m_slept_in_game = false;
  if (m_mode == Mode::InGame)
    QTimer::singleShot(800, this, [this] {
      if (m_mode == Mode::InGame)
        OpenPause();
    });
}

void BigPictureWindow::TakeSleepInhibitor()
{
#ifdef HAVE_BIGPICTURE_DBUS
  if (m_sleep_fd >= 0)
    return;
  // A "delay" lock gives Dolphin a moment to pause and save before the system sleeps.
  QDBusInterface login(QStringLiteral("org.freedesktop.login1"),
                       QStringLiteral("/org/freedesktop/login1"),
                       QStringLiteral("org.freedesktop.login1.Manager"),
                       QDBusConnection::systemBus());
  const QDBusReply<QDBusUnixFileDescriptor> reply =
      login.call(QStringLiteral("Inhibit"), QStringLiteral("sleep"), QStringLiteral("Dolphin"),
                 QStringLiteral("Sauvegarde du jeu avant la veille"), QStringLiteral("delay"));
  if (reply.isValid() && reply.value().isValid())
    m_sleep_fd = ::dup(reply.value().fileDescriptor());
#endif
}

void BigPictureWindow::OnPrepareForSleep(bool start)
{
  if (start)
  {
    OnSystemSuspend();
#ifdef HAVE_BIGPICTURE_DBUS
    if (m_sleep_fd >= 0)
    {
      ::close(m_sleep_fd);  // done: the system can sleep now
      m_sleep_fd = -1;
    }
#endif
  }
  else
  {
    TakeSleepInhibitor();
    OnSystemResume();
  }
}
