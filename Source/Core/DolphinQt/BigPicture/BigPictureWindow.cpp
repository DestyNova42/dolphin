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
#include <QSvgRenderer>
#include <QTime>
#include <QTimer>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HotkeyManager.h"
#include "Core/System.h"
#include "Core/TimePlayed.h"
#include "DiscIO/Enums.h"
#include "DolphinQt/BigPicture/BigPictureAudio.h"
#include "DolphinQt/GameList/GameListModel.h"
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
const QColor ACCENT_SOFT(201, 185, 255);
const QColor OK_GREEN(91, 214, 154);
const QColor WARN(255, 180, 71);
const QColor DANGER(255, 180, 163);

constexpr int POLL_INTERVAL_MS = 16;
constexpr qint64 REPEAT_DELAY_MS = 350;
constexpr qint64 REPEAT_RATE_MS = 85;
constexpr int STICK_THRESHOLD = 60;
constexpr int ACTION_COUNT = 11;

constexpr u32 Bit(int action)
{
  return 1u << action;
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

ControllerEmu::EmulatedController* Port1()
{
  return Pad::GetConfig()->GetController(0);
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
  return QStringLiteral("bigpicture/lastplayed/") +
         QString::number(qHash(path), 16);
}

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
}  // namespace

BigPictureWindow::BigPictureWindow(const GameListModel& model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
  setWindowTitle(tr("Dolphin - Mode manette"));
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setCursor(Qt::BlankCursor);

  const QString res_dir =
      QString::fromStdString(File::GetSysDirectory() + "Resources/BigPicture/");

  // Button icons: every SVG of the folder, by file name (used as provided, unmodified).
  const QDir icon_dir(res_dir);
  for (const QString& file : icon_dir.entryList({QStringLiteral("*.svg")}, QDir::Files))
  {
    auto* renderer = new QSvgRenderer(res_dir + file, this);
    if (renderer->isValid())
      m_icons.insert(QFileInfo(file).completeBaseName(), renderer);
    else
      delete renderer;
  }

  // Fonts
  const auto load_font = [&](const QString& file) {
    const int id = QFontDatabase::addApplicationFont(res_dir + QStringLiteral("Fonts/") + file);
    const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) :
                                           QStringList{};
    return families.isEmpty() ? QStringLiteral("Segoe UI") : families.front();
  };
  m_display_family = load_font(QStringLiteral("SpaceGrotesk.ttf"));
  m_text_family = load_font(QStringLiteral("Inter.ttf"));

  const QStringList favorites =
      Settings::GetQSettings().value(QStringLiteral("bigpicture/favorites")).toStringList();
  m_favorites = QSet<QString>(favorites.begin(), favorites.end());

  m_audio = std::make_unique<BigPictureAudio>();

  m_clock.start();

  m_poll_timer = new QTimer(this);
  m_poll_timer->setInterval(POLL_INTERVAL_MS);
  connect(m_poll_timer, &QTimer::timeout, this, &BigPictureWindow::PollController);

  m_anim_timer = new QTimer(this);
  m_anim_timer->setInterval(16);
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
      StartSetup();
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
    m_ambient_cache.clear();
    ScheduleRebuild();
  });

  RebuildList();
}

BigPictureWindow::~BigPictureWindow() = default;

void BigPictureWindow::Open()
{
  static bool s_cleaned_up = false;
  if (!s_cleaned_up)
  {
    s_cleaned_up = true;
    CleanupLegacyFrontendConfig();
  }
  m_active = true;
  m_mode = Mode::Library;
  // Ignore buttons that are still held (e.g. the one used to quit the game).
  m_prev_held = ~0u;
  showFullScreen();
  raise();
  activateWindow();
  setFocus();
  m_poll_timer->start();
  m_anim_timer->start();
  m_setup_check_timer->start();

  if (!m_played_startup)
  {
    m_played_startup = true;
    // Give the decoder a moment, then play the GameCube startup jingle before the music.
    QTimer::singleShot(400, this, [this] { m_audio->Play(Sound::Startup); });
    QTimer::singleShot(2600, this, [this] {
      if (m_active && isVisible())
        m_audio->StartMusic();
    });
  }
  else
  {
    m_audio->StartMusic();
  }
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
  Suspend();
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
      m_all[it->second].disc_count++;
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
  QSettings& settings = Settings::GetQSettings();
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
  m_scroll = m_scroll_target = 0;
  update();
}

