// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureWindow.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QTimer>

#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "InputCommon/GCPadStatus.h"
#include "UICommon/GameFile.h"

namespace
{
// Provisional theme (the look will be refined later).
const QColor BG_TOP(22, 18, 40);
const QColor BG_BOTTOM(8, 7, 16);
const QColor ACCENT(138, 104, 255);
const QColor TEXT(240, 238, 248);
const QColor DIM(150, 145, 170);
const QColor CARD(38, 33, 62);

constexpr int POLL_INTERVAL_MS = 16;
constexpr qint64 REPEAT_DELAY_MS = 350;
constexpr qint64 REPEAT_RATE_MS = 85;
constexpr int STICK_THRESHOLD = 60;

constexpr u32 Bit(int action)
{
  return 1u << action;
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

  const QString icon_dir =
      QString::fromStdString(File::GetSysDirectory() + "Resources/BigPicture/");
  for (const QString& name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("X"),
                              QStringLiteral("Y"), QStringLiteral("Z"), QStringLiteral("L"),
                              QStringLiteral("R"), QStringLiteral("Start-Pause"),
                              QStringLiteral("D-Pad"), QStringLiteral("Control_Stick")})
  {
    auto* renderer = new QSvgRenderer(icon_dir + name + QStringLiteral(".svg"), this);
    if (renderer->isValid())
      m_icons.insert(name, renderer);
  }

  m_clock.start();

  m_poll_timer = new QTimer(this);
  m_poll_timer->setInterval(POLL_INTERVAL_MS);
  connect(m_poll_timer, &QTimer::timeout, this, &BigPictureWindow::PollController);

  m_anim_timer = new QTimer(this);
  m_anim_timer->setInterval(16);
  connect(m_anim_timer, &QTimer::timeout, this, &BigPictureWindow::Tick);

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
    ScheduleRebuild();
  });

  RebuildList();
}

BigPictureWindow::~BigPictureWindow() = default;

void BigPictureWindow::Open()
{
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
}

void BigPictureWindow::Suspend()
{
  m_poll_timer->stop();
  m_anim_timer->stop();
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
  QString selected_path;
  if (m_selected >= 0 && m_selected < static_cast<int>(m_entries.size()))
    selected_path = QString::fromStdString(m_entries[m_selected].game->GetFilePath());

  m_entries.clear();
  std::map<std::string, size_t> first_disc;  // game ID -> entry index

  const int title_column = static_cast<int>(GameListModel::Column::Title);
  const int rows = m_model.rowCount(QModelIndex());
  std::vector<int> later_discs;

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
    Entry entry;
    entry.game = game;
    entry.title = m_model.data(m_model.index(row, title_column), Qt::DisplayRole).toString();
    if (entry.title.isEmpty())
      entry.title = QString::fromStdString(game->GetFileName());
    if (!game->GetGameID().empty())
      first_disc.emplace(game->GetGameID(), m_entries.size());
    m_entries.push_back(std::move(entry));
  }

  // Multi-disc games: show a single entry, Dolphin finds the other disc when booting.
  for (int row : later_discs)
  {
    auto game = m_model.GetGameFile(row);
    const auto it = first_disc.find(game->GetGameID());
    if (it != first_disc.end())
    {
      m_entries[it->second].disc_count++;
      continue;
    }
    Entry entry;
    entry.game = game;
    entry.title = m_model.data(m_model.index(row, title_column), Qt::DisplayRole).toString();
    m_entries.push_back(std::move(entry));
  }

  std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
    return QString::localeAwareCompare(a.title, b.title) < 0;
  });

  m_selected = 0;
  for (size_t i = 0; i < m_entries.size(); ++i)
  {
    if (QString::fromStdString(m_entries[i].game->GetFilePath()) == selected_path)
      m_selected = static_cast<int>(i);
  }
  update();
}

