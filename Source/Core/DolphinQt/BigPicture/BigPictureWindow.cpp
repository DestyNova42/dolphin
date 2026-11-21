// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <numbers>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSettings>
#include <QTime>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HotkeyManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/TimePlayed.h"
#include "DiscIO/Enums.h"
#include "DolphinQt/BigPicture/BigPictureAudio.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/QtUtils/ImageConverter.h"
#include "DolphinQt/Settings.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/GameFile.h"

using Sound = BigPictureAudio::Sound;
namespace Prefs = BigPicturePrefs;

namespace
{
// Theme (validated mock-up "Mode manette")
const QColor BG(13, 11, 26);
const QColor SURFACE(23, 20, 43);
const QColor SURFACE_DEEP(18, 15, 36);
const QColor RAISED(34, 29, 61);
const QColor LINE(46, 40, 80);
const QColor TEXT(244, 241, 255);
const QColor TEXT_SOFT(185, 178, 214);
const QColor TEXT_DIM(141, 133, 179);
const QColor ACCENT(139, 108, 255);
const QColor ACCENT_SOFT(201, 185, 255);
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
  PAUSE_SETTINGS,
  PAUSE_DISC,
  PAUSE_SCREENSHOT,
  PAUSE_QUIT,
};
}  // namespace

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
  host.interface_changed = [this] { ApplyPreferences(); };
  m_settings = BuildBigPictureSettings(host);

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

BigPictureWindow::~BigPictureWindow() = default;

void BigPictureWindow::ApplyPreferences()
{
  m_view = Prefs::GetView();
  m_effect = Prefs::GetEffect();
  m_transition = Prefs::GetTransition();
  m_audio->SetVolumes(Prefs::MusicEnabled() ? Prefs::MusicVolume() / 100.0f : 0.0f,
                      Prefs::SfxVolume() / 100.0f);
  update();
}