const BigPictureWindow::Entry* BigPictureWindow::Selected() const
{
  if (m_selected < 0 || m_selected >= static_cast<int>(m_entries.size()))
    return nullptr;
  return &m_entries[m_selected];
}

void BigPictureWindow::PollController()
{
  if (!m_active || !isVisible() || !isActiveWindow())
    return;
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;
  if (m_mode == Mode::Setup)
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
  for (int a = 0; a < ACTION_COUNT; ++a)
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

void BigPictureWindow::keyPressEvent(QKeyEvent* event)
{
  if (m_mode == Mode::Setup)
  {
    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat())
      FinishSetup(false);
    return;
  }
  const bool repeat = event->isAutoRepeat();
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
    if (!repeat)
      Press(Action::Accept);
    break;
  case Qt::Key_Escape:
  case Qt::Key_Backspace:
    if (!repeat)
      Press(Action::Back);
    break;
  case Qt::Key_Tab:
  case Qt::Key_F1:
    if (!repeat)
      Press(Action::Menu);
    break;
  case Qt::Key_PageUp:
  case Qt::Key_Q:
    if (!repeat)
      Press(Action::PageUp);
    break;
  case Qt::Key_PageDown:
  case Qt::Key_E:
    if (!repeat)
      Press(Action::PageDown);
    break;
  case Qt::Key_F:
    if (!repeat)
      Press(Action::Favorite);
    break;
  default:
    QWidget::keyPressEvent(event);
  }
}

void BigPictureWindow::Press(Action action)
{
  HandleAction(action);
  update();
}

void BigPictureWindow::ToggleFavorite()
{
  const Entry* sel = Selected();
  if (!sel)
    return;
  if (m_favorites.contains(sel->path))
    m_favorites.remove(sel->path);
  else
    m_favorites.insert(sel->path);
  Settings::GetQSettings().setValue(QStringLiteral("bigpicture/favorites"),
                                    QStringList(m_favorites.begin(), m_favorites.end()));
  m_audio->Play(Sound::Toggle);
  if (m_tab == Tab::Favorites)
    ApplyTab();
}

