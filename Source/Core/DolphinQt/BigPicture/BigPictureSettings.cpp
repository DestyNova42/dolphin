// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureSettings.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QProcess>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ActionReplay.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"
#include "Core/Config/GraphicsSettings.h"
#ifdef _WIN32
#include "InputCommon/ControllerInterface/Switch2BLE/Switch2BLE.h"
#endif
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "Core/PowerPC/PowerPC.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

using Item = BigPictureSettingItem;

namespace
{
QString Tr(const char* text)
{
  return QCoreApplication::translate("BigPictureSettings", text);
}

// ---------------------------------------------------------------- item builders

Item Toggle(const QString& label, const QString& help, const Config::Info<bool>& info,
            bool inverted = false)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Toggle;
  const auto* p = &info;
  item.is_on = [p, inverted] { return Config::Get(*p) != inverted; };
  item.change = [p](int) { Config::SetBaseOrCurrent(*p, !Config::Get(*p)); };
  item.activate = [p] { Config::SetBaseOrCurrent(*p, !Config::Get(*p)); };
  return item;
}

template <typename T>
Item Choice(const QString& label, const QString& help, const Config::Info<T>& info,
            std::vector<std::pair<T, QString>> options)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  const auto* p = &info;
  auto opts = std::make_shared<std::vector<std::pair<T, QString>>>(std::move(options));
  const auto index = [p, opts]() -> int {
    const T current = Config::Get(*p);
    for (size_t i = 0; i < opts->size(); ++i)
    {
      if ((*opts)[i].first == current)
        return static_cast<int>(i);
    }
    return -1;
  };
  item.value = [index, opts] {
    const int i = index();
    return i >= 0 ? (*opts)[i].second : Tr("Personnalisé");
  };
  item.change = [p, index, opts](int dir) {
    const int n = static_cast<int>(opts->size());
    int i = index();
    i = i < 0 ? 0 : (i + dir + n) % n;
    Config::SetBaseOrCurrent(*p, (*opts)[i].first);
  };
  item.activate = [change = item.change] { change(1); };
  return item;
}

Item IntRange(const QString& label, const QString& help, const Config::Info<int>& info, int min,
              int max, int step, std::function<QString(int)> format)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  const auto* p = &info;
  item.value = [p, format] { return format(Config::Get(*p)); };
  item.change = [p, min, max, step](int dir) {
    Config::SetBaseOrCurrent(*p, std::clamp(Config::Get(*p) + dir * step, min, max));
  };
  item.activate = [] {};
  return item;
}

Item Action(const QString& label, const QString& help, std::function<void()> fn)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Action;
  item.activate = std::move(fn);
  item.change = [](int) {};
  return item;
}

// Controller-mode preferences in QSettings
Item PrefChoice(const QString& label, const QString& help, const QString& key,
                std::vector<QString> options, int def, std::function<void()> changed)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  auto opts = std::make_shared<std::vector<QString>>(std::move(options));
  item.value = [key, opts, def] {
    const int i = Settings::GetQSettings().value(key, def).toInt();
    return (*opts)[std::clamp(i, 0, static_cast<int>(opts->size()) - 1)];
  };
  item.change = [key, opts, def, changed](int dir) {
    const int n = static_cast<int>(opts->size());
    const int i = Settings::GetQSettings().value(key, def).toInt();
    Settings::GetQSettings().setValue(key, (i + dir + n) % n);
    if (changed)
      changed();
  };
  item.activate = [change = item.change] { change(1); };
  return item;
}

Item PrefToggle(const QString& label, const QString& help, const QString& key, bool def,
                std::function<void()> changed)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Toggle;
  item.is_on = [key, def] { return Settings::GetQSettings().value(key, def).toBool(); };
  item.change = [key, def, changed](int) {
    QSettings& s = Settings::GetQSettings();
    s.setValue(key, !s.value(key, def).toBool());
    if (changed)
      changed();
  };
  item.activate = [change = item.change] { change(1); };
  return item;
}

Item PrefPercent(const QString& label, const QString& help, const QString& key, int def,
                 std::function<void()> changed)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  item.value = [key, def] {
    return QStringLiteral("%1 %").arg(Settings::GetQSettings().value(key, def).toInt());
  };
  item.change = [key, def, changed](int dir) {
    QSettings& s = Settings::GetQSettings();
    s.setValue(key, std::clamp(s.value(key, def).toInt() + dir * 10, 0, 100));
    if (changed)
      changed();
  };
  item.activate = [] {};
  return item;
}

QString Percent(float v)
{
  return QStringLiteral("%1 %").arg(static_cast<int>(std::lround(v * 100.0f)));
}
}  // namespace

// ---------------------------------------------------------------- preferences

namespace BigPicturePrefs
{
static int Pref(const char* key, int def)
{
  return Settings::GetQSettings().value(QString::fromLatin1(key), def).toInt();
}
View GetView()
{
  return static_cast<View>(std::clamp(Pref("bigpicture/view", 0), 0, 1));
}
Effect GetEffect()
{
  return static_cast<Effect>(std::clamp(Pref("bigpicture/effect", 0), 0, 5));
}
Theme GetTheme()
{
  return static_cast<Theme>(std::clamp(Pref("bigpicture/theme", 0), 0, 6));
}
IntroStyle GetIntroStyle()
{
  return static_cast<IntroStyle>(std::clamp(Pref("bigpicture/intro_style", 0), 0, 1));
}
bool AutoSwitchController()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/autoswitch"), true).toBool();
}
bool CheckUpdatesOnStart()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/check_updates"), true).toBool();
}
Transition GetTransition()
{
  return static_cast<Transition>(std::clamp(Pref("bigpicture/transition", 0), 0, 3));
}
bool IntroEnabled()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/intro"), true).toBool();
}
bool MusicEnabled()
{
  return Settings::GetQSettings().value(QStringLiteral("bigpicture/music"), true).toBool();
}
int MusicVolume()
{
  return std::clamp(Pref("bigpicture/music_volume", 60), 0, 100);
}
int SfxVolume()
{
  return std::clamp(Pref("bigpicture/sfx_volume", 90), 0, 100);
}
}  // namespace BigPicturePrefs

void SaveBigPictureSettings()
{
  Config::Save();
  SaveBigPicturePads();
  Settings::GetQSettings().sync();
}

// ---------------------------------------------------------------- the list

