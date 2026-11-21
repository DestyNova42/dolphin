// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Controller-friendly fullscreen interface ("Mode manette").

#pragma once

#include <memory>
#include <vector>

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "Common/CommonTypes.h"

class GameListModel;
class QSvgRenderer;
class QTimer;

namespace UICommon
{
class GameFile;
}

namespace ciface::Core
{
class InputDetector;
}

class BigPictureWindow final : public QWidget
{
  Q_OBJECT

public:
  explicit BigPictureWindow(const GameListModel& model, QWidget* parent = nullptr);
  ~BigPictureWindow() override;

  // Shows the interface fullscreen and starts listening to the controller.
  void Open();
  // Hides the interface (e.g. while a game is running) but keeps it "active".
  void Suspend();
  // Leaves the controller mode entirely.
  void Close();

  bool IsActive() const { return m_active; }

signals:
  void LaunchRequested(const QString& path);
  void ClassicRequested();
  void QuitRequested();

protected:
  void paintEvent(QPaintEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;

private:
  enum class Action
  {
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
    Menu,
    PageUp,
    PageDown,
  };

  enum class Mode
  {
    Library,
    Menu,
    ConfirmQuit,
    Setup,
  };

  struct Entry
  {
    std::shared_ptr<const UICommon::GameFile> game;
    QString title;
    int disc_count = 1;
  };

  // Controller setup wizard (maps GameCube port 1 by asking for each button).
  void StartSetup();
  void StartSetupStep();
  void UpdateSetup();
  void FinishSetup(bool save);
  bool NeedsSetup() const;
  void DrawSetup(QPainter& painter);

  void ScheduleRebuild();
  void RebuildList();
  void PollController();
  void Press(Action action);
  void HandleAction(Action action);
  void Tick();

  int Columns() const;
  QPixmap CoverFor(const Entry& entry, const QSize& size);
  void DrawIcon(QPainter& painter, const QString& name, const QRectF& rect);
  qreal DrawHints(QPainter& painter, const std::vector<std::pair<QString, QString>>& hints);
  void DrawLibrary(QPainter& painter);
  void DrawMenu(QPainter& painter);
  void DrawDialog(QPainter& painter, const QString& title,
                  const std::vector<std::pair<QString, QString>>& hints);

  const GameListModel& m_model;
  std::vector<Entry> m_entries;

  bool m_active = false;
  Mode m_mode = Mode::Library;
  int m_selected = 0;
  int m_menu_index = 0;
  qreal m_scroll = 0;
  qreal m_scroll_target = 0;

  QTimer* m_poll_timer = nullptr;
  QTimer* m_anim_timer = nullptr;
  QTimer* m_rebuild_timer = nullptr;

  // Controller edge detection / auto-repeat
  u32 m_prev_held = 0;
  u32 m_key_held = 0;
  QElapsedTimer m_clock;
  qint64 m_held_since[16] = {};
  qint64 m_last_repeat[16] = {};

  int m_setup_step = 0;
  bool m_setup_device_chosen = false;
  std::unique_ptr<ciface::Core::InputDetector> m_detector;
  QTimer* m_setup_timer = nullptr;
  QTimer* m_setup_check_timer = nullptr;

  QHash<QString, QPixmap> m_cover_cache;
  QHash<QString, QSvgRenderer*> m_icons;
};
