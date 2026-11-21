// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Controller-friendly fullscreen interface ("Mode manette").

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "DolphinQt/BigPicture/BigPictureSettings.h"

class BigPictureAudio;
class GameListModel;
class QTimer;

namespace Core
{
enum class State;
}

namespace UICommon
{
class GameFile;
}

namespace ciface::Core
{
class InputDetector;
}

namespace ciface::MappingCommon
{
class CalibrationBuilder;
}

namespace Memcard
{
class GCMemcard;
}

namespace Config
{
class Layer;
}

class BigPictureWindow final : public QWidget
{
  Q_OBJECT

public:
  explicit BigPictureWindow(const GameListModel& model, QWidget* parent = nullptr);
  ~BigPictureWindow() override;

  // Shows the interface fullscreen and starts listening to the controller.
  void Open();
  // Leaves the controller mode entirely.
  void Close();
  bool IsActive() const { return m_active; }

  // Called by the main window for every emulation state change.
  void OnEmulationStateChanged(Core::State state);

signals:
  void LaunchRequested(const QString& path);
  void ClassicRequested();
  void QuitRequested();
  void StopRequested();    // "Quitter le jeu" in the pause menu
  void ResumeRequested();  // the game gets the focus back

protected:
  void paintEvent(QPaintEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

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
    Favorite,  // Y
    Options,   // X
    Count,
  };

  enum class Mode
  {
    Intro,
    Library,
    Menu,
    ConfirmQuit,
    Setup,
    InGame,  // hidden, a game is running
    Pause,
    Launching,     // loading screen while the game boots
    FadingOut,     // the menu fades away, revealing the game
    Remap,         // waiting for one control
    Calibrate,     // stick calibration
    Memcard,       // memory card manager
    GameSettings,  // per-game settings
    Players,       // multiplayer: who plays on which port
    PadTest,       // live view of a controller
    Closing,       // fading to black before stopping the game
    ConfirmUpdate, // an update is available
  };

  struct SaveIcon
  {
    u8 index = 0;
    QString title;
    QString comment;
    u16 blocks = 0;
    QPixmap banner;
    std::vector<QPixmap> frames;
    std::vector<int> delays;  // in 1/60 s
  };

  struct CardView
  {
    QString path;
    QString error;
    std::unique_ptr<Memcard::GCMemcard> card;
    std::vector<SaveIcon> saves;
    u16 free_blocks = 0;
  };

  enum class Tab
  {
    All,
    Recent,
    Favorites,
    Settings,
  };

  struct Entry
  {
    std::shared_ptr<const UICommon::GameFile> game;
    QString path;
    QString title;
    QStringList discs;  // every disc of the game, in order
  };

  // ---- lifecycle / data
  void Suspend();
  void ScheduleRebuild();
  void RebuildList();
  void ApplyTab();
  void ApplyPreferences();
  const Entry* Selected() const;

  // ---- input
  void PollController();
  void PollInGame();
  void Press(Action action);
  void HandleAction(Action action);
  void HandleLibrary(Action action);
  void HandleSettings(Action action);
  void HandlePause(Action action);
  void HandleMenu(Action action);
  void ToggleFavorite();
  void SetSelected(int index);

  // ---- pause menu
  void OpenPause();
  void Resume();
  std::vector<int> PauseItems() const;

  // ---- controller setup wizard
  void StartSetup(int port);
  void StartSetupStep();
  void UpdateSetup();
  void FinishSetup(bool save);
  bool NeedsSetup() const;

  // ---- remap / calibration
  void StartRemap(int port, int group, int index, int mirror, bool flip, const QString& prompt);
  void StartRemapDetection();
  void UpdateRemap();
  void FinishRemap(bool ok);
  void StartCalibration(int port, int group);
  void UpdateCalibration();
  void FinishCalibration(bool apply);
  void DrawRemap(QPainter& painter);
  void DrawCalibration(QPainter& painter);