std::vector<BigPictureSettingCategory> BuildBigPictureSettings(const BigPictureSettingsHost& host)
{
  std::vector<BigPictureSettingCategory> cats;
  const auto changed = host.interface_changed;

  // ---- Controller mode
  {
    BigPictureSettingCategory c{Tr("Mode manette"), {}};
    c.items.push_back(PrefChoice(Tr("Affichage des jeux"),
                                 Tr("Jaquettes en rangée, ou liste avec les bannières des jeux."),
                                 QStringLiteral("bigpicture/view"),
                                 {Tr("Jaquettes"), Tr("Liste avec bannières")}, 0, changed));
    c.items.push_back(PrefChoice(Tr("Trier les jeux"), Tr("Aussi avec Z dans la bibliothèque."),
                                 QStringLiteral("bigpicture/sort"),
                                 {Tr("Nom"), Tr("Dernière partie"), Tr("Temps de jeu")}, 0,
                                 changed));
    c.items.push_back(PrefChoice(Tr("Préréglage d'affichage"),
                                 Tr("Automatique : « Portable » sur un petit écran (Ally, Deck), "
                                    "« TV » sur un grand. Remplace les réglages graphiques pendant "
                                    "le jeu."),
                                 QStringLiteral("bigpicture/preset"),
                                 {Tr("Désactivé"), Tr("Automatique"), Tr("Portable"), Tr("TV")}, 0,
                                 changed));
    c.items.push_back(PrefChoice(
        Tr("Bouton du menu en jeu"),
        Tr("Sur SteamOS / Bazzite, le bouton Guide ouvre le menu Steam : choisis Select (View), "
           "les deux sticks enfoncés ou START + Z. Dans les jeux Wii, maintiens-le un instant."),
        QStringLiteral("bigpicture/menu_button"),
        {Tr("Automatique"), Tr("Guide / Home"), Tr("Select (View)"), Tr("Les deux sticks (L3 + R3)"),
         Tr("START + Z")},
        0, {}));
    {
      // Third-party "Pro Controllers" (8BitDo…): how SDL reads them. For GameCube and Wii alike.
      static const Config::Info<std::string> HIDAPI_SWITCH{
          {Config::System::Main, "SDL_Hints", "SDL_JOYSTICK_HIDAPI_SWITCH"}, ""};
      const auto mode = [] {
        if (Config::Get(HIDAPI_SWITCH) == "0")
          return 2;
        return Config::Get(Config::MAIN_SDL_HINT_JOYSTICK_ENHANCED_REPORTS) == "0" ? 1 : 0;
      };
      const auto names = std::make_shared<std::vector<QString>>(std::vector<QString>{
          Tr("Normal"), Tr("Rapports simples"), Tr("Manette standard")});
      Item item;
      item.label = Tr("Manettes Switch d'autres marques");
      item.help = Tr("Pour une manette vue comme « Nintendo Switch Pro Controller » (8BitDo…) "
                     "qui ne répond pas ou se comporte bizarrement. Rapports simples : la "
                     "manette envoie ses boutons sans le gyroscope. Manette standard : lue "
                     "comme une manette ordinaire. Redémarre Dolphin après un changement.");
      item.kind = Item::Kind::Choice;
      item.value = [mode, names] { return (*names)[mode()]; };
      item.change = [mode, notify = host.notify](int dir) {
        const int m = (mode() + 3 + (dir < 0 ? -1 : 1)) % 3;
        Config::SetBase(HIDAPI_SWITCH, m == 2 ? "0" : "1");
        Config::SetBase(Config::MAIN_SDL_HINT_JOYSTICK_ENHANCED_REPORTS, m == 1 ? "0" : "1");
        Config::Save();
        if (notify)
          notify(Tr("Redémarre Dolphin pour appliquer"));
      };
      item.activate = [change = item.change] { change(1); };
      c.items.push_back(std::move(item));
    }
    {
      const auto diag = host.device_diagnostic;
      c.items.push_back(Action(Tr("Diagnostic des manettes"),
                               Tr("Chaque bouton et axe de chaque manette, en direct, tel que "
                                  "Dolphin le reçoit."),
                               [diag] { diag(); }));
    }
    c.items.push_back(PrefChoice(Tr("Icônes des boutons"),
                                 Tr("Les boutons affichés en bas de l'écran."),
                                 QStringLiteral("bigpicture/icons"),
                                 {Tr("GameCube"), Tr("Wii (manette classique)")}, 0, {}));
    c.items.push_back(PrefToggle(Tr("Mise en veille propre"),
                                 Tr("Avant la veille du PC ou de la console, le jeu est mis en "
                                    "pause et sauvegardé ; au réveil, le menu pause s'ouvre."),
                                 QStringLiteral("bigpicture/sleep_save"), true, {}));
    {
      // Sync folder: cycles through the usual cloud / Syncthing folders, A to pick another
      Item item;
      item.label = Tr("Synchroniser les sauvegardes");
      item.help = Tr("Cartes mémoire, sauvegardes Wii et reprises, dans un dossier partagé "
                     "(OneDrive, Syncthing, Nextcloud…) : les plus récentes gagnent.");
      item.kind = Item::Kind::Choice;
      const auto candidates = [] {
        QStringList list{QString()};
        const QString home = QDir::homePath();
        const QString onedrive = qEnvironmentVariable("OneDrive");
        for (const QString& d : QStringList{onedrive, home + QStringLiteral("/OneDrive"),
                                 home + QStringLiteral("/Sync"), home + QStringLiteral("/Nextcloud"),
                                 home + QStringLiteral("/Dropbox")})
        {
          if (!d.isEmpty() && QFileInfo(d).isDir() && !list.contains(d))
            list << d;
        }
        const QString cur =
            Settings::GetQSettings().value(QStringLiteral("bigpicture/sync_dir")).toString();
        if (!cur.isEmpty() && !list.contains(cur))
          list << cur;
        return list;
      };
      item.value = [] {
        const QString cur =
            Settings::GetQSettings().value(QStringLiteral("bigpicture/sync_dir")).toString();
        if (cur.isEmpty())
          return Tr("Désactivée");
        return QDir::toNativeSeparators(cur).section(QDir::separator(), -2);
      };
      item.change = [candidates](int dir) {
        const QStringList list = candidates();
        const QString cur =
            Settings::GetQSettings().value(QStringLiteral("bigpicture/sync_dir")).toString();
        int i = static_cast<int>(list.indexOf(cur));
        if (i < 0)
          i = 0;
        const int n = static_cast<int>(list.size());
        Settings::GetQSettings().setValue(QStringLiteral("bigpicture/sync_dir"),
                                          list[(i + dir + n) % n]);
      };
      item.activate = [] {
        const QString dir = QFileDialog::getExistingDirectory(
            nullptr, Tr("Dossier partagé pour les sauvegardes"), QDir::homePath());
        if (!dir.isEmpty())
          Settings::GetQSettings().setValue(QStringLiteral("bigpicture/sync_dir"), dir);
      };
      c.items.push_back(std::move(item));
    }
    {
      const auto sync = host.sync_now;
      c.items.push_back(Action(Tr("Synchroniser maintenant"), {}, [sync] { sync(); }));
    }
    c.items.push_back(PrefToggle(Tr("Reprendre"),
                                 Tr("En quittant un jeu depuis le menu pause, Dolphin le "
                                    "sauvegarde pour reprendre exactement au même endroit."),
                                 QStringLiteral("bigpicture/resume"), true, changed));
    c.items.push_back(PrefToggle(
        Tr("Vraie animation de démarrage GameCube"),
        Tr("Lance le BIOS de ta console avant le jeu. Copie le fichier IPL.bin de ta GameCube "
           "dans le dossier GC\\EUR (ou USA, JAP) de Dolphin."),
        QStringLiteral("bigpicture/real_boot"), false, {}));
    c.items.push_back(PrefChoice(Tr("Thème de couleur"),
                                 Tr("Les couleurs des modèles de GameCube."),
                                 QStringLiteral("bigpicture/theme"),
                                 {Tr("Indigo nuit"), Tr("Indigo"), Tr("Spice Orange"),
                                  Tr("Jet Black"), Tr("Platinum"), Tr("Pearl White"),
                                  Tr("Couleur du jeu")},
                                 0, changed));
    c.items.push_back(PrefChoice(
        Tr("Effet d'arrière-plan"), Tr("L'animation qui vit derrière la bibliothèque."),
        QStringLiteral("bigpicture/effect"),
        {Tr("Halo"), Tr("Particules"), Tr("Cubes"), Tr("Vagues"), Tr("Aucun"),
         Tr("Cube 3D (BIOS)")},
        0, changed));
    c.items.push_back(PrefChoice(Tr("Transitions"),
                                 Tr("L'effet utilisé quand on change d'onglet ou d'écran."),
                                 QStringLiteral("bigpicture/transition"),
                                 {Tr("Fondu"), Tr("Glissement"), Tr("Zoom"), Tr("Aucune")}, 0,
                                 changed));
    c.items.push_back(PrefToggle(Tr("Animation de démarrage"),
                                 Tr("Le cube animé et le jingle au lancement de Dolphin."),
                                 QStringLiteral("bigpicture/intro"), true, changed));
    c.items.push_back(PrefChoice(Tr("Style de l'animation"),
                                 Tr("GameCube : le petit cube qui roule et dessine le logo."),
                                 QStringLiteral("bigpicture/intro_style"),
                                 {Tr("GameCube"), Tr("Minimaliste")}, 0, changed));
    c.items.push_back(PrefToggle(Tr("Musique du menu"),
                                 Tr("Fichiers lus depuis <dossier utilisateur>/BigPicture/Sounds."),
                                 QStringLiteral("bigpicture/music"), true, changed));
    c.items.push_back(PrefPercent(Tr("Volume de la musique"), {},
                                  QStringLiteral("bigpicture/music_volume"), 60, changed));
    c.items.push_back(PrefPercent(Tr("Volume des bruitages"), {},
                                  QStringLiteral("bigpicture/sfx_volume"), 90, changed));
    c.items.push_back(PrefToggle(Tr("Démarrer Dolphin en mode manette"),
                                 Tr("Sinon, Dolphin s'ouvre sur l'interface classique."),
                                 QStringLiteral("bigpicture/onstart"), true, {}));
    c.items.push_back(Toggle(Tr("Télécharger les jaquettes"),
                             Tr("Jaquettes récupérées sur GameTDB (connexion internet requise)."),
                             Config::MAIN_USE_GAME_COVERS));
    c.items.push_back(Action(Tr("Rafraîchir la bibliothèque"),
                             Tr("Relit les dossiers de jeux pour trouver les nouveaux jeux."),
                             [] { Settings::Instance().RefreshGameList(); }));
    {
      const auto notify = host.notify;
      c.items.push_back(Action(
          Tr("Créer un raccourci sur le Bureau"),
          Tr("Lance Dolphin directement en mode manette (aussi utilisable dans Steam)."),
          [notify] {
            const QString desktop =
                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
#ifdef _WIN32
            const QString link = QDir(desktop).filePath(QStringLiteral("Dolphin (mode manette).lnk"));
            QFile::remove(link);
            const bool ok = QFile::link(QCoreApplication::applicationFilePath(), link);
#else
            // Linux: a .desktop launcher (on the desktop and in the applications menu, where
            // Steam's "Add a non-Steam game" finds it).
            const QString appimage = qEnvironmentVariable("APPIMAGE");
            const QString exe =
                appimage.isEmpty() ? QCoreApplication::applicationFilePath() : appimage;
            const QString entry = QStringLiteral("[Desktop Entry]\nType=Application\n"
                                                 "Name=Dolphin (mode manette)\n"
                                                 "Exec=\"%1\"\nIcon=dolphin-emu\n"
                                                 "Categories=Game;Emulator;\nTerminal=false\n")
                                      .arg(exe);
            bool ok = false;
            const QString apps =
                QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
            for (const QString& dir : {desktop, apps})
            {
              if (dir.isEmpty())
                continue;
              QDir().mkpath(dir);
              QFile f(QDir(dir).filePath(QStringLiteral("dolphin-manette.desktop")));
              if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
              {
                f.write(entry.toUtf8());
                f.close();
                f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
                ok = true;
              }
            }
#endif
            Settings::GetQSettings().setValue(QStringLiteral("bigpicture/onstart"), true);
            if (notify)
              notify(ok ? Tr("Raccourci créé sur le Bureau") : Tr("Impossible de créer le raccourci"));
          }));
    }
    c.items.push_back(PrefToggle(Tr("Rechercher les mises à jour au démarrage"), {},
                                 QStringLiteral("bigpicture/check_updates"), true, {}));
    {
      const auto check = host.check_updates;
      c.items.push_back(Action(Tr("Rechercher une mise à jour"),
                               Tr("Télécharge la dernière version compilée de ton Dolphin (avec "
                                  "les nouveautés du Dolphin officiel) et l'installe."),
                               [check] { check(); }));
    }
    cats.push_back(std::move(c));
  }

  // ---- General
  {
    BigPictureSettingCategory c{Tr("Général"), {}};
    c.items.push_back(Choice<float>(Tr("Vitesse d'émulation"),
                                    Tr("100 % = vitesse normale de la console."),
                                    Config::MAIN_EMULATION_SPEED,
                                    {{0.0f, Tr("Illimitée")},
                                     {0.5f, Percent(0.5f)},
                                     {0.75f, Percent(0.75f)},
                                     {1.0f, Percent(1.0f)},
                                     {1.5f, Percent(1.5f)},
                                     {2.0f, Percent(2.0f)},
                                     {3.0f, Percent(3.0f)}}));
    c.items.push_back(Toggle(Tr("Double cœur"),
                             Tr("Plus rapide ; quelques rares jeux préfèrent le simple cœur."),
                             Config::MAIN_CPU_THREAD));
    c.items.push_back(Toggle(Tr("Codes de triche"), Tr("Active les codes AR et Gecko des jeux."),
                             Config::MAIN_ENABLE_CHEATS));
    c.items.push_back(Toggle(Tr("Changement de disque automatique"),
                             Tr("Insère le disque suivant quand le jeu le demande."),
                             Config::MAIN_AUTO_DISC_CHANGE));
    c.items.push_back(Toggle(Tr("Suivi du temps de jeu"), {}, Config::MAIN_TIME_TRACKING));
    c.items.push_back(Toggle(Tr("Messages à l'écran"),
                             Tr("Notifications de Dolphin affichées pendant le jeu."),
                             Config::MAIN_OSD_MESSAGES));
    c.items.push_back(Toggle(Tr("Pause quand la fenêtre perd le focus"), {},
                             Config::MAIN_PAUSE_ON_FOCUS_LOST));
    c.items.push_back(Toggle(Tr("Confirmer avant d'arrêter un jeu"), {},
                             Config::MAIN_CONFIRM_ON_STOP));
    c.items.push_back(Toggle(Tr("Empêcher la mise en veille de l'écran"), {},
                             Config::MAIN_DISABLE_SCREENSAVER));
    cats.push_back(std::move(c));
  }

  // ---- Graphics
  {
    BigPictureSettingCategory c{Tr("Graphismes"), {}};
    std::vector<std::pair<std::string, QString>> backends;
    for (const auto& backend : VideoBackendBase::GetAvailableBackends())
    {
      backends.emplace_back(backend->GetConfigName(),
                            QString::fromStdString(backend->GetDisplayName()));
    }
    c.items.push_back(Choice<std::string>(Tr("Moteur graphique"),
                                          Tr("Pris en compte au prochain lancement d'un jeu."),
                                          Config::MAIN_GFX_BACKEND, std::move(backends)));
    c.items.push_back(Toggle(Tr("Plein écran"), {}, Config::MAIN_FULLSCREEN));
    c.items.push_back(Toggle(Tr("Plein écran sans bordure"),
                             Tr("Nécessaire pour afficher le menu pause par-dessus le jeu."),
                             Config::GFX_BORDERLESS_FULLSCREEN));
    c.items.push_back(Toggle(Tr("Synchronisation verticale"), Tr("Évite les déchirures d'image."),
                             Config::GFX_VSYNC));
    c.items.push_back(Choice<AspectMode>(Tr("Format d'image"), {}, Config::GFX_ASPECT_RATIO,
                                         {{AspectMode::Auto, Tr("Auto")},
                                          {AspectMode::ForceWide, Tr("16:9 forcé")},
                                          {AspectMode::ForceStandard, Tr("4:3 forcé")},
                                          {AspectMode::Stretch, Tr("Étirer à la fenêtre")},
                                          {AspectMode::Raw, Tr("Pixels carrés")}}));
    c.items.push_back(Toggle(Tr("Écran large forcé (hack)"),
                             Tr("Élargit l'image des jeux 4:3. Peut couper des éléments."),
                             Config::GFX_WIDESCREEN_HACK));
    c.items.push_back(Choice<ShaderCompilationMode>(
        Tr("Compilation des shaders"),
        Tr("Asynchrone : moins de saccades, quelques effets peuvent manquer un instant."),
        Config::GFX_SHADER_COMPILATION_MODE,
        {{ShaderCompilationMode::Synchronous, Tr("Synchrone")},
         {ShaderCompilationMode::SynchronousUberShaders, Tr("Ubershaders (synchrone)")},
         {ShaderCompilationMode::AsynchronousUberShaders, Tr("Ubershaders (asynchrone)")},
         {ShaderCompilationMode::AsynchronousSkipRendering, Tr("Asynchrone")}}));
    c.items.push_back(Toggle(Tr("Compiler les shaders avant de démarrer"),
                             Tr("Démarrage plus long, moins de saccades ensuite."),
                             Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING));
    c.items.push_back(Toggle(Tr("Sortie HDR"), Tr("S'applique au prochain lancement du jeu."),
                           Config::GFX_ENHANCE_HDR_OUTPUT));
  c.items.push_back(Toggle(Tr("Afficher les FPS"), {}, Config::GFX_SHOW_FPS));
    c.items.push_back(Toggle(Tr("Afficher la vitesse"), {}, Config::GFX_SHOW_SPEED));
    c.items.push_back(Toggle(Tr("Afficher la résolution interne"), {},
                             Config::GFX_SHOW_INTERNAL_RESOLUTION));
    cats.push_back(std::move(c));
  }

  // ---- Enhancements
  {
    BigPictureSettingCategory c{Tr("Améliorations"), {}};
    c.items.push_back(Toggle(Tr("Sortie HDR"),
                             Tr("Couleurs et luminosité étendues sur un écran HDR."),
                             Config::GFX_ENHANCE_HDR_OUTPUT));
    c.items.push_back(PrefToggle(Tr("Basculer l'écran en HDR automatiquement"),
                                 Tr("Pendant le jeu, Windows passe en HDR si la sortie HDR est "
                                    "activée (et en SDR sinon), puis revient comme avant."),
                                 QStringLiteral("bigpicture/auto_hdr"), true, {}));
    c.items.push_back(Choice<int>(Tr("Résolution interne"),
                                  Tr("Plus c'est haut, plus l'image est nette et plus la carte "
                                    "graphique travaille."),
                                  Config::GFX_EFB_SCALE,
                                  {{0, Tr("Auto (fenêtre)")},
                                   {1, Tr("Native (480p)")},
                                   {2, Tr("2× (720p)")},
                                   {3, Tr("3× (1080p)")},
                                   {4, Tr("4× (1440p)")},
                                   {5, Tr("5× (1800p)")},
                                   {6, Tr("6× (4K)")},
                                   {7, Tr("7×")},
                                   {8, Tr("8× (5K)")}}));
    {
      // Anti-aliasing: MSAA or SSAA, as one choice.
      struct AA
      {
        u32 samples;
        bool ssaa;
        const char* name;
      };
      static constexpr AA modes[] = {{1, false, "Aucun"},   {2, false, "MSAA 2×"},
                                     {4, false, "MSAA 4×"}, {8, false, "MSAA 8×"},
                                     {2, true, "SSAA 2×"},  {4, true, "SSAA 4×"},
                                     {8, true, "SSAA 8×"}};
      Item item;
      item.label = Tr("Anticrénelage");
      item.help = Tr("Lisse les bords. Le SSAA est plus beau mais beaucoup plus lourd.");
      item.kind = Item::Kind::Choice;
      const auto index = [] {
        const u32 s = Config::Get(Config::GFX_MSAA);
        const bool ss = Config::Get(Config::GFX_SSAA);
        for (int i = 0; i < static_cast<int>(std::size(modes)); ++i)
        {
          if (modes[i].samples == s && (s == 1 || modes[i].ssaa == ss))
            return i;
        }
        return 0;
      };
      item.value = [index] { return Tr(modes[index()].name); };
      item.change = [index](int dir) {
        const int n = static_cast<int>(std::size(modes));
        const int i = (index() + dir + n) % n;
        Config::SetBaseOrCurrent(Config::GFX_MSAA, modes[i].samples);
        Config::SetBaseOrCurrent(Config::GFX_SSAA, modes[i].ssaa);
      };
      item.activate = [change = item.change] { change(1); };
      c.items.push_back(std::move(item));
    }
    c.items.push_back(Choice<AnisotropicFilteringMode>(
        Tr("Filtrage anisotrope"), Tr("Textures nettes vues de biais."),
        Config::GFX_ENHANCE_MAX_ANISOTROPY,
        {{AnisotropicFilteringMode::Default, Tr("Par défaut du jeu")},
         {AnisotropicFilteringMode::Force1x, Tr("1×")},
         {AnisotropicFilteringMode::Force2x, Tr("2×")},
         {AnisotropicFilteringMode::Force4x, Tr("4×")},
         {AnisotropicFilteringMode::Force8x, Tr("8×")},
         {AnisotropicFilteringMode::Force16x, Tr("16×")}}));
    c.items.push_back(Choice<TextureFilteringMode>(
        Tr("Filtrage des textures"), {}, Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
        {{TextureFilteringMode::Default, Tr("Par défaut")},
         {TextureFilteringMode::Nearest, Tr("Plus proche (pixelisé)")},
         {TextureFilteringMode::Linear, Tr("Linéaire (lissé)")}}));
    c.items.push_back(Choice<OutputResamplingMode>(
        Tr("Rééchantillonnage de l'image"), Tr("Méthode de mise à l'échelle de l'image finale."),
        Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
        {{OutputResamplingMode::Default, Tr("Par défaut")},
         {OutputResamplingMode::Bilinear, Tr("Bilinéaire")},
         {OutputResamplingMode::BSpline, Tr("B-Spline")},
         {OutputResamplingMode::MitchellNetravali, Tr("Mitchell-Netravali")},
         {OutputResamplingMode::CatmullRom, Tr("Catmull-Rom")},
         {OutputResamplingMode::SharpBilinear, Tr("Bilinéaire net")},
         {OutputResamplingMode::AreaSampling, Tr("Échantillonnage par zone")}}));
    c.items.push_back(Toggle(Tr("Éclairage par pixel"), {}, Config::GFX_ENABLE_PIXEL_LIGHTING));
    c.items.push_back(Toggle(Tr("Couleurs 24 bits forcées"), Tr("Supprime les bandes de couleur."),
                             Config::GFX_ENHANCE_FORCE_TRUE_COLOR));
    c.items.push_back(Toggle(Tr("Désactiver le filtre de copie"),
                             Tr("Image plus nette dans certains jeux."),
                             Config::GFX_ENHANCE_DISABLE_COPY_FILTER));
    c.items.push_back(Toggle(Tr("Désactiver le brouillard"), {}, Config::GFX_DISABLE_FOG));
    c.items.push_back(Toggle(Tr("Textures HD personnalisées"),
                             Tr("Packs de textures placés dans Load/Textures."),
                             Config::GFX_HIRES_TEXTURES));
    c.items.push_back(Toggle(Tr("Précharger les textures HD"), {},
                             Config::GFX_CACHE_HIRES_TEXTURES));
    c.items.push_back(Toggle(Tr("Mods graphiques"), {}, Config::GFX_MODS_ENABLE));
    c.items.push_back(Toggle(Tr("Sortie HDR"), {}, Config::GFX_ENHANCE_HDR_OUTPUT));
    c.items.push_back(Choice<StereoMode>(Tr("3D stéréoscopique"), {}, Config::GFX_STEREO_MODE,
                                         {{StereoMode::Off, Tr("Désactivée")},
                                          {StereoMode::SideBySide, Tr("Côte à côte")},
                                          {StereoMode::TopAndBottom, Tr("Haut et bas")},
                                          {StereoMode::Anaglyph, Tr("Anaglyphe")},
                                          {StereoMode::Passive, Tr("Passive")}}));
    cats.push_back(std::move(c));
  }

  // ---- Graphics hacks
  {
    BigPictureSettingCategory c{Tr("Hacks graphiques"), {}};
    c.items.push_back(Choice<int>(Tr("Précision du cache de textures"), {},
                                  Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                                  {{0, Tr("Sûr")}, {512, Tr("Moyen")}, {128, Tr("Rapide")}}));
    c.items.push_back(Toggle(Tr("Ignorer l'accès EFB depuis le CPU"), {},
                             Config::GFX_HACK_EFB_ACCESS_ENABLE, true));
    c.items.push_back(Toggle(Tr("Ignorer les changements de format"), {},
                             Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true));
    c.items.push_back(Toggle(Tr("Copies EFB vers texture uniquement"), {},
                             Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
    c.items.push_back(Toggle(Tr("Copies XFB vers texture uniquement"), {},
                             Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
    c.items.push_back(Toggle(Tr("Mode XFB immédiat"), {}, Config::GFX_HACK_IMMEDIATE_XFB));
    c.items.push_back(Toggle(Tr("Ignorer les images XFB en double"), {},
                             Config::GFX_HACK_SKIP_DUPLICATE_XFBS));
    c.items.push_back(Toggle(Tr("Copies EFB à l'échelle"), {}, Config::GFX_HACK_COPY_EFB_SCALED));
    c.items.push_back(Toggle(Tr("Échantillonnage rapide des textures"), {},
                             Config::GFX_HACK_FAST_TEXTURE_SAMPLING));
    c.items.push_back(Toggle(Tr("Arrondi des sommets"), {}, Config::GFX_HACK_VERTEX_ROUNDING));
    c.items.push_back(Toggle(Tr("Boîte englobante (bounding box)"), {},
                             Config::GFX_HACK_BBOX_ENABLE));
    c.items.push_back(Toggle(Tr("Calcul de profondeur rapide"), {},
                             Config::GFX_FAST_DEPTH_CALC));
    cats.push_back(std::move(c));
  }

  // ---- Audio
  {
    BigPictureSettingCategory c{Tr("Son"), {}};
    c.items.push_back(IntRange(Tr("Volume"), {}, Config::MAIN_AUDIO_VOLUME, 0, 100, 5,
                               [](int v) { return QStringLiteral("%1 %").arg(v); }));
    c.items.push_back(Toggle(Tr("Couper le son"), {}, Config::MAIN_AUDIO_MUTED));
    std::vector<std::pair<std::string, QString>> backends;
    for (const std::string& b : AudioCommon::GetSoundBackends())
      backends.emplace_back(b, QString::fromStdString(b));
    c.items.push_back(
        Choice<std::string>(Tr("Moteur audio"), {}, Config::MAIN_AUDIO_BACKEND, backends));
    c.items.push_back(Toggle(Tr("Émulation DSP rapide (HLE)"),
                             Tr("Désactivé = LLE, plus précis mais plus lourd."),
                             Config::MAIN_DSP_HLE));
    c.items.push_back(Toggle(Tr("Préserver la hauteur du son"),
                             Tr("Évite que le son change de ton quand le jeu ralentit."),
                             Config::MAIN_AUDIO_PRESERVE_PITCH));
    c.items.push_back(Toggle(Tr("Combler les coupures"), {}, Config::MAIN_AUDIO_FILL_GAPS));
    c.items.push_back(Toggle(Tr("Dolby Pro Logic II (5.1)"), {}, Config::MAIN_DPL2_DECODER));
    c.items.push_back(Toggle(Tr("Couper le son en vitesse illimitée"), {},
                             Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT));
    cats.push_back(std::move(c));
  }

  // ---- GameCube
  {
    BigPictureSettingCategory c{Tr("GameCube"), {}};
    c.items.push_back(Choice<int>(Tr("Langue du système"), Tr("Pour les jeux européens."),
                                  Config::MAIN_GC_LANGUAGE,
                                  {{0, Tr("Anglais")},
                                   {1, Tr("Allemand")},
                                   {2, Tr("Français")},
                                   {3, Tr("Espagnol")},
                                   {4, Tr("Italien")},
                                   {5, Tr("Néerlandais")}}));
    c.items.push_back(Toggle(Tr("Passer le menu de démarrage (IPL)"), {}, Config::MAIN_SKIP_IPL));
    using EXI = ExpansionInterface::EXIDeviceType;
    const std::vector<std::pair<EXI, QString>> exi_slots = {
        {EXI::None, Tr("Vide")},
        {EXI::MemoryCard, Tr("Carte mémoire")},
        {EXI::MemoryCardFolder, Tr("Dossier GCI")}};
    c.items.push_back(Choice<EXI>(Tr("Emplacement A"), {}, Config::MAIN_SLOT_A, exi_slots));
    c.items.push_back(Choice<EXI>(Tr("Emplacement B"), {}, Config::MAIN_SLOT_B, exi_slots));
    c.items.push_back(Toggle(Tr("Lecture rapide du disque"), Tr("Chargements plus courts."),
                             Config::MAIN_FAST_DISC_SPEED));
    const auto memcards = host.open_memory_cards;
    c.items.push_back(Action(Tr("Gestionnaire des cartes mémoire"),
                             Tr("Copier, déplacer ou effacer les sauvegardes."),
                             [memcards] { memcards(); }));
    cats.push_back(std::move(c));
  }

  // ---- Wii
  {
    BigPictureSettingCategory c{Tr("Wii"), {}};
    c.items.push_back(Choice<u32>(Tr("Langue"), {}, Config::SYSCONF_LANGUAGE,
                                  {{0, Tr("Japonais")},
                                   {1, Tr("Anglais")},
                                   {2, Tr("Allemand")},
                                   {3, Tr("Français")},
                                   {4, Tr("Espagnol")},
                                   {5, Tr("Italien")},
                                   {6, Tr("Néerlandais")},
                                   {7, Tr("Chinois simplifié")},
                                   {8, Tr("Chinois traditionnel")},
                                   {9, Tr("Coréen")}}));
    c.items.push_back(Toggle(Tr("Format 16:9"), {}, Config::SYSCONF_WIDESCREEN));
    c.items.push_back(Toggle(Tr("Balayage progressif"), {}, Config::SYSCONF_PROGRESSIVE_SCAN));
    c.items.push_back(Toggle(Tr("Mode PAL60"), {}, Config::SYSCONF_PAL60));
    c.items.push_back(Toggle(Tr("Économiseur d'écran"), {}, Config::SYSCONF_SCREENSAVER));
    c.items.push_back(Choice<u32>(Tr("Position de la sensor bar"), {},
                                  Config::SYSCONF_SENSOR_BAR_POSITION,
                                  {{0, Tr("Sous l'écran")}, {1, Tr("Au-dessus de l'écran")}}));
    c.items.push_back(Choice<u32>(Tr("Sensibilité IR"), {}, Config::SYSCONF_SENSOR_BAR_SENSITIVITY,
                                  {{1, QStringLiteral("1")},
                                   {2, QStringLiteral("2")},
                                   {3, QStringLiteral("3")},
                                   {4, QStringLiteral("4")},
                                   {5, QStringLiteral("5")}}));
    c.items.push_back(Toggle(Tr("Vibreur des Wiimotes"), {}, Config::SYSCONF_WIIMOTE_MOTOR));
    c.items.push_back(Toggle(Tr("Carte SD"), {}, Config::MAIN_WII_SD_CARD));
    c.items.push_back(Toggle(Tr("Clavier USB"), {}, Config::MAIN_WII_KEYBOARD));
    cats.push_back(std::move(c));
  }

  // ---- Controllers
  {
    BigPictureSettingCategory c{Tr("Manettes"), {}};
    auto port = std::make_shared<int>(0);
    const auto pad = [port] { return Pad::GetConfig()->GetController(*port); };

    {
      Item item;
      item.label = Tr("Manette à régler");
      item.help = Tr("Les réglages ci-dessous s'appliquent à ce port GameCube.");
      item.kind = Item::Kind::Choice;
      item.value = [port] { return Tr("Port %1").arg(*port + 1); };
      item.change = [port](int dir) { *port = (*port + dir + 4) % 4; };
      item.activate = [port] { *port = (*port + 1) % 4; };
      c.items.push_back(std::move(item));
    }
    {
      using SI = SerialInterface::SIDevices;
      const std::vector<std::pair<SI, QString>> types = {
          {SI::SIDEVICE_NONE, Tr("Aucun")},
          {SI::SIDEVICE_GC_CONTROLLER, Tr("Manette standard")},
          {SI::SIDEVICE_WIIU_ADAPTER, Tr("Adaptateur GameCube")},
          {SI::SIDEVICE_GC_KEYBOARD, Tr("Clavier GameCube")},
          {SI::SIDEVICE_GC_GBA_EMULATED, Tr("Game Boy Advance")}};
      Item item;
      item.label = Tr("Type d'appareil branché");
      item.kind = Item::Kind::Choice;
      item.value = [port, types] {
        const SI cur = Config::Get(Config::GetInfoForSIDevice(*port));
        for (const auto& [v, n] : types)
        {
          if (v == cur)
            return n;
        }
        return Tr("Autre");
      };
      item.change = [port, types](int dir) {
        const SI cur = Config::Get(Config::GetInfoForSIDevice(*port));
        int i = 0;
        for (size_t j = 0; j < types.size(); ++j)
        {
          if (types[j].first == cur)
            i = static_cast<int>(j);
        }
        const int n = static_cast<int>(types.size());
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(*port), types[(i + dir + n) % n].first);
      };
      item.activate = [change = item.change] { change(1); };
      c.items.push_back(std::move(item));
    }
    {
      Item item;
      item.label = Tr("Manette utilisée");
      item.help = Tr("Choisis parmi les manettes connectées (USB, Bluetooth, XInput, SDL…).");
      item.kind = Item::Kind::Choice;
      item.value = [pad] {
        const auto& dq = pad()->GetDefaultDevice();
        QString name = QString::fromStdString(dq.name);
        if (name.size() > 26)
          name = name.left(24) + QStringLiteral("…");
        return name.isEmpty() ? Tr("Aucune") : name;
      };
      item.change = [pad](int dir) {
        std::vector<std::string> devices = g_controller_interface.GetAllDeviceStrings();
        std::erase_if(devices, [](const std::string& d) { return d.rfind("DSUClient", 0) == 0; });
        if (devices.empty())
          return;
        const std::string current = pad()->GetDefaultDevice().ToString();
        int i = -1;
        for (size_t j = 0; j < devices.size(); ++j)
        {
          if (devices[j] == current)
            i = static_cast<int>(j);
        }
        const int n = static_cast<int>(devices.size());
        i = i < 0 ? 0 : (i + dir + n) % n;
        const auto lock = ControllerEmu::EmulatedController::GetStateLock();
        pad()->SetDefaultDevice(devices[i]);
        pad()->UpdateReferences(g_controller_interface);
      };
      item.activate = [change = item.change] { change(1); };
      c.items.push_back(std::move(item));
    }
    c.items.push_back(PrefToggle(Tr("Changement de manette automatique"),
                                 Tr("La dernière manette utilisée devient celle du joueur 1, avec "
                                    "ses propres réglages."),
                                 QStringLiteral("bigpicture/autoswitch"), true, {}));
    {
      const auto test = host.test_controller;
      c.items.push_back(Action(Tr("Tester la manette"),
                               Tr("Aperçu des boutons, des sticks et des gâchettes analogiques."),
                               [test, port] { test(*port); }));
    }
    {
      const auto notify = host.notify;
      c.items.push_back(Action(Tr("Enregistrer le profil de cette manette"),
                               Tr("Ses réglages reviendront dès qu'elle sera de nouveau utilisée."),
                               [port, notify] {
                                 SaveBigPictureDeviceProfile(*port);
                                 if (notify)
                                   notify(Tr("Profil enregistré"));
                               }));
    }
    c.items.push_back(Action(Tr("Profil automatique"),
                             Tr("Manette GameCube (Switch 2, adaptateur), Xbox, PlayStation… : "
                                "tout est réglé d'un coup."),
                             [port] {
                               auto* pad = Pad::GetConfig()->GetController(*port);
                               const std::string d = pad ? pad->GetDefaultDevice().ToString() : "";
                               if (!ApplyGameCubeProfile(*port, d))
                                 ApplyStandardGamepadProfile(*port);
                               SaveBigPicturePads();
                             }));
    const auto configure = host.configure_controller;
    c.items.push_back(Action(Tr("Assistant bouton par bouton"),
                             Tr("Pour les manettes peu courantes."),
                             [configure, port] { configure(*port); }));
#ifdef __linux__
    {
      const auto notify = host.notify;
      c.items.push_back(Action(
          Tr("Autoriser les manettes Nintendo en USB"),
          Tr("Une seule fois : manette GameCube Switch 2, adaptateur GameCube Wii U / Switch. "
             "Le système demande ton mot de passe."),
          [notify] {
            const QString rules = QStringLiteral(
                "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"057e\", ATTRS{idProduct}==\"0337\", MODE=\"0666\"\n"
                "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"057e\", ATTRS{idProduct}==\"20[67][0-9a-f]\", MODE=\"0666\"\n"
                "KERNEL==\"hidraw*\", ATTRS{idVendor}==\"057e\", ATTRS{idProduct}==\"20[67][0-9a-f]\", MODE=\"0666\"\n"
                "KERNEL==\"hidraw*\", ATTRS{idVendor}==\"057e\", ATTRS{idProduct}==\"0337\", MODE=\"0666\"\n");
            QProcess p;
            p.start(QStringLiteral("pkexec"),
                    {QStringLiteral("sh"), QStringLiteral("-c"),
                     QStringLiteral("cat > /etc/udev/rules.d/51-dolphin-manette.rules && "
                                    "udevadm control --reload-rules && udevadm trigger")});
            p.waitForStarted();
            p.write(rules.toUtf8());
            p.closeWriteChannel();
            p.waitForFinished(120000);
            const bool ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
            if (ok)
              g_controller_interface.RefreshDevices();
            if (notify)
              notify(ok ? Tr("Manettes autorisées : rebranche-les") : Tr("Autorisation refusée"));
          }));
    }
#endif
#ifdef _WIN32
    {
      const auto install = host.install_drivers;
      c.items.push_back(Action(Tr("Pilote USB des manettes Switch 2"),
                               Tr("Une seule fois, pour brancher la manette GameCube Switch 2 en "
                                  "USB (remplace Zadig). Windows demande une confirmation."),
                               [install] { install(); }));
    }
#endif
    {
      const auto notify = host.notify;
      Item bt = Action(
          Tr("Manettes Switch 2 en Bluetooth"),
          Tr("GameCube, Pro 2 et Joy-Con 2 : appuie sur le petit bouton d'appairage de la "
             "manette, elle se connecte toute seule. Ne l'appaire pas dans Windows."),
          [notify] {
            if (notify)
              notify(Tr("Appuie sur le bouton d'appairage de la manette"));
          });
#ifdef _WIN32
      bt.value = [] { return QString::fromStdString(ciface::Switch2BLE::GetStatus()); };
#endif
      c.items.push_back(std::move(bt));
    }

    // Individual controls
    struct Ctl
    {
      PadGroup group;
      int index;
      int mirror;
      bool flip;
      const char* label;
    };
    static constexpr Ctl controls[] = {
        {PadGroup::Buttons, 0, -1, false, "Bouton A"},
        {PadGroup::Buttons, 1, -1, false, "Bouton B"},
        {PadGroup::Buttons, 2, -1, false, "Bouton X"},
        {PadGroup::Buttons, 3, -1, false, "Bouton Y"},
        {PadGroup::Buttons, 4, -1, false, "Bouton Z"},
        {PadGroup::Buttons, 5, -1, false, "START"},
        {PadGroup::MainStick, 0, 1, true, "Stick principal : haut / bas"},
        {PadGroup::MainStick, 3, 2, true, "Stick principal : droite / gauche"},
        {PadGroup::CStick, 0, 1, true, "Stick C : haut / bas"},
        {PadGroup::CStick, 3, 2, true, "Stick C : droite / gauche"},
        {PadGroup::Triggers, 2, 0, false, "Gâchette L"},
        {PadGroup::Triggers, 3, 1, false, "Gâchette R"},
        {PadGroup::DPad, 0, -1, false, "Croix : haut"},
        {PadGroup::DPad, 1, -1, false, "Croix : bas"},
        {PadGroup::DPad, 2, -1, false, "Croix : gauche"},
        {PadGroup::DPad, 3, -1, false, "Croix : droite"},
    };
    const auto remap = host.remap_control;
    for (const Ctl& ctl : controls)
    {
      Item item;
      item.label = Tr(ctl.label);
      item.help = Tr("A : appuie ensuite sur la touche voulue. X : effacer.");
      item.kind = Item::Kind::Action;
      item.value = [port, ctl] {
        std::string expr =
            Pad::GetGroup(*port, ctl.group)->controls[ctl.index]->control_ref->GetExpression();
        std::erase(expr, '`');
        QString q = QString::fromStdString(expr);
        if (q.size() > 22)
          q = q.left(20) + QStringLiteral("…");
        return q.isEmpty() ? Tr("—") : q;
      };
      item.activate = [remap, port, ctl] {
        remap(*port, static_cast<int>(ctl.group), ctl.index, ctl.mirror, ctl.flip,
              Tr(ctl.label));
      };
      item.change = [](int) {};
      item.clear = [port, ctl] {
        const auto lock = ControllerEmu::EmulatedController::GetStateLock();
        auto* group = Pad::GetGroup(*port, ctl.group);
        (void)group->controls[ctl.index]->control_ref->SetExpression("");
        if (ctl.mirror >= 0)
          (void)group->controls[ctl.mirror]->control_ref->SetExpression("");
        Pad::GetConfig()->GetController(*port)->UpdateReferences(g_controller_interface);
      };
      c.items.push_back(std::move(item));
    }

    // Calibration and dead zones
    const auto calibrate = host.calibrate_stick;
    c.items.push_back(Action(Tr("Étalonner le stick principal"),
                             Tr("Fais tourner le stick doucement le long du bord, plusieurs fois."),
                             [calibrate, port] {
                               calibrate(*port, static_cast<int>(PadGroup::MainStick));
                             }));
    c.items.push_back(Action(Tr("Étalonner le stick C"),
                             Tr("Fais tourner le stick doucement le long du bord, plusieurs fois."),
                             [calibrate, port] {
                               calibrate(*port, static_cast<int>(PadGroup::CStick));
                             }));
    const auto numeric = [port](PadGroup group, const char* ini_name, const QString& label,
                                const QString& help) {
      Item item;
      item.label = label;
      item.help = help;
      item.kind = Item::Kind::Choice;
      const auto find = [port, group, ini_name]() -> ControllerEmu::NumericSetting<double>* {
        for (auto& setting : Pad::GetGroup(*port, group)->numeric_settings)
        {
          if (std::string_view(setting->GetININame()) == ini_name &&
              setting->GetType() == ControllerEmu::SettingType::Double)
          {
            return static_cast<ControllerEmu::NumericSetting<double>*>(setting.get());
          }
        }
        return nullptr;
      };
      item.value = [find] {
        const auto* s = find();
        return s ? QStringLiteral("%1 %").arg(static_cast<int>(std::lround(s->GetValue()))) :
                   Tr("—");
      };
      item.change = [find](int dir) {
        if (auto* s = find())
        {
          const auto lock = ControllerEmu::EmulatedController::GetStateLock();
          s->SetValue(std::clamp(std::round(s->GetValue()) + dir * 1.0, 0.0, 50.0));
        }
      };
      item.activate = [] {};
      return item;
    };
    c.items.push_back(numeric(PadGroup::MainStick, "Dead Zone", Tr("Zone morte du stick principal"),
                              Tr("Ignore les petits mouvements autour du centre (stick qui "
                                 "dérive).")));
    c.items.push_back(numeric(PadGroup::CStick, "Dead Zone", Tr("Zone morte du stick C"), {}));
    c.items.push_back(numeric(PadGroup::Triggers, "Dead Zone", Tr("Zone morte des gâchettes"),
                              {}));
    {
      Item item;
      item.label = Tr("Seuil des gâchettes (clic L/R)");
      item.help = Tr("À partir de quelle pression les gâchettes analogiques comptent comme "
                     "un clic complet.");
      item.kind = Item::Kind::Choice;
      const auto find = [port]() -> ControllerEmu::NumericSetting<double>* {
        for (auto& setting : Pad::GetGroup(*port, PadGroup::Triggers)->numeric_settings)
        {
          if (std::string_view(setting->GetININame()) == "Threshold" &&
              setting->GetType() == ControllerEmu::SettingType::Double)
          {
            return static_cast<ControllerEmu::NumericSetting<double>*>(setting.get());
          }
        }
        return nullptr;
      };
      item.value = [find] {
        const auto* st = find();
        return st ? QStringLiteral("%1 %").arg(static_cast<int>(std::lround(st->GetValue()))) :
                    Tr("—");
      };
      item.change = [find](int dir) {
        if (auto* st = find())
        {
          const auto lock = ControllerEmu::EmulatedController::GetStateLock();
          st->SetValue(std::clamp(std::round(st->GetValue()) + dir * 5.0, 50.0, 100.0));
        }
      };
      item.activate = [] {};
      c.items.push_back(std::move(item));
    }
    {
      Item item;
      item.label = Tr("Vibrations");
      item.kind = Item::Kind::Toggle;
      item.is_on = [port] {
        return !Pad::GetGroup(*port, PadGroup::Rumble)
                    ->controls[0]
                    ->control_ref->GetExpression()
                    .empty();
      };
      item.change = [port](int) {
        auto& ref = Pad::GetGroup(*port, PadGroup::Rumble)->controls[0]->control_ref;
        const auto lock = ControllerEmu::EmulatedController::GetStateLock();
        (void)ref->SetExpression(ref->GetExpression().empty() ? "`Motor`" : "");
        Pad::GetConfig()->GetController(*port)->UpdateReferences(g_controller_interface);
      };
      item.activate = [change = item.change] { change(1); };
      c.items.push_back(std::move(item));
    }
    cats.push_back(std::move(c));
  }

  // ---- Wii controllers
  for (auto& wc : BuildBigPictureWiiControllers(host))
    cats.push_back(std::move(wc));

  // ---- Advanced
  {
    BigPictureSettingCategory c{Tr("Avancé"), {}};
    std::vector<std::pair<PowerPC::CPUCore, QString>> cores;
    for (const PowerPC::CPUCore core : PowerPC::AvailableCPUCores())
    {
      switch (core)
      {
      case PowerPC::CPUCore::Interpreter:
        cores.emplace_back(core, Tr("Interpréteur (très lent)"));
        break;
      case PowerPC::CPUCore::CachedInterpreter:
        cores.emplace_back(core, Tr("Interpréteur avec cache"));
        break;
      case PowerPC::CPUCore::JIT64:
        cores.emplace_back(core, Tr("JIT x86-64 (recommandé)"));
        break;
      case PowerPC::CPUCore::JITARM64:
        cores.emplace_back(core, Tr("JIT ARM64 (recommandé)"));
        break;
      }
    }
    c.items.push_back(Choice<PowerPC::CPUCore>(Tr("Moteur d'émulation du processeur"), {},
                                               Config::MAIN_CPU_CORE, std::move(cores)));
    c.items.push_back(Toggle(Tr("Overclock du processeur"),
                             Tr("Peut fluidifier certains jeux, ou en casser d'autres."),
                             Config::MAIN_OVERCLOCK_ENABLE));
    c.items.push_back(Choice<float>(Tr("Niveau d'overclock"), {}, Config::MAIN_OVERCLOCK,
                                    {{0.5f, Percent(0.5f)},
                                     {0.75f, Percent(0.75f)},
                                     {1.0f, Percent(1.0f)},
                                     {1.5f, Percent(1.5f)},
                                     {2.0f, Percent(2.0f)},
                                     {3.0f, Percent(3.0f)},
                                     {4.0f, Percent(4.0f)}}));
    c.items.push_back(Toggle(Tr("MMU"), Tr("Requis par quelques jeux. Plus lent."),
                             Config::MAIN_MMU));
    c.items.push_back(Toggle(Tr("Synchroniser le GPU"), {}, Config::MAIN_SYNC_GPU));
    c.items.push_back(Toggle(Tr("Cache CPU précis"), {}, Config::MAIN_ACCURATE_CPU_CACHE));
    c.items.push_back(Toggle(Tr("Charger le jeu en mémoire"), {},
                             Config::MAIN_LOAD_GAME_INTO_MEMORY));
    const auto open_classic = host.open_classic;
    c.items.push_back(Action(Tr("Interface classique"),
                             Tr("Pour les réglages très pointus et les outils de débogage."),
                             [open_classic] { open_classic(); }));
    cats.push_back(std::move(c));
  }

  return cats;
}

// ---------------------------------------------------------------- standard gamepad profile

bool ApplyStandardGamepadProfile(int port, const std::string& forced_device)
{
  auto* pad = Pad::GetConfig()->GetController(port);
  if (!pad)
    return false;
  const auto is_gamepad = [](const std::string& d) {
    return d.rfind("SDL/", 0) == 0 || d.rfind("XInput/", 0) == 0 ||
           d.rfind("WGInput/", 0) == 0 || d.rfind("evdev/", 0) == 0 || d.rfind("Switch2/", 0) == 0;
  };
  std::string device = forced_device.empty() ? pad->GetDefaultDevice().ToString() : forced_device;
  if (forced_device.empty() &&
      (!is_gamepad(device) || !g_controller_interface.FindDevice(pad->GetDefaultDevice())))
  {
    device.clear();
    for (const std::string& d : g_controller_interface.GetAllDeviceStrings())
    {
      if (is_gamepad(d))
      {
        device = d;
        break;
      }
    }
  }
  if (device.empty())
    return false;

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  pad->SetDefaultDevice(device);
  const auto set = [port](PadGroup group, int index, const char* expr) {
    (void)Pad::GetGroup(port, group)->controls[index]->control_ref->SetExpression(expr);
  };
  set(PadGroup::Buttons, 0, "`Button A`");
  set(PadGroup::Buttons, 1, "`Button B`");
  set(PadGroup::Buttons, 2, "`Button X`");
  set(PadGroup::Buttons, 3, "`Button Y`");
  set(PadGroup::Buttons, 4, "`Shoulder R`");
  set(PadGroup::Buttons, 5, "`Start`");
  set(PadGroup::MainStick, 0, "`Left Y+`");
  set(PadGroup::MainStick, 1, "`Left Y-`");
  set(PadGroup::MainStick, 2, "`Left X-`");
  set(PadGroup::MainStick, 3, "`Left X+`");
  set(PadGroup::CStick, 0, "`Right Y+`");
  set(PadGroup::CStick, 1, "`Right Y-`");
  set(PadGroup::CStick, 2, "`Right X-`");
  set(PadGroup::CStick, 3, "`Right X+`");
  set(PadGroup::Triggers, 0, "`Trigger L`");
  set(PadGroup::Triggers, 1, "`Trigger R`");
  set(PadGroup::Triggers, 2, "`Trigger L`");
  set(PadGroup::Triggers, 3, "`Trigger R`");
  set(PadGroup::DPad, 0, "`Pad N`");
  set(PadGroup::DPad, 1, "`Pad S`");
  set(PadGroup::DPad, 2, "`Pad W`");
  set(PadGroup::DPad, 3, "`Pad E`");
  set(PadGroup::Rumble, 0, device.rfind("XInput/", 0) == 0 ? "`Motor L`" : "`Motor`");
  pad->UpdateReferences(g_controller_interface);
  Pad::GetConfig()->SaveConfig();
  return true;
}

// ---------------------------------------------------------------- per-game settings

namespace
{
template <typename T>
Item GameChoice(const std::shared_ptr<Config::Layer>& layer, const QString& label,
                const QString& help, const Config::Info<T>& info,
                std::vector<std::pair<T, QString>> options)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  const auto* p = &info;
  auto opts = std::make_shared<std::vector<std::pair<T, QString>>>(std::move(options));
  const auto name_of = [opts](const T& v) {
    for (const auto& [value, name] : *opts)
    {
      if (value == v)
        return name;
    }
    return Tr("Personnalisé");
  };
  // -1 = follows the global setting
  const auto index = [layer, p, opts]() -> int {
    const std::optional<T> v = layer->Get<T>(p->GetLocation());
    if (!v)
      return -1;
    for (size_t i = 0; i < opts->size(); ++i)
    {
      if ((*opts)[i].first == *v)
        return static_cast<int>(i);
    }
    return 0;
  };
  item.value = [layer, p, name_of, index] {
    const int i = index();
    if (i < 0)
      return Tr("Global (%1)").arg(name_of(Config::Get(*p)));
    return name_of(*layer->Get<T>(p->GetLocation()));
  };
  item.change = [layer, p, opts, index](int dir) {
    const int n = static_cast<int>(opts->size()) + 1;
    const int i = (index() + 1 + dir + n) % n - 1;
    if (i < 0)
      (void)layer->DeleteKey(p->GetLocation());
    else
      (void)layer->Set(*p, (*opts)[i].first);
  };
  item.activate = [change = item.change] { change(1); };
  return item;
}

Item GameBool(const std::shared_ptr<Config::Layer>& layer, const QString& label,
              const QString& help, const Config::Info<bool>& info, bool inverted = false)
{
  return GameChoice<bool>(layer, label, help, info,
                          {{!inverted, Tr("Activé")}, {inverted, Tr("Désactivé")}});
}
}  // namespace

std::vector<BigPictureSettingCategory>
BuildBigPictureGameSettings(const std::shared_ptr<Config::Layer>& layer)
{
  std::vector<BigPictureSettingCategory> cats;
  {
    BigPictureSettingCategory c{Tr("Graphismes"), {}};
    c.items.push_back(GameChoice<int>(layer, Tr("Résolution interne"), {}, Config::GFX_EFB_SCALE,
                                      {{0, Tr("Auto (fenêtre)")},
                                       {1, Tr("Native (480p)")},
                                       {2, Tr("2× (720p)")},
                                       {3, Tr("3× (1080p)")},
                                       {4, Tr("4× (1440p)")},
                                       {5, Tr("5× (1800p)")},
                                       {6, Tr("6× (4K)")},
                                       {8, Tr("8× (5K)")}}));
    c.items.push_back(GameChoice<u32>(layer, Tr("Anticrénelage (MSAA)"), {}, Config::GFX_MSAA,
                                      {{1, Tr("Aucun")},
                                       {2, Tr("2×")},
                                       {4, Tr("4×")},
                                       {8, Tr("8×")}}));
    c.items.push_back(GameBool(layer, Tr("Suréchantillonnage (SSAA)"),
                               Tr("Avec l'anticrénelage : bien plus beau, bien plus lourd."),
                               Config::GFX_SSAA));
    c.items.push_back(GameChoice<AnisotropicFilteringMode>(
        layer, Tr("Filtrage anisotrope"), {}, Config::GFX_ENHANCE_MAX_ANISOTROPY,
        {{AnisotropicFilteringMode::Default, Tr("Par défaut du jeu")},
         {AnisotropicFilteringMode::Force2x, Tr("2×")},
         {AnisotropicFilteringMode::Force4x, Tr("4×")},
         {AnisotropicFilteringMode::Force8x, Tr("8×")},
         {AnisotropicFilteringMode::Force16x, Tr("16×")}}));
    c.items.push_back(GameChoice<AspectMode>(layer, Tr("Format d'image"), {},
                                             Config::GFX_ASPECT_RATIO,
                                             {{AspectMode::Auto, Tr("Auto")},
                                              {AspectMode::ForceWide, Tr("16:9 forcé")},
                                              {AspectMode::ForceStandard, Tr("4:3 forcé")},
                                              {AspectMode::Stretch, Tr("Étirer")}}));
    c.items.push_back(GameBool(layer, Tr("Écran large forcé (hack)"), {},
                               Config::GFX_WIDESCREEN_HACK));
    c.items.push_back(GameChoice<ShaderCompilationMode>(
        layer, Tr("Compilation des shaders"), {}, Config::GFX_SHADER_COMPILATION_MODE,
        {{ShaderCompilationMode::Synchronous, Tr("Synchrone")},
         {ShaderCompilationMode::SynchronousUberShaders, Tr("Ubershaders (synchrone)")},
         {ShaderCompilationMode::AsynchronousUberShaders, Tr("Ubershaders (asynchrone)")},
         {ShaderCompilationMode::AsynchronousSkipRendering, Tr("Asynchrone")}}));
    c.items.push_back(GameChoice<TextureFilteringMode>(
        layer, Tr("Filtrage des textures"), {}, Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
        {{TextureFilteringMode::Default, Tr("Par défaut")},
         {TextureFilteringMode::Nearest, Tr("Plus proche")},
         {TextureFilteringMode::Linear, Tr("Linéaire")}}));
    c.items.push_back(GameBool(layer, Tr("Éclairage par pixel"), {},
                               Config::GFX_ENABLE_PIXEL_LIGHTING));
    c.items.push_back(GameBool(layer, Tr("Couleurs 24 bits forcées"), {},
                               Config::GFX_ENHANCE_FORCE_TRUE_COLOR));
    c.items.push_back(GameBool(layer, Tr("Désactiver le filtre de copie"), {},
                               Config::GFX_ENHANCE_DISABLE_COPY_FILTER));
    c.items.push_back(GameBool(layer, Tr("Désactiver le brouillard"), {}, Config::GFX_DISABLE_FOG));
    c.items.push_back(GameBool(layer, Tr("Textures HD personnalisées"), {},
                               Config::GFX_HIRES_TEXTURES));
    cats.push_back(std::move(c));
  }
  {
    BigPictureSettingCategory c{Tr("Hacks graphiques"), {}};
    c.items.push_back(GameChoice<int>(layer, Tr("Précision du cache de textures"), {},
                                      Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                                      {{0, Tr("Sûr")}, {512, Tr("Moyen")}, {128, Tr("Rapide")}}));
    c.items.push_back(GameBool(layer, Tr("Ignorer l'accès EFB depuis le CPU"), {},
                               Config::GFX_HACK_EFB_ACCESS_ENABLE, true));
    c.items.push_back(GameBool(layer, Tr("Ignorer les changements de format"), {},
                               Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true));
    c.items.push_back(GameBool(layer, Tr("Copies EFB vers texture uniquement"), {},
                               Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
    c.items.push_back(GameBool(layer, Tr("Copies XFB vers texture uniquement"), {},
                               Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
    c.items.push_back(GameBool(layer, Tr("Mode XFB immédiat"), {}, Config::GFX_HACK_IMMEDIATE_XFB));
    c.items.push_back(GameBool(layer, Tr("Copies EFB à l'échelle"), {},
                               Config::GFX_HACK_COPY_EFB_SCALED));
    c.items.push_back(GameBool(layer, Tr("Arrondi des sommets"), {},
                               Config::GFX_HACK_VERTEX_ROUNDING));
    c.items.push_back(GameBool(layer, Tr("Boîte englobante (bounding box)"), {},
                               Config::GFX_HACK_BBOX_ENABLE));
    cats.push_back(std::move(c));
  }
  {
    BigPictureSettingCategory c{Tr("Émulation"), {}};
    c.items.push_back(GameBool(layer, Tr("Double cœur"), {}, Config::MAIN_CPU_THREAD));
    c.items.push_back(GameBool(layer, Tr("MMU"), {}, Config::MAIN_MMU));
    c.items.push_back(GameBool(layer, Tr("Synchroniser le GPU"), {}, Config::MAIN_SYNC_GPU));
    c.items.push_back(GameBool(layer, Tr("Émulation DSP rapide (HLE)"), {}, Config::MAIN_DSP_HLE));
    c.items.push_back(GameBool(layer, Tr("Lecture rapide du disque"), {},
                               Config::MAIN_FAST_DISC_SPEED));
    c.items.push_back(GameBool(layer, Tr("Overclock du processeur"), {},
                               Config::MAIN_OVERCLOCK_ENABLE));
    c.items.push_back(GameChoice<float>(layer, Tr("Niveau d'overclock"), {}, Config::MAIN_OVERCLOCK,
                                        {{0.5f, Percent(0.5f)},
                                         {1.0f, Percent(1.0f)},
                                         {1.5f, Percent(1.5f)},
                                         {2.0f, Percent(2.0f)},
                                         {3.0f, Percent(3.0f)}}));
    cats.push_back(std::move(c));
  }
  return cats;
}


// ---------------------------------------------------------------- device profiles

std::string BigPictureProfilePath(const std::string& device_name)
{
  std::string safe;
  for (const char ch : device_name)
  {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == ' ' || ch == '-' || ch == '_';
    safe += ok ? ch : '_';
  }
  return File::GetUserPath(D_CONFIG_IDX) + "Profiles/GCPad/Mode manette - " + safe + ".ini";
}

void SaveBigPictureDeviceProfile(int port)
{
  auto* pad = Pad::GetConfig()->GetController(port);
  if (!pad)
    return;
  const auto& dq = pad->GetDefaultDevice();
  if (dq.name.empty() || dq.source == "DSUClient" || dq.name.find("Keyboard") != std::string::npos)
    return;
  const std::string path = BigPictureProfilePath(dq.name);
  File::CreateFullPath(path);
  Common::IniFile ini;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    pad->SaveConfig(ini.GetOrCreateSection("Profile"));
  }
  ini.Save(path);
}

bool LoadBigPictureDeviceProfile(int port, const std::string& device)
{
  auto* pad = Pad::GetConfig()->GetController(port);
  if (!pad)
    return false;
  ciface::Core::DeviceQualifier dq;
  dq.FromString(device);
  const std::string path = BigPictureProfilePath(dq.name);
  Common::IniFile ini;
  if (!File::Exists(path) || !ini.Load(path))
    return false;
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  pad->LoadConfig(ini.GetOrCreateSection("Profile"));
  // The profile may name another instance of the same model (e.g. SDL/1/...).
  pad->SetDefaultDevice(dq);
  pad->UpdateReferences(g_controller_interface);
  return true;
}

// ---------------------------------------------------------------- quick settings (pause)

std::vector<BigPictureSettingCategory> BuildBigPictureQuickSettings()
{
  BigPictureSettingCategory c{Tr("Réglages rapides"), {}};
  c.items.push_back(Choice<int>(Tr("Résolution interne"), {}, Config::GFX_EFB_SCALE,
                                {{1, Tr("Native (480p)")},
                                 {2, Tr("2× (720p)")},
                                 {3, Tr("3× (1080p)")},
                                 {4, Tr("4× (1440p)")},
                                 {5, Tr("5× (1800p)")},
                                 {6, Tr("6× (4K)")},
                                 {8, Tr("8× (5K)")}}));
  c.items.push_back(Choice<u32>(Tr("Anticrénelage (MSAA)"), {}, Config::GFX_MSAA,
                                {{1, Tr("Aucun")}, {2, Tr("2×")}, {4, Tr("4×")}, {8, Tr("8×")}}));
  c.items.push_back(Choice<AspectMode>(Tr("Format d'image"), {}, Config::GFX_ASPECT_RATIO,
                                       {{AspectMode::Auto, Tr("Auto")},
                                        {AspectMode::ForceWide, Tr("16:9 forcé")},
                                        {AspectMode::ForceStandard, Tr("4:3 forcé")},
                                        {AspectMode::Stretch, Tr("Étirer")}}));
  c.items.push_back(Toggle(Tr("Écran large forcé (hack)"), {}, Config::GFX_WIDESCREEN_HACK));
  c.items.push_back(Choice<float>(Tr("Vitesse d'émulation"), {}, Config::MAIN_EMULATION_SPEED,
                                  {{0.5f, Percent(0.5f)},
                                   {1.0f, Percent(1.0f)},
                                   {1.5f, Percent(1.5f)},
                                   {2.0f, Percent(2.0f)},
                                   {0.0f, Tr("Illimitée")}}));
  c.items.push_back(Toggle(Tr("Afficher les FPS"), {}, Config::GFX_SHOW_FPS));
  c.items.push_back(IntRange(Tr("Volume"), {}, Config::MAIN_AUDIO_VOLUME, 0, 100, 5,
                             [](int v) { return QStringLiteral("%1 %").arg(v); }));
  c.items.push_back(Toggle(Tr("Couper le son"), {}, Config::MAIN_AUDIO_MUTED));
  std::vector<BigPictureSettingCategory> cats;
  cats.push_back(std::move(c));
  return cats;
}

// ---------------------------------------------------------------- cheats

std::vector<BigPictureSettingCategory> BuildBigPictureCheatSettings(const std::string& game_id,
                                                                     u16 revision)
{
  const std::string ini_path = File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
  Common::IniFile local;
  local.Load(ini_path);
  const Common::IniFile def = SConfig::LoadDefaultGameIni(game_id, revision);
  auto gecko = std::make_shared<std::vector<Gecko::GeckoCode>>(Gecko::LoadCodes(def, local));
  auto ar = std::make_shared<std::vector<ActionReplay::ARCode>>(ActionReplay::LoadCodes(def, local));

  const auto save = [ini_path, gecko, ar, game_id, revision] {
    Common::IniFile ini;
    ini.Load(ini_path);
    Gecko::SaveCodes(ini, *gecko);
    ActionReplay::SaveCodes(&ini, *ar);
    File::CreateFullPath(ini_path);
    ini.Save(ini_path);
    // Apply right away if this game is running.
    auto& system = Core::System::GetInstance();
    if (!Core::IsUninitialized(system) && SConfig::GetInstance().GetGameID() == game_id)
    {
      Gecko::SetActiveCodes(*gecko, game_id, revision);
      ActionReplay::ApplyCodes(*ar, game_id, revision);
    }
  };

  std::vector<BigPictureSettingCategory> cats;
  BigPictureSettingCategory g{Tr("Codes Gecko"), {}};
  for (size_t i = 0; i < gecko->size(); ++i)
  {
    Item item;
    item.label = QString::fromStdString((*gecko)[i].name);
    item.help = QString::fromStdString((*gecko)[i].creator);
    item.kind = Item::Kind::Toggle;
    item.is_on = [gecko, i] { return (*gecko)[i].enabled; };
    item.change = [gecko, i, save](int) {
      (*gecko)[i].enabled = !(*gecko)[i].enabled;
      save();
    };
    item.activate = [change = item.change] { change(1); };
    g.items.push_back(std::move(item));
  }
  if (g.items.empty())
    g.items.push_back(Action(Tr("Aucun code Gecko pour ce jeu"), {}, [] {}));
  cats.push_back(std::move(g));

  BigPictureSettingCategory a{Tr("Codes Action Replay"), {}};
  for (size_t i = 0; i < ar->size(); ++i)
  {
    Item item;
    item.label = QString::fromStdString((*ar)[i].name);
    item.kind = Item::Kind::Toggle;
    item.is_on = [ar, i] { return (*ar)[i].enabled; };
    item.change = [ar, i, save](int) {
      (*ar)[i].enabled = !(*ar)[i].enabled;
      save();
    };
    item.activate = [change = item.change] { change(1); };
    a.items.push_back(std::move(item));
  }
  if (a.items.empty())
    a.items.push_back(Action(Tr("Aucun code Action Replay pour ce jeu"), {}, [] {}));
  cats.push_back(std::move(a));

  BigPictureSettingCategory o{Tr("Options des codes"), {}};
  o.items.push_back(Toggle(Tr("Activer les codes de triche"),
                           Tr("Réglage général : nécessaire pour que les codes fonctionnent."),
                           Config::MAIN_ENABLE_CHEATS));
  cats.push_back(std::move(o));
  return cats;
}

void SaveBigPicturePads()
{
  Pad::GetConfig()->SaveConfig();
  Wiimote::GetConfig()->SaveConfig();
  for (int port = 0; port < 4; ++port)
  {
    if (port == 0 || Config::Get(Config::GetInfoForSIDevice(port)) != SerialInterface::SIDEVICE_NONE)
      SaveBigPictureDeviceProfile(port);
  }
}

bool IsSwitch2GameCube(const std::string& device)
{
  return device.find("GameCube") != std::string::npos &&
         (device.rfind("SDL/", 0) == 0 || device.rfind("Switch2/", 0) == 0);
}

bool ApplyGameCubeProfile(int port, const std::string& device)
{
  auto* pad = Pad::GetConfig()->GetController(port);
  if (!pad || !IsSwitch2GameCube(device))
    return false;
  // Names are the same for SDL (USB) and the Bluetooth backend of the controller mode.
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  pad->SetDefaultDevice(device);
  const auto set = [port](PadGroup group, int index, const char* expr) {
    (void)Pad::GetGroup(port, group)->controls[index]->control_ref->SetExpression(expr);
  };
  set(PadGroup::Buttons, 0, "`Button S`");    // A
  set(PadGroup::Buttons, 1, "`Button W`");    // B
  set(PadGroup::Buttons, 2, "`Button E`");    // X
  set(PadGroup::Buttons, 3, "`Button N`");    // Y
  set(PadGroup::Buttons, 4, "`Shoulder R`");  // Z
  set(PadGroup::Buttons, 5, "`Start`");
  set(PadGroup::MainStick, 0, "`Left Y+`");
  set(PadGroup::MainStick, 1, "`Left Y-`");
  set(PadGroup::MainStick, 2, "`Left X-`");
  set(PadGroup::MainStick, 3, "`Left X+`");
  set(PadGroup::CStick, 0, "`Right Y+`");
  set(PadGroup::CStick, 1, "`Right Y-`");
  set(PadGroup::CStick, 2, "`Right X-`");
  set(PadGroup::CStick, 3, "`Right X+`");
  set(PadGroup::Triggers, 0, "`Misc 3`");  // L click (end of travel)
  set(PadGroup::Triggers, 1, "`Misc 4`");  // R click
  set(PadGroup::Triggers, 2, "`Trigger L`");
  set(PadGroup::Triggers, 3, "`Trigger R`");
  set(PadGroup::DPad, 0, "`Pad N`");
  set(PadGroup::DPad, 1, "`Pad S`");
  set(PadGroup::DPad, 2, "`Pad W`");
  set(PadGroup::DPad, 3, "`Pad E`");
  set(PadGroup::Rumble, 0, "`Motor`");
  pad->UpdateReferences(g_controller_interface);
  return true;
}

// ---------------------------------------------------------------- generic control groups

ControllerEmu::EmulatedController* BigPictureController(int port, int code)
{
  if (code / 100 == 0)
    return Pad::GetConfig()->GetController(port);
  return Wiimote::GetConfig()->GetController(port);
}

ControllerEmu::ControlGroup* BigPictureGroup(int port, int code)
{
  const int g = code % 100;
  switch (code / 100)
  {
  case 1:
    return Wiimote::GetWiimoteGroup(port, static_cast<WiimoteEmu::WiimoteGroup>(g));
  case 2:
    return Wiimote::GetNunchukGroup(port, static_cast<WiimoteEmu::NunchukGroup>(g));
  case 3:
    return Wiimote::GetClassicGroup(port, static_cast<WiimoteEmu::ClassicGroup>(g));
  default:
    return Pad::GetGroup(port, static_cast<PadGroup>(g));
  }
}

int BigPictureWiiExtension(int port)
{
  auto* att = static_cast<ControllerEmu::Attachments*>(
      Wiimote::GetWiimoteGroup(port, WiimoteEmu::WiimoteGroup::Attachments));
  return static_cast<int>(att->GetSelectedAttachment());
}

namespace
{
constexpr int W = 100, N = 200, C = 300;
constexpr int WG(WiimoteEmu::WiimoteGroup g)
{
  return W + static_cast<int>(g);
}
constexpr int NG(WiimoteEmu::NunchukGroup g)
{
  return N + static_cast<int>(g);
}
constexpr int CG(WiimoteEmu::ClassicGroup g)
{
  return C + static_cast<int>(g);
}
}  // namespace

namespace
{
ControllerEmu::NumericSetting<bool>* SidewaysSetting(int port)
{
  for (auto& setting :
       Wiimote::GetWiimoteGroup(port, WiimoteEmu::WiimoteGroup::Options)->numeric_settings)
  {
    if (std::string_view(setting->GetININame()) == WiimoteEmu::Wiimote::SIDEWAYS_OPTION)
      return static_cast<ControllerEmu::NumericSetting<bool>*>(setting.get());
  }
  return nullptr;
}
}  // namespace

bool BigPictureWiiSideways(int port)
{
  const auto* setting = SidewaysSetting(port);
  return setting && setting->GetValue();
}

void BigPictureSetWiiSideways(int port, bool sideways)
{
  auto* setting = SidewaysSetting(port);
  if (!setting)
    return;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    setting->SetValue(sideways);
  }
  Wiimote::GetConfig()->SaveConfig();
}

void FinishBigPictureWiiSetup(int port)
{
  auto* wm = Wiimote::GetConfig()->GetController(port);
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    static_cast<ControllerEmu::ReshapableInput*>(
        Wiimote::GetWiimoteGroup(port, WiimoteEmu::WiimoteGroup::Point))
        ->SetCalibrationFromGate(ControllerEmu::RoundStickGate(1.0));
    wm->UpdateReferences(g_controller_interface);
  }
  Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(port), WiimoteSource::Emulated);
  Wiimote::GetConfig()->SaveConfig();
}

bool ApplyWiiProfile(int port, int extension, bool sideways)
{
  using WGr = WiimoteEmu::WiimoteGroup;
  using NGr = WiimoteEmu::NunchukGroup;
  using CGr = WiimoteEmu::ClassicGroup;
  // The Wii Remote uses the controller of the same player.
  auto* gc = Pad::GetConfig()->GetController(port);
  auto* wm = Wiimote::GetConfig()->GetController(port);
  if (!gc || !wm)
    return false;
  const std::string device = gc->GetDefaultDevice().ToString();
  if (device.empty())
    return false;
  const bool gcpad = IsSwitch2GameCube(device);
  const bool xinput = device.rfind("XInput/", 0) == 0;

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  wm->SetDefaultDevice(device);
  for (int code : {WG(WGr::Buttons), WG(WGr::DPad), WG(WGr::Point), WG(WGr::Shake), WG(WGr::Tilt),
                   WG(WGr::Swing), NG(NGr::Buttons), NG(NGr::Stick), NG(NGr::Shake),
                   CG(CGr::Buttons), CG(CGr::Triggers), CG(CGr::DPad), CG(CGr::LeftStick),
                   CG(CGr::RightStick)})
  {
    for (auto& control : BigPictureGroup(port, code)->controls)
      (void)control->control_ref->SetExpression("");
  }
  const auto set = [port](int code, int index, const char* expr) {
    auto* group = BigPictureGroup(port, code);
    if (index < static_cast<int>(group->controls.size()))
      (void)group->controls[index]->control_ref->SetExpression(expr);
  };
  const auto stick = [&set](int code, const char* up, const char* down, const char* left,
                            const char* right) {
    set(code, 0, up);
    set(code, 1, down);
    set(code, 2, left);
    set(code, 3, right);
  };
  if (sideways)
    extension = 0;
  const int dpad = extension == 2 ? CG(CGr::DPad) : WG(WGr::DPad);
  if (sideways)  // the D-pad is the "stick" of a sideways Wii Remote
    stick(dpad, "`Pad N` | `Left Y+`", "`Pad S` | `Left Y-`", "`Pad W` | `Left X-`",
          "`Pad E` | `Left X+`");
  else
    stick(dpad, "`Pad N`", "`Pad S`", "`Pad W`", "`Pad E`");
  set(WG(WGr::Rumble), 0, xinput ? "`Motor L`" : "`Motor`");

  if (extension == 2)
  {
    // Classic Controller: Nintendo layout (A on the right)
    const int b = CG(CGr::Buttons);
    if (gcpad)
    {
      set(b, 0, "`Button S`");  // A
      set(b, 1, "`Button W`");  // B
      set(b, 2, "`Button E`");  // X
      set(b, 3, "`Button N`");  // Y
      set(b, 4, "`Shoulder L`");  // ZL
      set(b, 5, "`Shoulder R`");  // ZR = Z
      set(b, 6, "`Misc 2`");      // - = C
      set(CG(CGr::Triggers), 0, "`Misc 3`");
      set(CG(CGr::Triggers), 1, "`Misc 4`");
      set(CG(CGr::Triggers), 2, "`Trigger L`");
      set(CG(CGr::Triggers), 3, "`Trigger R`");
    }
    else
    {
      set(b, 0, "`Button B`");
      set(b, 1, "`Button A`");
      set(b, 2, "`Button Y`");
      set(b, 3, "`Button X`");
      set(b, 4, "`Trigger L`");
      set(b, 5, "`Trigger R`");
      set(b, 6, "`Back`");
      set(CG(CGr::Triggers), 0, "`Shoulder L`");
      set(CG(CGr::Triggers), 1, "`Shoulder R`");
      set(CG(CGr::Triggers), 2, "`Shoulder L`");
      set(CG(CGr::Triggers), 3, "`Shoulder R`");
    }
    set(b, 7, "`Start`");
    set(b, 8, "`Guide`");
    stick(CG(CGr::LeftStick), "`Left Y+`", "`Left Y-`", "`Left X-`", "`Left X+`");
    stick(CG(CGr::RightStick), "`Right Y+`", "`Right Y-`", "`Right X-`", "`Right X+`");
  }
  else if (sideways)
  {
    // Wii Remote held sideways: 2 and 1 are the main buttons, tilt on the right stick
    const int b = WG(WGr::Buttons);
    set(b, 3, gcpad ? "`Button S`" : "`Button A`");  // 2
    set(b, 2, gcpad ? "`Button W`" : "`Button B`");  // 1
    set(b, 0, gcpad ? "`Button E`" : "`Button X`");  // A
    set(b, 1, gcpad ? "`Button N`" : "`Button Y`");  // B
    set(b, 4, gcpad ? "`Shoulder L`" : "`Back`");    // -
    set(b, 5, "`Start`");
    set(b, 6, "`Guide`");
    for (int i = 0; i < 3; ++i)
      set(WG(WGr::Shake), i, "`Shoulder R`");
    stick(WG(WGr::Tilt), "`Right Y+`", "`Right Y-`", "`Right X-`", "`Right X+`");
  }
  else
  {
    // Wii Remote (+ Nunchuk): pointer on the right stick
    const int b = WG(WGr::Buttons);
    if (gcpad)
    {
      set(b, 0, "`Button S`");    // A
      set(b, 1, "`Trigger R`");   // B = R
      set(b, 2, "`Button E`");    // 1 = X
      set(b, 3, "`Button N`");    // 2 = Y
      set(b, 4, "`Shoulder L`");  // - = ZL
      for (int i = 0; i < 3; ++i)
        set(WG(WGr::Shake), i, "`Button W`");  // shake = B
      set(NG(NGr::Buttons), 0, "`Shoulder R`");  // C = Z
      set(NG(NGr::Buttons), 1, "`Trigger L`");   // Z = L
    }
    else
    {
      set(b, 0, "`Button A`");
      set(b, 1, "`Trigger R`");
      set(b, 2, "`Button X`");
      set(b, 3, "`Button Y`");
      set(b, 4, "`Back`");
      for (int i = 0; i < 3; ++i)
        set(WG(WGr::Shake), i, "`Button B`");
      set(NG(NGr::Buttons), 0, "`Shoulder L`");
      set(NG(NGr::Buttons), 1, "`Trigger L`");
      for (int i = 0; i < 3; ++i)
        set(NG(NGr::Shake), i, "`Thumb L`");
      set(WG(WGr::Point), 5, "`Thumb R`");  // recenter
    }
    set(b, 5, "`Start`");
    set(b, 6, "`Guide`");
    stick(WG(WGr::Point), "`Right Y+`", "`Right Y-`", "`Right X-`", "`Right X+`");
    stick(NG(NGr::Stick), "`Left Y+`", "`Left Y-`", "`Left X-`", "`Left X+`");
  }

  static_cast<ControllerEmu::Attachments*>(BigPictureGroup(port, WG(WGr::Attachments)))
      ->SetSelectedAttachment(static_cast<u32>(extension));
  if (auto* setting = SidewaysSetting(port))
    setting->SetValue(sideways);
  // A round stick drives the pointer: stretch its circle to the square screen, so the
  // diagonals reach the corners
  static_cast<ControllerEmu::ReshapableInput*>(BigPictureGroup(port, WG(WGr::Point)))
      ->SetCalibrationFromGate(ControllerEmu::RoundStickGate(1.0));
  wm->UpdateReferences(g_controller_interface);
  Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(port), WiimoteSource::Emulated);
  Wiimote::GetConfig()->SaveConfig();
  return true;
}

std::vector<BigPictureSettingCategory> BuildBigPictureWiiControllers(const BigPictureSettingsHost& host)
{
  using WGr = WiimoteEmu::WiimoteGroup;
  using NGr = WiimoteEmu::NunchukGroup;
  using CGr = WiimoteEmu::ClassicGroup;
  BigPictureSettingCategory c{Tr("Manettes Wii"), {}};
  auto port = std::make_shared<int>(0);
  {
    Item item;
    item.label = Tr("Wiimote");
    item.help = Tr("Les réglages ci-dessous s'appliquent à cette Wiimote.");
    item.kind = Item::Kind::Choice;
    item.value = [port] { return Tr("Joueur %1").arg(*port + 1); };
    item.change = [port](int dir) { *port = (*port + dir + 4) % 4; };
    item.activate = [port] { *port = (*port + 1) % 4; };
    c.items.push_back(std::move(item));
  }
  {
    Item item;
    item.label = Tr("Source");
    item.help = Tr("Émulée : ta manette fait la Wiimote. Vraie : une Wiimote connectée au PC.");
    item.kind = Item::Kind::Choice;
    item.value = [port] {
      switch (Config::Get(Config::GetInfoForWiimoteSource(*port)))
      {
      case WiimoteSource::Emulated:
        return Tr("Émulée (ta manette)");
      case WiimoteSource::Real:
        return Tr("Vraie Wiimote");
      default:
        return Tr("Aucune");
      }
    };
    item.change = [port](int dir) {
      const int cur = static_cast<int>(Config::Get(Config::GetInfoForWiimoteSource(*port)));
      Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(*port),
                               static_cast<WiimoteSource>((cur + dir + 3) % 3));
    };
    item.activate = [change = item.change] { change(1); };
    c.items.push_back(std::move(item));
  }
  {
    Item item;
    item.label = Tr("Accessoire");
    item.help = Tr("Branché sur la Wiimote émulée.");
    item.kind = Item::Kind::Choice;
    item.value = [port] {
      switch (BigPictureWiiExtension(*port))
      {
      case 1:
        return Tr("Nunchuk");
      case 2:
        return Tr("Manette classique");
      case 0:
        return Tr("Aucun");
      default:
        return Tr("Autre");
      }
    };
    item.change = [port](int dir) {
      const int cur = std::clamp(BigPictureWiiExtension(*port), 0, 2);
      const auto lock = ControllerEmu::EmulatedController::GetStateLock();
      static_cast<ControllerEmu::Attachments*>(BigPictureGroup(*port, WG(WGr::Attachments)))
          ->SetSelectedAttachment(static_cast<u32>((cur + dir + 3) % 3));
    };
    item.activate = [change = item.change] { change(1); };
    c.items.push_back(std::move(item));
  }
  {
    const auto notify = host.notify;
    c.items.push_back(Action(Tr("Réglage automatique"),
                             Tr("Wiimote (+ Nunchuk) : pointeur au stick droit, secouer avec B. "
                                "Manette classique : disposition Nintendo."),
                             [port, notify] {
                               const int ext = std::clamp(BigPictureWiiExtension(*port), 0, 2);
                               const bool ok = ApplyWiiProfile(*port, ext);
                               if (notify)
                                 notify(ok ? Tr("Wiimote réglée") :
                                             Tr("Configure d'abord la manette de ce joueur"));
                             }));
  }
  {
    const auto notify = host.notify;
    c.items.push_back(Action(Tr("Réglage automatique : Wiimote à l'horizontale"),
                             Tr("Pour les jeux qui se jouent Wiimote couchée (Mario Kart, New "
                                "Super Mario Bros.…) : croix au stick gauche et à la croix, 2 et 1 "
                                "sur A et B, inclinaison au stick droit, secouer avec R."),
                             [port, notify] {
                               const bool ok = ApplyWiiProfile(*port, 0, true);
                               if (notify)
                                 notify(ok ? Tr("Wiimote réglée à l'horizontale") :
                                             Tr("Configure d'abord la manette de ce joueur"));
                             }));
  }
  {
    const auto configure = host.configure_wiimote;
    c.items.push_back(Action(Tr("Assistant bouton par bouton"),
                             Tr("Chaque bouton de la Wiimote et du Nunchuk, ou de la manette "
                                "classique, selon l'accessoire choisi."),
                             [configure, port] { configure(*port); }));
  }
  {
    const auto test = host.test_wiimote;
    c.items.push_back(Action(Tr("Tester la Wiimote"),
                             Tr("Boutons, pointeur, Nunchuk et manette classique."),
                             [test, port] { test(*port); }));
  }
  {
    const auto notify = host.notify;
    c.items.push_back(Action(Tr("Connecter une vraie Wiimote"),
                             Tr("Appuie sur 1 + 2 (ou sur le bouton Sync) de la Wiimote. Sous "
                                "Windows, appaire-la d'abord dans les paramètres Bluetooth."),
                             [port, notify] {
                               Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(*port),
                                                        WiimoteSource::Real);
                               Config::SetBaseOrCurrent(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
                                                        true);
                               WiimoteReal::Refresh();
                               if (notify)
                                 notify(Tr("Recherche… appuie sur 1 + 2 de la Wiimote"));
                             }));
  }

  struct Ctl
  {
    int code;
    int index;
    int mirror;
    bool flip;
    const char* label;
  };
  static const Ctl controls[] = {
      {WG(WGr::Buttons), 0, -1, false, "Wiimote : A"},
      {WG(WGr::Buttons), 1, -1, false, "Wiimote : B"},
      {WG(WGr::Buttons), 2, -1, false, "Wiimote : 1"},
      {WG(WGr::Buttons), 3, -1, false, "Wiimote : 2"},
      {WG(WGr::Buttons), 4, -1, false, "Wiimote : −"},
      {WG(WGr::Buttons), 5, -1, false, "Wiimote : +"},
      {WG(WGr::Buttons), 6, -1, false, "Wiimote : HOME"},
      {WG(WGr::DPad), 0, -1, false, "Wiimote : croix haut"},
      {WG(WGr::DPad), 1, -1, false, "Wiimote : croix bas"},
      {WG(WGr::DPad), 2, -1, false, "Wiimote : croix gauche"},
      {WG(WGr::DPad), 3, -1, false, "Wiimote : croix droite"},
      {WG(WGr::Point), 0, 1, true, "Pointeur : haut / bas"},
      {WG(WGr::Point), 3, 2, true, "Pointeur : droite / gauche"},
      {WG(WGr::Shake), 0, 1, false, "Secouer la Wiimote"},
      {NG(NGr::Buttons), 0, -1, false, "Nunchuk : C"},
      {NG(NGr::Buttons), 1, -1, false, "Nunchuk : Z"},
      {NG(NGr::Stick), 0, 1, true, "Nunchuk : stick haut / bas"},
      {NG(NGr::Stick), 3, 2, true, "Nunchuk : stick droite / gauche"},
      {NG(NGr::Shake), 0, 1, false, "Secouer le Nunchuk"},
      {CG(CGr::Buttons), 0, -1, false, "Classique : a"},
      {CG(CGr::Buttons), 1, -1, false, "Classique : b"},
      {CG(CGr::Buttons), 2, -1, false, "Classique : x"},
      {CG(CGr::Buttons), 3, -1, false, "Classique : y"},
      {CG(CGr::Buttons), 4, -1, false, "Classique : ZL"},
      {CG(CGr::Buttons), 5, -1, false, "Classique : ZR"},
      {CG(CGr::Triggers), 2, 0, false, "Classique : L"},
      {CG(CGr::Triggers), 3, 1, false, "Classique : R"},
      {CG(CGr::Buttons), 6, -1, false, "Classique : −"},
      {CG(CGr::Buttons), 7, -1, false, "Classique : +"},
      {CG(CGr::Buttons), 8, -1, false, "Classique : HOME"},
      {CG(CGr::LeftStick), 0, 1, true, "Classique : stick gauche haut / bas"},
      {CG(CGr::LeftStick), 3, 2, true, "Classique : stick gauche droite / gauche"},
      {CG(CGr::RightStick), 0, 1, true, "Classique : stick droit haut / bas"},
      {CG(CGr::RightStick), 3, 2, true, "Classique : stick droit droite / gauche"},
  };
  const auto remap = host.remap_control;
  for (const Ctl& ctl : controls)
  {
    Item item;
    item.label = Tr(ctl.label);
    item.help = Tr("A : appuie ensuite sur la touche voulue. X : effacer.");
    item.kind = Item::Kind::Action;
    item.value = [port, ctl] {
      std::string expr =
          BigPictureGroup(*port, ctl.code)->controls[ctl.index]->control_ref->GetExpression();
      std::erase(expr, '`');
      QString q = QString::fromStdString(expr);
      if (q.size() > 22)
        q = q.left(20) + QStringLiteral("…");
      return q.isEmpty() ? Tr("—") : q;
    };
    item.activate = [remap, port, ctl] {
      remap(*port, ctl.code, ctl.index, ctl.mirror, ctl.flip, Tr(ctl.label));
    };
    item.change = [](int) {};
    item.clear = [port, ctl] {
      const auto lock = ControllerEmu::EmulatedController::GetStateLock();
      auto* group = BigPictureGroup(*port, ctl.code);
      (void)group->controls[ctl.index]->control_ref->SetExpression("");
      if (ctl.mirror >= 0)
        (void)group->controls[ctl.mirror]->control_ref->SetExpression("");
      Wiimote::GetConfig()->GetController(*port)->UpdateReferences(g_controller_interface);
    };
    c.items.push_back(std::move(item));
  }
  const auto calibrate = host.calibrate_stick;
  c.items.push_back(Action(Tr("Étalonner le stick du Nunchuk"), {},
                           [calibrate, port] { calibrate(*port, NG(NGr::Stick)); }));
  c.items.push_back(Action(Tr("Étalonner le stick gauche (classique)"), {},
                           [calibrate, port] { calibrate(*port, CG(CGr::LeftStick)); }));
  c.items.push_back(Action(Tr("Étalonner le stick droit (classique)"), {},
                           [calibrate, port] { calibrate(*port, CG(CGr::RightStick)); }));
  c.items.push_back(Toggle(Tr("Bluetooth direct (vraies Wiimotes)"),
                           Tr("Donne une clé Bluetooth entière à la Wii émulée (avancé)."),
                           Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED));
  std::vector<BigPictureSettingCategory> cats;
  cats.push_back(std::move(c));
  return cats;
}