void BigPictureWindow::Open()
{
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
  case Core::State::Running:
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
    if (m_mode == Mode::InGame || m_mode == Mode::Pause)
    {
      m_pause_settings = false;
      QTimer::singleShot(0, this, [this] {
        if (m_active)
        {
          Open();
          BeginTransition(0);
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
  if (m_mode == Mode::Setup || m_mode == Mode::InGame)
    return;

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
  home |= (status.button & PAD_BUTTON_START) && (status.button & PAD_TRIGGER_Z);

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
    if (list)
      break;
    SetSelected(m_selected + 1);
    break;
  case Action::Left:
    if (list)
      break;
    SetSelected(m_selected - 1);
    break;
  case Action::Down:
    SetSelected(m_selected + 1);
    break;
  case Action::Up:
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
    ApplyTab();
    break;
  }
  case Action::Accept:
    if (const Entry* sel = Selected())
    {
      m_audio->Play(Sound::Launch);
      Settings::GetQSettings().setValue(LastPlayedKey(sel->path),
                                        QDateTime::currentSecsSinceEpoch());
      m_running_entry = *sel;
      m_disc = 0;
      const QString path = sel->path;
      // Let the select sound play before the game takes over.
      QTimer::singleShot(380, this, [this, path] { emit LaunchRequested(path); });
    }
    else
    {
      m_audio->Play(Sound::Error);
    }
    break;
  case Action::Favorite:
    ToggleFavorite();
    break;
  case Action::Options:
    m_audio->Play(Sound::Page);
    BeginTransition(1);
    m_tab = Tab::Settings;
    m_settings_cat = 1;  // General
    m_settings_in_items = false;
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
  const int cats = static_cast<int>(m_settings.size());
  auto& items = m_settings[m_settings_cat].items;
  const int count = static_cast<int>(items.size());

  // L / R still switch tabs in the library.
  if ((action == Action::PageUp || action == Action::PageDown) && !m_pause_settings)
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
      if (m_pause_settings)
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
    break;
  default:
    break;
  }
}

void BigPictureWindow::HandleMenu(Action action)
{
  constexpr int ITEMS = 5;
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
      StartSetup(0);
      break;
    case 3:
      emit ClassicRequested();
      break;
    case 4:
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
  std::vector<int> items = {PAUSE_RESUME, PAUSE_SAVE, PAUSE_LOAD, PAUSE_SETTINGS};
  if (m_running_entry.discs.size() > 1)
    items.push_back(PAUSE_DISC);
  items.push_back(PAUSE_SCREENSHOT);
  items.push_back(PAUSE_QUIT);
  return items;
}

void BigPictureWindow::OpenPause()
{
  auto& system = Core::System::GetInstance();
  Core::SetState(system, Core::State::Paused);
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
      m_mode = Mode::InGame;
      m_poll_timer->stop();
      hide();
      emit StopRequested();
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
    m_intro_t += dt / 3.0;
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
                                       const QString& label, bool primary, qreal alpha)
{
  const qreal k = height() / 1080.0;
  const QFont font = Font(false, int((primary ? 24 : 22) * k), primary ? 600 : 500);
  const QFontMetricsF fm(font);
  const qreal icon_size = (primary ? 40 : 36) * k;
  const qreal h = 68 * k;
  const qreal w = 16 * k + icon_size + 14 * k + fm.horizontalAdvance(label) + 32 * k;
  const QRectF r(x, y, w, h);
  painter.save();
  painter.setOpacity(painter.opacity() * alpha);
  painter.setPen(Qt::NoPen);
  painter.setBrush(primary ? TEXT : RAISED);
  painter.drawRoundedRect(r, 16 * k, 16 * k);
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
  if (m_mode == Mode::Pause)
  {
    DrawPause(painter);
    DrawToast(painter);
    return;
  }

  painter.fillRect(rect(), BG);
  if (m_mode == Mode::Setup)
  {
    DrawBackground(painter);
    DrawSetup(painter);
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
  }
}

void BigPictureWindow::DrawIntro(QPainter& painter)
{
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
  painter.setBrush(TEXT);
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

  // Controller chip
  const QString name = ControllerName();
  const QString chip_label = name.isEmpty() ? tr("Manette non configurée") : name;
  const QFont chip_font = Font(false, int(20 * k), 500);
  const qreal chip_w =
      44 * k + QFontMetricsF(chip_font).horizontalAdvance(chip_label) + 20 * k;
  const QRectF chip(clock_x - 28 * k - chip_w, cy - 24 * k, chip_w, 48 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(chip, 24 * k, 24 * k);
  QColor dot = name.isEmpty() ? WARN : OK_GREEN;
  if (name.isEmpty())
    dot.setAlphaF(static_cast<float>(0.55 + 0.45 * std::sin(m_time * 4)));
  painter.setBrush(dot);
  painter.drawEllipse(QPointF(chip.left() + 26 * k, cy), 6 * k, 6 * k);
  painter.setFont(chip_font);
  painter.setPen(TEXT);
  painter.drawText(chip.adjusted(44 * k, 0, 0, 0), Qt::AlignVCenter, chip_label);
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
  bx += DrawPillButton(painter, bx, y, QStringLiteral("A"), tr("Jouer"), true) + 16 * k;
  bx += DrawPillButton(painter, bx, y, QStringLiteral("X"), tr("Réglages"), false) + 16 * k;
  DrawPillButton(painter, bx, y, QStringLiteral("Y"),
                 m_favorites.contains(sel.path) ? tr("Retirer des favoris") : tr("Favori"),
                 false);
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

  DrawHero(painter, *sel, left, 216 * k, 1150 * k);

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
    DrawHints(painter, {{QStringLiteral("Control_Stick"), tr("Naviguer")}},
              {{QStringLiteral("A"), tr("Jouer")},
               {QStringLiteral("Y"), tr("Favori")},
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
              {{QStringLiteral("A"), tr("Jouer")},
               {QStringLiteral("Y"), tr("Favori")},
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
  const int ncat = static_cast<int>(m_settings.size());
  m_settings_cat = std::clamp(m_settings_cat, 0, ncat - 1);

  if (m_pause_settings)
  {
    QFont title = Font(true, int(48 * k), 700);
    painter.setFont(title);
    painter.setPen(TEXT);
    painter.drawText(QRectF(left, top - 110 * k, 800 * k, 70 * k), Qt::AlignVCenter,
                     tr("Réglages"));
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
    painter.drawText(r.adjusted(24 * k, 0, 0, 0), Qt::AlignVCenter, m_settings[i].name);
  }

  // Items
  const auto& items = m_settings[m_settings_cat].items;
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

  if (m_mode == Mode::Library || m_mode == Mode::Pause)
  {
    if (m_settings_in_items)
    {
      DrawHints(painter,
                {{QStringLiteral("D-Pad-U"), QString()}, {QStringLiteral("D-Pad-D"), tr("Choisir")}},
                {{QStringLiteral("D-Pad-L"), QString()},
                 {QStringLiteral("D-Pad-R"), tr("Modifier")},
                 {QStringLiteral("A"), tr("Valider")},
                 {QStringLiteral("B"), tr("Catégories")}});
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
  painter.fillRect(panel, QColor(23, 20, 43, 245));

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
                           tr("Configurer la manette"), tr("Interface classique"),
                           tr("Quitter Dolphin")};
  qreal y = 230 * k;
  for (int i = 0; i < 5; ++i)
  {
    if (i == 4)
    {
      painter.fillRect(QRectF(64 * k, y + 4 * k, 572 * k, 1 * k), LINE);
      y += 20 * k;
    }
    // items slide in one after the other
    const qreal ie = EaseOut((m_overlay_t - i * 0.06) / 0.7);
    const QRectF r(64 * k - (1 - ie) * 40 * k, y, 572 * k, 80 * k);
    const bool active = i == m_menu_index;
    painter.setOpacity(e * ie);
    if (active)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(TEXT);
      painter.drawRoundedRect(r, 16 * k, 16 * k);
      DrawIcon(painter, QStringLiteral("A"),
               QRectF(r.right() - 28 * k - 38 * k, r.center().y() - 19 * k, 38 * k, 38 * k));
    }
    painter.setFont(Font(false, int(26 * k), active ? 600 : 500));
    painter.setPen(active ? BG : (i == 4 ? DANGER : TEXT));
    painter.drawText(r.adjusted(28 * k, 0, 0, 0), Qt::AlignVCenter, items[i]);
    y += 88 * k;
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
      text = tr("Réglages");
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
  const qreal ch = 128 * k;
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
    painter.setFont(Font(true, int(48 * k), 700));
    painter.setPen(current ? TEXT : TEXT_DIM);
    painter.drawText(QRectF(r.left() + 24 * k, r.top(), 70 * k, r.height()), Qt::AlignVCenter,
                     QString::number(s));
    painter.setFont(Font(false, int(22 * k), 600));
    painter.setPen(TEXT);
    painter.drawText(QRectF(r.left() + 100 * k, r.top() + 26 * k, r.width() - 120 * k, 34 * k),
                     Qt::AlignVCenter, tr("Emplacement %1").arg(s));
    painter.setFont(Font(false, int(19 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(r.left() + 100 * k, r.top() + 64 * k, r.width() - 120 * k, 34 * k),
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
  // Short pause so the same press isn't detected twice.
  QTimer::singleShot(250, this, [this] {
    if (m_mode == Mode::Setup)
      StartSetupStep();
  });
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
      Pad::GetConfig()->SaveConfig();
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