void BigPictureWindow::HandleAction(Action action)
{
  const int count = static_cast<int>(m_entries.size());

  switch (m_mode)
  {
  case Mode::Library:
    switch (action)
    {
    case Action::Right:
    case Action::Down:
      if (m_selected < count - 1)
      {
        m_selected++;
        m_audio->Play(Sound::Move);
      }
      break;
    case Action::Left:
    case Action::Up:
      if (m_selected > 0)
      {
        m_selected--;
        m_audio->Play(Sound::Move);
      }
      break;
    case Action::PageDown:
      m_tab = static_cast<Tab>((static_cast<int>(m_tab) + 1) % 4);
      m_audio->Play(Sound::Page);
      ApplyTab();
      break;
    case Action::PageUp:
      m_tab = static_cast<Tab>((static_cast<int>(m_tab) + 3) % 4);
      m_audio->Play(Sound::Page);
      ApplyTab();
      break;
    case Action::Accept:
      if (m_tab == Tab::Settings)
      {
        m_audio->Play(Sound::Confirm);
        emit ClassicRequested();
      }
      else if (const Entry* sel = Selected())
      {
        m_audio->Play(Sound::Launch);
        Settings::GetQSettings().setValue(LastPlayedKey(sel->path),
                                          QDateTime::currentSecsSinceEpoch());
        const QString path = sel->path;
        // Let the select sound play before the game takes over.
        QTimer::singleShot(350, this, [this, path] { emit LaunchRequested(path); });
      }
      else
      {
        m_audio->Play(Sound::Error);
      }
      break;
    case Action::Favorite:
      if (m_tab != Tab::Settings)
        ToggleFavorite();
      break;
    case Action::Options:
      m_audio->Play(Sound::Error);
      break;
    case Action::Back:
      m_audio->Play(Sound::Back);
      m_mode = Mode::ConfirmQuit;
      break;
    case Action::Menu:
      m_audio->Play(Sound::MenuOpen);
      m_mode = Mode::Menu;
      m_menu_index = 0;
      break;
    }
    break;

  case Mode::Menu:
  {
    constexpr int ITEMS = 4;
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
      m_mode = Mode::Library;
      break;
    case Action::Accept:
      m_audio->Play(Sound::Confirm);
      m_mode = Mode::Library;
      if (m_menu_index == 1)
        StartSetup();
      else if (m_menu_index == 2)
        emit ClassicRequested();
      else if (m_menu_index == 3)
        m_mode = Mode::ConfirmQuit;
      break;
    default:
      break;
    }
    break;
  }

  case Mode::Setup:
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
      m_mode = Mode::Library;
    }
    break;
  }
}

void BigPictureWindow::Tick()
{
  bool changed = false;
  if (std::abs(m_scroll_target - m_scroll) > 0.5)
  {
    m_scroll += (m_scroll_target - m_scroll) * 0.22;
    changed = true;
  }
  // Ambient glow follows the selected game's cover colour.
  QColor target = BG;
  if (const Entry* sel = Selected(); sel && m_tab != Tab::Settings)
    target = AmbientFor(*sel);
  if (!m_ambient.isValid())
    m_ambient = target;
  const auto mix = [](int a, int b) { return a + (b - a) / 8 + (b > a ? 1 : (b < a ? -1 : 0)); };
  if (m_ambient != target)
  {
    m_ambient = QColor(mix(m_ambient.red(), target.red()), mix(m_ambient.green(), target.green()),
                       mix(m_ambient.blue(), target.blue()));
    changed = true;
  }
  static int s_minute = -1;
  const int minute = QTime::currentTime().minute();
  if (minute != s_minute)
  {
    s_minute = minute;
    changed = true;
  }
  if (changed || m_mode == Mode::Setup)
    update();
}

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
  m_cover_cache.insert(key, pixmap);
  return pixmap;
}

QColor BigPictureWindow::AmbientFor(const Entry& entry)
{
  const auto it = m_ambient_cache.constFind(entry.path);
  if (it != m_ambient_cache.constEnd())
    return *it;

  QColor color(47, 123, 216);  // default: GameCube indigo-blue
  const auto& cover = entry.game->GetCoverImage();
  QImage image;
  if (!cover.empty() &&
      image.loadFromData(cover.buffer.data(), static_cast<int>(cover.buffer.size())))
  {
    const QImage px = image.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    color = px.pixelColor(0, 0);
    // Keep the glow colourful but never too bright.
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
  s_time_played.Reload();
  const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
                           s_time_played.GetTimePlayed(entry.game->GetGameID()))
                           .count();
  if (minutes >= 60)
    parts << tr("%1 h %2 de jeu").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
  else if (minutes > 0)
    parts << tr("%1 min de jeu").arg(minutes);
  if (entry.disc_count > 1)
    parts << tr("%1 disques").arg(entry.disc_count);
  return parts.join(QStringLiteral("  ·  "));
}