// ---------------------------------------------------------------- per game: textures, controls

namespace
{
std::string TexturePackDir(const std::string& game_id)
{
  const std::string root = File::GetUserPath(D_HIRESTEXTURES_IDX);
  for (const std::string& id : {game_id, game_id.substr(0, 3)})
  {
    if (!id.empty() && File::IsDirectory(root + id))
      return root + id;
  }
  return {};
}

void SetGameControlsProfile(const std::string& game_id, const char* key, const std::string& name)
{
  const std::string game_ini = File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
  Common::IniFile gini;
  gini.Load(game_ini);
  gini.GetOrCreateSection("Controls")->Set(key, name);
  File::CreateFullPath(game_ini);
  gini.Save(game_ini);
}

QStringList ProfileNames(InputConfig* config)
{
  const QDir dir(QString::fromStdString(config->GetUserProfileDirectoryPath()));
  QStringList names;
  for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.ini")}, QDir::Files, QDir::Name))
    names << f.completeBaseName();
  return names;
}

Item GameProfileChoice(const QString& label, const QString& help, const std::string& game_id,
                       const char* key, InputConfig* config)
{
  Item item;
  item.label = label;
  item.help = help;
  item.kind = Item::Kind::Choice;
  const std::string ini_path = File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
  const auto read = [ini_path, key]() -> QString {
    Common::IniFile ini;
    ini.Load(ini_path);
    std::string value;
    if (auto* section = ini.GetSection("Controls"))
      section->Get(key, &value);
    return QString::fromStdString(value);
  };
  const auto write = [ini_path, key](const QString& value) {
    Common::IniFile ini;
    ini.Load(ini_path);
    auto* section = ini.GetOrCreateSection("Controls");
    if (value.isEmpty())
      section->Delete(key);
    else
      section->Set(key, value.toStdString());
    File::CreateFullPath(ini_path);
    ini.Save(ini_path);
  };
  item.value = [read] {
    const QString v = read();
    return v.isEmpty() ? Tr("Manette habituelle") : v;
  };
  item.change = [read, write, config](int dir) {
    QStringList options = ProfileNames(config);
    options.prepend(QString());
    int i = static_cast<int>(options.indexOf(read()));
    if (i < 0)
      i = 0;
    const int n = static_cast<int>(options.size());
    write(options[(i + dir + n) % n]);
  };
  item.activate = [change = item.change] { change(1); };
  return item;
}
}  // namespace