void BigPictureWindow::PollController()
{
  if (!m_active || !isVisible() || !isActiveWindow())
    return;
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;

  const GCPadStatus pad = Pad::GetStatus(0);

  u32 held = m_key_held;
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
  if ((pad.button & (PAD_TRIGGER_R | PAD_TRIGGER_Z)) || pad.triggerRight > 200)
    held |= Bit(int(Action::PageDown));

  const qint64 now = m_clock.elapsed();
  for (int a = 0; a <= int(Action::PageDown); ++a)
  {
    const bool is_held = held & Bit(a);
    const bool was_held = m_prev_held & Bit(a);
    if (is_held && !was_held)
    {
      m_held_since[a] = m_last_repeat[a] = now;
      Press(static_cast<Action>(a));
    }
    else if (is_held && was_held)
    {
      const bool repeatable = a <= int(Action::Right) || a >= int(Action::PageUp);
      if (repeatable && now - m_held_since[a] > REPEAT_DELAY_MS &&
          now - m_last_repeat[a] > REPEAT_RATE_MS)
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
  if (event->isAutoRepeat())
    return;
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
    Press(Action::Accept);
    break;
  case Qt::Key_Escape:
  case Qt::Key_Backspace:
    Press(Action::Back);
    break;
  case Qt::Key_Tab:
  case Qt::Key_F1:
    Press(Action::Menu);
    break;
  case Qt::Key_PageUp:
    Press(Action::PageUp);
    break;
  case Qt::Key_PageDown:
    Press(Action::PageDown);
    break;
  default:
    QWidget::keyPressEvent(event);
  }
}

void BigPictureWindow::keyReleaseEvent(QKeyEvent* event)
{
  QWidget::keyReleaseEvent(event);
}

void BigPictureWindow::Press(Action action)
{
  HandleAction(action);
  update();
}

int BigPictureWindow::Columns() const
{
  return 6;
}

void BigPictureWindow::HandleAction(Action action)
{
  const int count = static_cast<int>(m_entries.size());
  const int cols = Columns();

  switch (m_mode)
  {
  case Mode::Library:
    switch (action)
    {
    case Action::Right:
      m_selected = std::min(count - 1, m_selected + 1);
      break;
    case Action::Left:
      m_selected = std::max(0, m_selected - 1);
      break;
    case Action::Down:
      m_selected = std::min(count - 1, m_selected + cols);
      break;
    case Action::Up:
      m_selected = std::max(0, m_selected - cols);
      break;
    case Action::PageDown:
      m_selected = std::min(count - 1, m_selected + cols * 3);
      break;
    case Action::PageUp:
      m_selected = std::max(0, m_selected - cols * 3);
      break;
    case Action::Accept:
      if (m_selected >= 0 && m_selected < count)
        emit LaunchRequested(QString::fromStdString(m_entries[m_selected].game->GetFilePath()));
      break;
    case Action::Back:
      m_mode = Mode::ConfirmQuit;
      break;
    case Action::Menu:
      m_mode = Mode::Menu;
      m_menu_index = 0;
      break;
    }
    m_selected = std::max(0, m_selected);
    break;

  case Mode::Menu:
  {
    constexpr int ITEMS = 3;
    switch (action)
    {
    case Action::Down:
      m_menu_index = (m_menu_index + 1) % ITEMS;
      break;
    case Action::Up:
      m_menu_index = (m_menu_index + ITEMS - 1) % ITEMS;
      break;
    case Action::Back:
    case Action::Menu:
      m_mode = Mode::Library;
      break;
    case Action::Accept:
      m_mode = Mode::Library;
      if (m_menu_index == 1)
        emit ClassicRequested();
      else if (m_menu_index == 2)
        emit QuitRequested();
      break;
    default:
      break;
    }
    break;
  }

  case Mode::ConfirmQuit:
    if (action == Action::Accept)
    {
      m_mode = Mode::Library;
      emit QuitRequested();
    }
    else if (action == Action::Back)
    {
      m_mode = Mode::Library;
    }
    break;
  }
}

void BigPictureWindow::Tick()
{
  if (std::abs(m_scroll_target - m_scroll) > 0.5)
  {
    m_scroll += (m_scroll_target - m_scroll) * 0.25;
    update();
  }
}

QPixmap BigPictureWindow::CoverFor(const Entry& entry, const QSize& size)
{
  const QString key = QString::fromStdString(entry.game->GetFilePath()) +
                      QStringLiteral("@%1x%2").arg(size.width()).arg(size.height());
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

void BigPictureWindow::DrawIcon(QPainter& painter, const QString& name, const QRectF& rect)
{
  const auto it = m_icons.constFind(name);
  if (it != m_icons.constEnd())
    (*it)->render(&painter, rect);
}

qreal BigPictureWindow::DrawHints(QPainter& painter,
                                  const std::vector<std::pair<QString, QString>>& hints)
{
  const qreal k = height() / 1080.0;
  QFont font = painter.font();
  font.setPixelSize(int(26 * k));
  painter.setFont(font);
  const QFontMetricsF fm(font);
  const qreal icon = 46 * k;
  qreal x = width() - 60 * k;
  const qreal y = height() - 55 * k;
  for (auto it = hints.rbegin(); it != hints.rend(); ++it)
  {
    const qreal tw = fm.horizontalAdvance(it->second);
    painter.setPen(TEXT);
    painter.drawText(QRectF(x - tw, y - icon / 2, tw, icon), Qt::AlignVCenter | Qt::AlignLeft,
                     it->second);
    x -= tw + 10 * k + icon;
    DrawIcon(painter, it->first, QRectF(x, y - icon / 2, icon, icon));
    x -= 44 * k;
  }
  return x;
}

void BigPictureWindow::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);

  QLinearGradient bg(0, 0, 0, height());
  bg.setColorAt(0, BG_TOP);
  bg.setColorAt(1, BG_BOTTOM);
  painter.fillRect(rect(), bg);

  DrawLibrary(painter);

  if (m_mode == Mode::Menu)
    DrawMenu(painter);
  else if (m_mode == Mode::ConfirmQuit)
    DrawDialog(painter, tr("Quitter Dolphin ?"),
               {{QStringLiteral("A"), tr("Oui")}, {QStringLiteral("B"), tr("Non")}});
}