  // ---- memory cards
  void OpenMemcards();
  void LoadCard(int slot);
  void HandleMemcard(Action action);
  void DrawMemcards(QPainter& painter);

  // ---- per-game settings
  void OpenGameSettings();
  void OpenGameSettingsFor(const Entry& entry, bool cheats_only);

  // ---- controllers
  void PollDevices();          // auto-switch, multiplayer assignment
  void OnDevicesChanged();     // hot plug
  void SwitchPort(int port, const std::string& device, bool announce);
  std::string PressedDevice(const std::vector<std::string>& exclude) const;
  void DrawPlayers(QPainter& painter);
  void DrawPadTest(QPainter& painter);
  void Notify(const QString& text);  // toast, also over a running game

  // ---- updates
  void CheckUpdates(bool silent);

  // ---- themes
  void ApplyTheme();
  void DrawIntroGameCube(QPainter& painter);
  std::vector<BigPictureSettingCategory>& Cats();

  // ---- pause screenshots
  QString PauseShotPath() const;
  QString SlotThumbPath(int slot) const;
  void LoadSlotThumbs();

  // ---- animation
  void Tick();
  void BeginTransition(int direction);
  void SetMode(Mode mode);
  void Toast(const QString& text);

  // ---- drawing
  QFont Font(bool display, int px, int weight) const;
  void DrawIcon(QPainter& painter, const QString& name, const QRectF& rect);
  void DrawHints(QPainter& painter, const std::vector<std::pair<QString, QString>>& left,
                 const std::vector<std::pair<QString, QString>>& right);
  qreal DrawPillButton(QPainter& painter, qreal x, qreal y, const QString& icon,
                       const QString& label, bool primary, qreal alpha = 1.0);
  void DrawScene(QPainter& painter);
  void DrawBackground(QPainter& painter);
  void DrawIntro(QPainter& painter);
  void DrawTopBar(QPainter& painter);
  void DrawHero(QPainter& painter, const Entry& sel, qreal left, qreal top, qreal max_width);
  void DrawCoverShelf(QPainter& painter);
  void DrawList(QPainter& painter);
  void DrawEmpty(QPainter& painter);
  void DrawSettings(QPainter& painter, qreal top);
  void DrawMenu(QPainter& painter);
  void DrawDialog(QPainter& painter, const QString& title,
                  const std::vector<std::pair<QString, QString>>& hints);
  void DrawSetup(QPainter& painter);
  void DrawPause(QPainter& painter);
  void DrawToast(QPainter& painter);

  QPixmap CoverFor(const Entry& entry, const QSize& size);
  QPixmap BannerFor(const Entry& entry, const QSize& size);
  QColor AmbientFor(const Entry& entry);
  QString PlayInfo(const Entry& entry) const;
  QString ControllerName() const;

  const GameListModel& m_model;
  std::vector<Entry> m_all;      // every game
  std::vector<Entry> m_entries;  // games of the current tab

  std::unique_ptr<BigPictureAudio> m_audio;

  bool m_active = false;
  bool m_intro_done = false;
  Mode m_mode = Mode::Library;
  Mode m_mode_before_setup = Mode::Library;
  Tab m_tab = Tab::All;
  int m_selected = 0;
  int m_menu_index = 0;
  QSet<QString> m_favorites;

  // settings screen
  std::vector<BigPictureSettingCategory> m_settings;
  int m_settings_cat = 0;
  int m_settings_item = 0;
  bool m_settings_in_items = false;
  bool m_pause_settings = false;  // settings opened from the pause menu
  qreal m_settings_scroll = 0;

  // pause menu
  const Entry* m_running = nullptr;
  Entry m_running_entry;
  int m_pause_index = 0;
  int m_slot = 1;
  int m_disc = 0;
  bool m_home_was_down = true;

  // preferences
  BigPicturePrefs::View m_view = BigPicturePrefs::View::Covers;
  BigPicturePrefs::Effect m_effect = BigPicturePrefs::Effect::Halo;
  BigPicturePrefs::Transition m_transition = BigPicturePrefs::Transition::Fade;