bool HasTexturePack(const std::string& game_id)
{
  return !TexturePackDir(game_id).empty();
}

std::vector<BigPictureSettingCategory>
BuildBigPictureGameExtras(const std::shared_ptr<Config::Layer>& layer, const std::string& game_id,
                          const QString& title, std::function<void(const QString&)> notify)
{
  std::vector<BigPictureSettingCategory> cats;
  {
    BigPictureSettingCategory c{Tr("Textures HD"), {}};
    {
      Item item;
      item.label = Tr("Pack de textures");
      item.help = Tr("Mets le pack dans le dossier Load/Textures/%1 de Dolphin.")
                      .arg(QString::fromStdString(game_id));
      item.kind = Item::Kind::Action;
      item.value = [game_id] {
        const std::string dir = TexturePackDir(game_id);
        if (dir.empty())
          return Tr("Aucun");
        qint64 files = 0, bytes = 0;
        QDirIterator it(QString::fromStdString(dir), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext() && files < 200000)
        {
          it.next();
          ++files;
          bytes += it.fileInfo().size();
        }
        return Tr("Installé · %1 fichiers · %2 Mo").arg(files).arg(bytes / (1024 * 1024));
      };
      item.activate = [game_id] {
        std::string dir = TexturePackDir(game_id);
        if (dir.empty())
        {
          dir = File::GetUserPath(D_HIRESTEXTURES_IDX) + game_id;
          File::CreateFullPath(dir + DIR_SEP);
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
      };
      item.change = [](int) {};
      c.items.push_back(std::move(item));
    }
    c.items.push_back(GameBool(layer, Tr("Utiliser les textures HD"),
                               Tr("Charge le pack de textures de ce jeu."),
                               Config::GFX_HIRES_TEXTURES));
    c.items.push_back(GameBool(layer, Tr("Précharger les textures"),
                               Tr("Évite les saccades, mais demande beaucoup de mémoire."),
                               Config::GFX_CACHE_HIRES_TEXTURES));
    cats.push_back(std::move(c));
  }
  {
    BigPictureSettingCategory c{Tr("Manette pour ce jeu"), {}};
    c.items.push_back(GameProfileChoice(Tr("Profil GameCube (joueur 1)"),
                                        Tr("Chargé au lancement de ce jeu à la place des réglages "
                                           "habituels."),
                                        game_id, "PadProfile1", Pad::GetConfig()));
    c.items.push_back(GameProfileChoice(Tr("Profil Wiimote (joueur 1)"), {}, game_id,
                                        "WiimoteProfile1", Wiimote::GetConfig()));
    c.items.push_back(Action(
        Tr("Créer un profil avec la manette actuelle"),
        Tr("Enregistre les réglages actuels du joueur 1 et les associe à ce jeu."),
        [game_id, title, notify] {
          QString name = title;
          name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
          name = name.left(60).trimmed();
          const std::string path = Pad::GetConfig()->GetUserProfileDirectoryPath() +
                                   name.toStdString() + ".ini";
          File::CreateFullPath(path);
          Common::IniFile ini;
          {
            const auto lock = ControllerEmu::EmulatedController::GetStateLock();
            Pad::GetConfig()->GetController(0)->SaveConfig(ini.GetOrCreateSection("Profile"));
          }
          ini.Save(path);
          const std::string game_ini = File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
          Common::IniFile gini;
          gini.Load(game_ini);
          gini.GetOrCreateSection("Controls")->Set("PadProfile1", name.toStdString());
          File::CreateFullPath(game_ini);
          gini.Save(game_ini);
          if (notify)
            notify(Tr("Profil « %1 » associé à ce jeu").arg(name));
        }));
    c.items.push_back(Action(
        Tr("Créer un profil Wiimote avec les réglages actuels"),
        Tr("Enregistre la Wiimote du joueur 1 (accessoire, orientation, boutons) et l'associe à "
           "ce jeu."),
        [game_id, title, notify] {
          QString name = title;
          name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
          name = name.left(60).trimmed() + QStringLiteral(" (Wii)");
          const std::string path = Wiimote::GetConfig()->GetUserProfileDirectoryPath() +
                                   name.toStdString() + ".ini";
          File::CreateFullPath(path);
          Common::IniFile ini;
          {
            const auto lock = ControllerEmu::EmulatedController::GetStateLock();
            Wiimote::GetConfig()->GetController(0)->SaveConfig(ini.GetOrCreateSection("Profile"));
          }
          ini.Save(path);
          SetGameControlsProfile(game_id, "WiimoteProfile1", name.toStdString());
          if (notify)
            notify(Tr("Profil « %1 » associé à ce jeu").arg(name));
        }));
    c.items.push_back(Action(
        Tr("Charger un fichier .ini pour la Wiimote"),
        Tr("Un profil Dolphin (Config/Profiles/Wiimote) ou un WiimoteNew.ini : il est copié dans "
           "tes profils et associé à ce jeu."),
        [game_id, notify] {
          const QString file = QFileDialog::getOpenFileName(
              nullptr, Tr("Configuration de Wiimote pour ce jeu"), QDir::homePath(),
              Tr("Configuration Dolphin (*.ini)"));
          if (file.isEmpty())
            return;
          Common::IniFile src;
          if (!src.Load(file.toStdString()))
          {
            if (notify)
              notify(Tr("Fichier illisible"));
            return;
          }
          // A profile has a [Profile] section; WiimoteNew.ini has [Wiimote1]…[Wiimote4].
          const Common::IniFile::Section* section = src.GetSection("Profile");
          if (!section || section->GetValues().empty())
            section = src.GetSection("Wiimote1");
          if (!section || section->GetValues().empty())
          {
            if (notify)
              notify(Tr("Ce fichier n'est pas une configuration de Wiimote"));
            return;
          }
          const std::string name = QFileInfo(file).completeBaseName().toStdString();
          const std::string path =
              Wiimote::GetConfig()->GetUserProfileDirectoryPath() + name + ".ini";
          Common::IniFile out;
          auto* dst = out.GetOrCreateSection("Profile");
          for (const auto& [key, value] : section->GetValues())
            dst->Set(key, value);
          File::CreateFullPath(path);
          out.Save(path);
          SetGameControlsProfile(game_id, "WiimoteProfile1", name);
          if (notify)
            notify(Tr("« %1 » chargé pour ce jeu").arg(QString::fromStdString(name)));
        }));
    cats.push_back(std::move(c));
  }
  return cats;
}

// ---------------------------------------------------------------- display presets

void ApplyBigPictureDisplayPreset(int screen_height)
{
  int preset = Settings::GetQSettings().value(QStringLiteral("bigpicture/preset"), 0).toInt();
  if (preset == 0)
    return;
  if (preset == 1)  // automatic: small screens are handhelds
    preset = screen_height <= 1200 ? 2 : 3;
  if (preset == 2)
  {
    // Portable (ROG Xbox Ally, Steam Deck…): smooth and light on the battery
    Config::SetCurrent(Config::GFX_EFB_SCALE, 2);
    Config::SetCurrent(Config::GFX_MSAA, 1u);
    Config::SetCurrent(Config::GFX_SSAA, false);
    Config::SetCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force2x);
    Config::SetCurrent(Config::GFX_VSYNC, true);
  }
  else
  {
    // TV: sharp picture at the TV's resolution (1080p: 3x, 1440p: 4x, 4K: 5x)
    const int scale = std::clamp(static_cast<int>(std::lround(screen_height / 480.0 + 0.5)), 2, 6);
    Config::SetCurrent(Config::GFX_EFB_SCALE, scale);
    Config::SetCurrent(Config::GFX_MSAA, 4u);
    Config::SetCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY, AnisotropicFilteringMode::Force16x);
  }
  // Shaders compiled in the background: no stutter the first time an effect shows up
  Config::SetCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                     ShaderCompilationMode::AsynchronousUberShaders);
}