void BigPictureWindow::DrawLibrary(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  QFont font = painter.font();

  font.setPixelSize(int(54 * k));
  font.setBold(true);
  painter.setFont(font);
  painter.setPen(TEXT);
  painter.drawText(QRectF(60 * k, 20 * k, width(), 70 * k), Qt::AlignVCenter | Qt::AlignLeft,
                   tr("Bibliothèque"));
  font.setBold(false);
  font.setPixelSize(int(26 * k));
  painter.setFont(font);
  painter.setPen(DIM);
  painter.drawText(QRectF(60 * k, 85 * k, width(), 34 * k), Qt::AlignVCenter | Qt::AlignLeft,
                   tr("%n jeu(x)", "", static_cast<int>(m_entries.size())));

  const int count = static_cast<int>(m_entries.size());
  if (count == 0)
  {
    font.setPixelSize(int(34 * k));
    painter.setFont(font);
    painter.drawText(rect(), Qt::AlignCenter,
                     tr("Aucun jeu. Ajoute un dossier de jeux depuis l'interface classique."));
    DrawHints(painter, {{QStringLiteral("Start-Pause"), tr("Menu")}});
    return;
  }

  const int cols = Columns();
  const qreal margin = 60 * k;
  const qreal gap = 28 * k;
  const qreal cw = (width() - 2 * margin - gap * (cols - 1)) / cols;
  const qreal ch = cw * 1.4;
  const qreal top = 150 * k;
  const qreal row_h = ch + gap;
  const qreal view_h = height() - top - 210 * k;
  const int sel_row = m_selected / cols;
  m_scroll_target = std::max<qreal>(0, sel_row * row_h - (view_h - row_h) / 2);

  painter.save();
  painter.setClipRect(QRectF(0, top - 20 * k, width(), view_h + 40 * k));
  const int first = std::max(0, int(m_scroll / row_h) * cols);
  const int last = std::min(count, first + cols * (int(view_h / row_h) + 3));
  font.setPixelSize(int(24 * k));
  painter.setFont(font);
  for (int i = first; i < last; ++i)
  {
    const Entry& entry = m_entries[i];
    const int col = i % cols;
    const int row = i / cols;
    QRectF r(margin + col * (cw + gap), top + row * row_h - m_scroll, cw, ch);
    const bool selected = i == m_selected;
    if (selected)
      r.adjust(-8 * k, -11 * k, 8 * k, 11 * k);

    QPainterPath clip;
    clip.addRoundedRect(r, 12 * k, 12 * k);
    const QPixmap cover = CoverFor(entry, r.size().toSize());
    if (!cover.isNull())
    {
      painter.save();
      painter.setClipPath(clip, Qt::IntersectClip);
      painter.drawPixmap(r.topLeft(), cover);
      painter.restore();
    }
    else
    {
      painter.fillPath(clip, CARD);
      painter.setPen(TEXT);
      painter.drawText(r.adjusted(14 * k, 14 * k, -14 * k, -14 * k),
                       Qt::AlignCenter | Qt::TextWordWrap, entry.title);
    }

    if (entry.game->GetPlatform() == DiscIO::Platform::WiiDisc ||
        entry.game->GetPlatform() == DiscIO::Platform::WiiWAD)
    {
      const QRectF tag(r.right() - 64 * k, r.top() + 8 * k, 56 * k, 32 * k);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(20, 20, 30, 220));
      painter.drawRoundedRect(tag, 8 * k, 8 * k);
      painter.setPen(TEXT);
      painter.drawText(tag, Qt::AlignCenter, QStringLiteral("Wii"));
    }

    if (selected)
    {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(ACCENT, 5 * k));
      painter.drawRoundedRect(r.adjusted(-5 * k, -5 * k, 5 * k, 5 * k), 14 * k, 14 * k);
    }
  }
  painter.restore();

  // Selected game details
  const Entry& sel = m_entries[std::clamp(m_selected, 0, count - 1)];
  font.setPixelSize(int(34 * k));
  painter.setFont(font);
  painter.setPen(TEXT);
  const QFontMetricsF fm(font);
  painter.drawText(QRectF(0, height() - 185 * k, width(), 50 * k), Qt::AlignCenter,
                   fm.elidedText(sel.title, Qt::ElideRight, width() - 200 * k));
  font.setPixelSize(int(26 * k));
  painter.setFont(font);
  painter.setPen(DIM);
  QString details = QString::fromStdString(sel.game->GetGameID());
  if (sel.disc_count > 1)
    details += QStringLiteral("  ·  ") + tr("%1 disques").arg(sel.disc_count);
  painter.drawText(QRectF(0, height() - 140 * k, width(), 36 * k), Qt::AlignCenter, details);

  if (m_mode == Mode::Library)
  {
    DrawHints(painter, {{QStringLiteral("A"), tr("Jouer")},
                        {QStringLiteral("B"), tr("Quitter")},
                        {QStringLiteral("Start-Pause"), tr("Menu")},
                        {QStringLiteral("L"), tr("Page -")},
                        {QStringLiteral("R"), tr("Page +")}});
  }
}