  // animation state
  QElapsedTimer m_clock;
  qint64 m_last_tick = 0;
  qreal m_time = 0;              // seconds, for ambient effects
  qreal m_scroll = 0;            // cover shelf (px at 1080p)
  qreal m_list_scroll = 0;       // list view (rows)
  qreal m_hero_t = 1;            // hero fade-in after a selection change
  qreal m_overlay_t = 0;         // menu / dialog / pause opening
  qreal m_intro_t = 0;
  qreal m_trans_t = 1;           // screen transition
  int m_trans_dir = 0;
  QPixmap m_frame;               // last composed frame
  QPixmap m_trans_from;          // frame before the transition
  std::vector<qreal> m_focus;    // per entry: 0 = normal, 1 = selected
  QColor m_ambient;
  QString m_toast;
  qreal m_toast_t = 0;

  QTimer* m_poll_timer = nullptr;
  QTimer* m_game_timer = nullptr;
  QTimer* m_anim_timer = nullptr;
  QTimer* m_rebuild_timer = nullptr;

  // Controller edge detection / auto-repeat
  u32 m_prev_held = 0;
  qint64 m_held_since[16] = {};
  qint64 m_last_repeat[16] = {};

  // setup wizard
  int m_setup_port = 0;
  int m_setup_step = 0;
  bool m_setup_device_chosen = false;
  std::unique_ptr<ciface::Core::InputDetector> m_detector;
  QTimer* m_setup_timer = nullptr;
  QTimer* m_setup_check_timer = nullptr;

  QString m_display_family;
  QString m_text_family;

  QHash<QString, QPixmap> m_cover_cache;
  QHash<QString, QPixmap> m_banner_cache;
  QHash<QString, QColor> m_ambient_cache;
  QHash<QString, QPixmap> m_icons;

  // remap
  int m_remap_port = 0, m_remap_group = 0, m_remap_index = 0, m_remap_mirror = -1;
  bool m_remap_flip = false;
  QString m_remap_prompt;
  Mode m_mode_before_remap = Mode::Library;
  qint64 m_remap_started = 0;

  // calibration
  int m_cal_port = 0, m_cal_group = 0;
  std::unique_ptr<ciface::MappingCommon::CalibrationBuilder> m_calibrator;
  qreal m_cal_x = 0, m_cal_y = 0;

  // memory cards
  std::array<CardView, 2> m_cards;
  int m_mc_card = 0;
  int m_mc_index = 0;
  bool m_mc_actions_open = false;
  bool m_mc_confirm_erase = false;
  int m_mc_action = 0;
  Mode m_mode_before_memcard = Mode::Library;

  // per-game settings
  std::shared_ptr<Config::Layer> m_game_layer;
  std::vector<BigPictureSettingCategory> m_game_settings;
  QString m_game_settings_title;

  // loading / fading
  qreal m_fade_out = 0;  // FadingOut progress
  qreal m_fade_in = 1;   // black veil after returning from a game (1 = done)
  qreal m_window_opacity = 1;
  std::array<QPixmap, 9> m_slot_thumbs;

  // quick settings / cheats (pause)
  std::vector<BigPictureSettingCategory> m_quick_settings;
  bool m_quick = false;
  Mode m_mode_before_game_settings = Mode::Library;

  // controllers
  QTimer* m_device_timer = nullptr;
  QStringList m_known_devices;
  int m_test_port = 0;
  qint64 m_test_start_since = -1;
  Mode m_mode_before_players = Mode::Library;
  qint64 m_last_switch = 0;

  // in-game notifications (separate, click-through window)
  QWidget* m_notifier = nullptr;
  QTimer* m_notify_timer = nullptr;
  QString m_notify_text;
  qreal m_notify_t = 0;

  // closing
  qreal m_close_t = 0;
  bool m_stop_sent = false;

  // updates
  QString m_update_url;
  QString m_update_version;
  bool m_update_busy = false;
};