QString BigPictureWindow::ControllerName() const
{
  auto* pad = Port1();
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

// ------------------------------------------------------------------ drawing

QFont BigPictureWindow::Font(bool display, int px, int weight) const
{
  QFont font(display ? m_display_family : m_text_family);
  font.setPixelSize(std::max(1, px));
  font.setWeight(static_cast<QFont::Weight>(weight));
  return font;
}

void BigPictureWindow::DrawIcon(QPainter& painter, const QString& name, const QRectF& rect)
{
  const auto it = m_icons.constFind(name);
  if (it != m_icons.constEnd())
    (*it)->render(&painter, rect);
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
                                       const QString& label, bool primary)
{
  const qreal k = height() / 1080.0;
  const QFont font = Font(false, int((primary ? 24 : 22) * k), primary ? 600 : 500);
  const QFontMetricsF fm(font);
  const qreal icon_size = (primary ? 40 : 36) * k;
  const qreal h = 68 * k;
  const qreal w = 16 * k + icon_size + 14 * k + fm.horizontalAdvance(label) + 32 * k;
  const QRectF r(x, y, w, h);
  painter.setPen(Qt::NoPen);
  painter.setBrush(primary ? TEXT : RAISED);
  painter.drawRoundedRect(r, 16 * k, 16 * k);
  DrawIcon(painter, icon, QRectF(x + 16 * k, y + (h - icon_size) / 2, icon_size, icon_size));
  painter.setFont(font);
  painter.setPen(primary ? BG : TEXT);
  painter.drawText(QRectF(x + 16 * k + icon_size + 14 * k, y, w, h), Qt::AlignVCenter, label);
  return w;
}

void BigPictureWindow::DrawTopBar(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const qreal cy = 84 * k;

  // Logo: cube outline + name
  painter.save();
  painter.translate(80 * k, cy - 20 * k);
  painter.scale(k, k);
  QPen pen(TEXT, 2.5);
  pen.setJoinStyle(Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  QPainterPath cube;
  cube.moveTo(20, 3);
  cube.lineTo(35, 11.5);
  cube.lineTo(35, 28.5);
  cube.lineTo(20, 37);
  cube.lineTo(5, 28.5);
  cube.lineTo(5, 11.5);
  cube.closeSubpath();
  cube.moveTo(20, 20);
  cube.lineTo(35, 11.5);
  cube.moveTo(20, 20);
  cube.lineTo(5, 11.5);
  cube.moveTo(20, 20);
  cube.lineTo(20, 37);
  painter.drawPath(cube);
  painter.restore();
  painter.setFont(Font(true, int(30 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(136 * k, cy - 30 * k, 400 * k, 60 * k), Qt::AlignVCenter,
                   QStringLiteral("Dolphin"));

  // Tabs
  const QString tabs[] = {tr("Bibliothèque"), tr("Récents"), tr("Favoris"), tr("Réglages")};
  const QFont tab_font = Font(false, int(20 * k), 600);
  const QFontMetricsF fm(tab_font);
  qreal pill_w = 12 * k;
  for (const QString& t : tabs)
    pill_w += fm.horizontalAdvance(t) + 56 * k + 6 * k;
  pill_w -= 6 * k;
  const qreal pill_h = 58 * k;
  const qreal pill_x = (width() - pill_w) / 2;
  const QRectF pill(pill_x, cy - pill_h / 2, pill_w, pill_h);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(pill, pill_h / 2, pill_h / 2);
  qreal tx = pill_x + 6 * k;
  for (int i = 0; i < 4; ++i)
  {
    const qreal tw = fm.horizontalAdvance(tabs[i]) + 56 * k;
    const QRectF r(tx, cy - pill_h / 2 + 6 * k, tw, pill_h - 12 * k);
    const bool active = static_cast<int>(m_tab) == i;
    if (active)
    {
      painter.setBrush(TEXT);
      painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    }
    painter.setFont(Font(false, int(20 * k), active ? 600 : 500));
    painter.setPen(active ? BG : TEXT_SOFT);
    painter.drawText(r, Qt::AlignCenter, tabs[i]);
    tx += tw + 6 * k;
  }
  const qreal icon = 40 * k;
  DrawIcon(painter, QStringLiteral("L"), QRectF(pill.left() - 14 * k - icon, cy - icon / 2, icon, icon));
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
  const qreal chip_w = 20 * k + 12 * k + 12 * k + QFontMetricsF(chip_font).horizontalAdvance(chip_label) + 20 * k;
  const QRectF chip(clock_x - 28 * k - chip_w, cy - 24 * k, chip_w, 48 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(chip, 24 * k, 24 * k);
  painter.setBrush(name.isEmpty() ? WARN : OK_GREEN);
  painter.drawEllipse(QPointF(chip.left() + 26 * k, cy), 6 * k, 6 * k);
  painter.setFont(chip_font);
  painter.setPen(TEXT);
  painter.drawText(chip.adjusted(44 * k, 0, 0, 0), Qt::AlignVCenter, chip_label);
}

void BigPictureWindow::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.setRenderHint(QPainter::TextAntialiasing);
  painter.fillRect(rect(), BG);

  if (m_mode == Mode::Setup)
  {
    DrawSetup(painter);
    return;
  }

  // Ambient glow (selected game's colour)
  if (m_ambient.isValid() && m_tab != Tab::Settings)
  {
    const qreal k = height() / 1080.0;
    const QPointF c(width() - 430 * k, 290 * k);
    QRadialGradient g(c, 760 * k);
    QColor inner = m_ambient;
    inner.setAlphaF(0.26);
    QColor outer = m_ambient;
    outer.setAlphaF(0.0);
    g.setColorAt(0, inner);
    g.setColorAt(0.55, QColor(m_ambient.red(), m_ambient.green(), m_ambient.blue(), 25));
    g.setColorAt(1, outer);
    painter.fillRect(rect(), g);
  }

  DrawTopBar(painter);
  if (m_tab == Tab::Settings)
    DrawSettingsTab(painter);
  else
    DrawLibrary(painter);

  if (m_mode == Mode::Menu)
    DrawMenu(painter);
  else if (m_mode == Mode::ConfirmQuit)
    DrawDialog(painter, tr("Quitter Dolphin ?"),
               {{QStringLiteral("A"), tr("Quitter")}, {QStringLiteral("B"), tr("Annuler")}});
}

void BigPictureWindow::DrawLibrary(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const Entry* sel = Selected();

  if (!sel)
  {
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
      help = tr("Ajoute un dossier de jeux depuis l'interface classique (Start > Interface classique).");
    }
    painter.drawText(QRectF(0, height() / 2.0 - 80 * k, width(), 70 * k), Qt::AlignCenter, title);
    painter.setFont(Font(false, int(24 * k), 400));
    painter.setPen(TEXT_SOFT);
    painter.drawText(QRectF(0, height() / 2.0, width(), 50 * k), Qt::AlignCenter, help);
    DrawHints(painter, {}, {{QStringLiteral("Start-Pause"), tr("Menu")}});
    return;
  }

  // ---- Hero
  const qreal left = 80 * k;
  qreal y = 216 * k;
  QStringList meta;
  const auto platform = sel->game->GetPlatform();
  meta << ((platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD) ?
               QStringLiteral("WII") :
               QStringLiteral("GAMECUBE"));
  const QString region = RegionName(sel->game->GetRegion()).toUpper();
  if (!region.isEmpty())
    meta << region;
  if (!sel->game->GetGameID().empty())
    meta << QString::fromStdString(sel->game->GetGameID());
  QFont meta_font = Font(false, int(18 * k), 600);
  meta_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(meta_font);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(left, y, 1200 * k, 28 * k), Qt::AlignVCenter,
                   meta.join(QStringLiteral("   ·   ")));
  y += 40 * k;

  // Title: up to two lines, shrinks if needed.
  const QRectF title_box(left, y, 1150 * k, 170 * k);
  int title_px = int(76 * k);
  QFont title_font = Font(true, title_px, 700);
  title_font.setLetterSpacing(QFont::AbsoluteSpacing, -2 * k);
  QRectF needed;
  for (;;)
  {
    needed = QFontMetricsF(title_font)
                 .boundingRect(title_box, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                               sel->title);
    if (needed.height() <= title_box.height() || title_px <= int(44 * k))
      break;
    title_px -= int(4 * k);
    title_font.setPixelSize(title_px);
  }
  painter.setFont(title_font);
  painter.setPen(TEXT);
  painter.drawText(title_box, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, sel->title);
  y += std::min(needed.height(), title_box.height()) + 18 * k;

  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(left, y, 1400 * k, 32 * k), Qt::AlignVCenter, PlayInfo(*sel));
  y += 52 * k;

  qreal bx = left;
  bx += DrawPillButton(painter, bx, y, QStringLiteral("A"), tr("Jouer"), true) + 16 * k;
  bx += DrawPillButton(painter, bx, y, QStringLiteral("X"), tr("Réglages du jeu"), false) + 16 * k;
  DrawPillButton(painter, bx, y, QStringLiteral("Y"),
                 m_favorites.contains(sel->path) ? tr("Retirer des favoris") : tr("Favori"),
                 false);

  // ---- Shelf
  const qreal shelf_label_y = 576 * k;
  const QString tab_title = m_tab == Tab::Recent    ? tr("Récemment joués") :
                            m_tab == Tab::Favorites ? tr("Favoris") :
                                                      tr("Tous les jeux");
  const QFont label_font = Font(true, int(28 * k), 600);
  painter.setFont(label_font);
  painter.setPen(TEXT);
  painter.drawText(QRectF(left, shelf_label_y, 800 * k, 40 * k), Qt::AlignVCenter, tab_title);
  const qreal lw = QFontMetricsF(label_font).horizontalAdvance(tab_title);
  painter.setFont(Font(false, int(20 * k), 400));
  painter.setPen(TEXT_DIM);
  painter.drawText(QRectF(left + lw + 14 * k, shelf_label_y + 3 * k, 800 * k, 40 * k),
                   Qt::AlignVCenter,
                   tr("%1 / %2").arg(m_selected + 1).arg(m_entries.size()));

  const qreal cw = 220 * k, ch = 308 * k;
  const qreal sw = 250 * k, sh = 350 * k;
  const qreal gap = 28 * k;
  const qreal step = cw + gap;
  const qreal bottom = 985 * k;
  m_scroll_target = std::max<qreal>(0, (m_selected - 2) * step);

  painter.save();
  painter.setClipRect(QRectF(0, 600 * k, width(), 400 * k));
  const int first = std::max(0, int(m_scroll / step) - 1);
  const int visible = int(width() / step) + 3;
  const int count = static_cast<int>(m_entries.size());
  for (int i = first; i < std::min(count, first + visible); ++i)
  {
    const Entry& e = m_entries[i];
    const bool selected = i == m_selected;
    qreal x = left + i * step - m_scroll;
    if (i > m_selected)
      x += sw - cw;  // the selected cover is wider
    const qreal w = selected ? sw : cw;
    const qreal h = selected ? sh : ch;
    const QRectF r(x, bottom - h, w, h);
    if (r.right() < 0 || r.left() > width())
      continue;

    QPainterPath clip;
    clip.addRoundedRect(r, 16 * k, 16 * k);
    painter.setOpacity(selected ? 1.0 : 0.82);
    const QPixmap cover = CoverFor(e, r.size().toSize());
    if (!cover.isNull())
    {
      painter.save();
      painter.setClipPath(clip, Qt::IntersectClip);
      painter.drawPixmap(r.topLeft(), cover);
      painter.restore();
    }
    else
    {
      painter.fillPath(clip, RAISED);
      painter.setFont(Font(true, int(25 * k), 700));
      painter.setPen(TEXT);
      painter.drawText(r.adjusted(22 * k, 22 * k, -22 * k, -22 * k),
                       Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, e.title);
    }
    if (m_favorites.contains(e.path))
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(13, 11, 26, 200));
      painter.drawEllipse(QPointF(r.right() - 26 * k, r.top() + 26 * k), 16 * k, 16 * k);
      painter.setBrush(WARN);
      QPainterPath star;
      const QPointF c(r.right() - 26 * k, r.top() + 26 * k);
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
      painter.drawPath(star);
    }
    painter.setOpacity(1.0);
    if (selected)
    {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(TEXT, 4 * k));
      painter.drawRoundedRect(r.adjusted(-9 * k, -9 * k, 9 * k, 9 * k), 22 * k, 22 * k);
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

void BigPictureWindow::DrawSettingsTab(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const QRectF card((width() - 1000 * k) / 2, 300 * k, 1000 * k, 420 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawRoundedRect(card, 28 * k, 28 * k);

  painter.setFont(Font(true, int(44 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(card.left(), card.top() + 70 * k, card.width(), 60 * k),
                   Qt::AlignCenter, tr("Réglages"));
  painter.setFont(Font(false, int(23 * k), 400));
  painter.setPen(TEXT_SOFT);
  painter.drawText(QRectF(card.left() + 80 * k, card.top() + 150 * k, card.width() - 160 * k,
                          110 * k),
                   Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Tous les réglages de Dolphin arrivent ici, à la manette, dans la "
                      "prochaine version. En attendant, ils sont dans l'interface classique."));
  const QString label = tr("Ouvrir l'interface classique");
  const QFontMetricsF fm(Font(false, int(24 * k), 600));
  const qreal bw = 16 * k + 40 * k + 14 * k + fm.horizontalAdvance(label) + 32 * k;
  DrawPillButton(painter, card.center().x() - bw / 2, card.bottom() - 120 * k,
                 QStringLiteral("A"), label, true);

  if (m_mode == Mode::Library)
  {
    DrawHints(painter, {{QStringLiteral("L"), tr("Onglet précédent")}},
              {{QStringLiteral("A"), tr("Ouvrir")}, {QStringLiteral("Start-Pause"), tr("Menu")}});
  }
}

void BigPictureWindow::DrawMenu(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  painter.fillRect(rect(), QColor(9, 7, 20, 184));

  const QRectF panel(0, 0, 700 * k, height());
  painter.fillRect(panel, QColor(23, 20, 43, 240));

  painter.setFont(Font(false, int(18 * k), 600));
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(64 * k, 80 * k, 600 * k, 30 * k), Qt::AlignVCenter,
                   tr("MENU"));
  painter.setFont(Font(true, int(48 * k), 700));
  painter.setPen(TEXT);
  painter.drawText(QRectF(64 * k, 116 * k, 600 * k, 64 * k), Qt::AlignVCenter,
                   QStringLiteral("Dolphin"));

  const QString items[] = {tr("Retour à la bibliothèque"), tr("Configurer la manette"),
                           tr("Interface classique"), tr("Quitter Dolphin")};
  qreal y = 230 * k;
  for (int i = 0; i < 4; ++i)
  {
    if (i == 3)
    {
      painter.fillRect(QRectF(64 * k, y + 4 * k, 572 * k, 1 * k), LINE);
      y += 20 * k;
    }
    const QRectF r(64 * k, y, 572 * k, 80 * k);
    const bool active = i == m_menu_index;
    if (active)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(TEXT);
      painter.drawRoundedRect(r, 16 * k, 16 * k);
      DrawIcon(painter, QStringLiteral("A"),
               QRectF(r.right() - 28 * k - 38 * k, r.center().y() - 19 * k, 38 * k, 38 * k));
    }
    painter.setFont(Font(false, int(26 * k), active ? 600 : 500));
    painter.setPen(active ? BG : (i == 3 ? DANGER : TEXT));
    painter.drawText(r.adjusted(28 * k, 0, 0, 0), Qt::AlignVCenter, items[i]);
    y += 88 * k;
  }

  // Hints inside the panel
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
}

void BigPictureWindow::DrawDialog(QPainter& painter, const QString& title,
                                  const std::vector<std::pair<QString, QString>>& hints)
{
  const qreal k = height() / 1080.0;
  painter.fillRect(rect(), QColor(9, 7, 20, 190));
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
  {
    total += QFontMetricsF(Font(false, int(22 * k), 500)).horizontalAdvance(label) + 120 * k;
  }
  qreal x = box.center().x() - (total - 16 * k) / 2;
  for (size_t i = 0; i < hints.size(); ++i)
    x += DrawPillButton(painter, x, box.bottom() - 110 * k, hints[i].first, hints[i].second,
                        i == 0) + 16 * k;
}

void BigPictureWindow::DrawSetup(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  const SetupStep& step = SETUP_STEPS[std::min(m_setup_step, SETUP_STEP_COUNT - 1)];

  QFont label_font = Font(false, int(18 * k), 600);
  label_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5 * k);
  painter.setFont(label_font);
  painter.setPen(ACCENT_SOFT);
  painter.drawText(QRectF(0, 110 * k, width(), 30 * k), Qt::AlignCenter,
                   tr("CONFIGURATION DE LA MANETTE"));

  painter.setFont(Font(false, int(22 * k), 400));
  painter.setPen(TEXT_SOFT);
  QString device_line;
  if (m_setup_device_chosen)
    device_line =
        tr("Manette détectée : %1").arg(QString::fromStdString(Port1()->GetDefaultDevice().name));
  else
    device_line = tr("Appuie sur A sur la manette que tu veux utiliser");
  painter.drawText(QRectF(0, 152 * k, width(), 34 * k), Qt::AlignCenter, device_line);

  // Button to press
  const QPointF c(width() / 2.0, 420 * k);
  painter.setPen(QPen(LINE, 2 * k));
  painter.setBrush(SURFACE_DEEP);
  painter.drawEllipse(c, 166 * k, 166 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(SURFACE);
  painter.drawEllipse(c, 150 * k, 150 * k);
  DrawIcon(painter, QString::fromLatin1(step.icon),
           QRectF(c.x() - 90 * k, c.y() - 90 * k, 180 * k, 180 * k));

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

// ------------------------------------------------------------------ controller setup

bool BigPictureWindow::NeedsSetup() const
{
  auto* pad = Port1();
  if (!pad)
    return false;
  const auto& dq = pad->GetDefaultDevice();
  if (dq.source == "DSUClient")
    return true;
  return g_controller_interface.FindDevice(dq) == nullptr;
}

void BigPictureWindow::StartSetup()
{
  auto* pad = Port1();
  if (!pad)
    return;
  m_mode = Mode::Setup;
  m_setup_step = 0;
  m_setup_device_chosen = false;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    for (PadGroup group :
         {PadGroup::Buttons, PadGroup::MainStick, PadGroup::CStick, PadGroup::Triggers,
          PadGroup::DPad})
    {
      for (auto& control : Pad::GetGroup(0, group)->controls)
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
    devices.push_back(Port1()->GetDefaultDevice().ToString());
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
    update();
    return;
  }

  auto* pad = Port1();
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
    auto* group = Pad::GetGroup(0, step.group);
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
  update();
}

void BigPictureWindow::FinishSetup(bool save)
{
  m_setup_timer->stop();
  m_detector.reset();
  auto* pad = Port1();
  if (save && pad)
  {
    pad->UpdateReferences(g_controller_interface);
    Pad::GetConfig()->SaveConfig();
  }
  else if (pad)
  {
    // Cancelled: reload what was saved before.
    Pad::LoadConfig();
  }
  m_mode = Mode::Library;
  m_prev_held = ~0u;
  update();
}