void BigPictureWindow::DrawMenu(QPainter& painter)
{
  const qreal k = height() / 1080.0;
  painter.fillRect(rect(), QColor(0, 0, 0, 190));

  const QString items[] = {tr("Retour à la bibliothèque"), tr("Interface classique"),
                           tr("Quitter Dolphin")};
  QFont font = painter.font();
  font.setPixelSize(int(34 * k));
  painter.setFont(font);
  const qreal w = 760 * k;
  const qreal item_h = 76 * k;
  qreal y = height() / 2.0 - item_h * 1.5;
  for (int i = 0; i < 3; ++i)
  {
    const QRectF r((width() - w) / 2, y, w, item_h - 12 * k);
    if (i == m_menu_index)
    {
      painter.setPen(Qt::NoPen);
      painter.setBrush(ACCENT);
      painter.drawRoundedRect(r, 14 * k, 14 * k);
    }
    painter.setPen(TEXT);
    painter.drawText(r.adjusted(28 * k, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, items[i]);
    y += item_h;
  }
  DrawHints(painter, {{QStringLiteral("A"), tr("Valider")}, {QStringLiteral("B"), tr("Retour")}});
}

void BigPictureWindow::DrawDialog(QPainter& painter, const QString& title,
                                  const std::vector<std::pair<QString, QString>>& hints)
{
  const qreal k = height() / 1080.0;
  painter.fillRect(rect(), QColor(0, 0, 0, 170));
  const QRectF box((width() - 760 * k) / 2, (height() - 260 * k) / 2, 760 * k, 260 * k);
  painter.setPen(Qt::NoPen);
  painter.setBrush(CARD);
  painter.drawRoundedRect(box, 22 * k, 22 * k);

  QFont font = painter.font();
  font.setPixelSize(int(34 * k));
  painter.setFont(font);
  painter.setPen(TEXT);
  painter.drawText(QRectF(box.left(), box.top() + 40 * k, box.width(), 70 * k), Qt::AlignCenter,
                   title);

  font.setPixelSize(int(26 * k));
  painter.setFont(font);
  const QFontMetricsF fm(font);
  const qreal icon = 46 * k;
  qreal x = box.center().x() - 140 * k;
  const qreal y = box.bottom() - 70 * k;
  for (const auto& [btn, label] : hints)
  {
    DrawIcon(painter, btn, QRectF(x, y - icon / 2, icon, icon));
    painter.drawText(QRectF(x + icon + 12 * k, y - icon / 2, 200 * k, icon),
                     Qt::AlignVCenter | Qt::AlignLeft, label);
    x += 200 * k;
  }
}
