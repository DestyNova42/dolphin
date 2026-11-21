// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/BigPicture/BigPictureSettings.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QSettings>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
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
  return static_cast<Effect>(std::clamp(Pref("bigpicture/effect", 0), 0, 4));
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
  Pad::GetConfig()->SaveConfig();
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
    c.items.push_back(PrefChoice(
        Tr("Effet d'arrière-plan"), Tr("L'animation qui vit derrière la bibliothèque."),
        QStringLiteral("bigpicture/effect"),
        {Tr("Halo"), Tr("Particules"), Tr("Cubes"), Tr("Vagues"), Tr("Aucun")}, 0, changed));
    c.items.push_back(PrefChoice(Tr("Transitions"),
                                 Tr("L'effet utilisé quand on change d'onglet ou d'écran."),
                                 QStringLiteral("bigpicture/transition"),
                                 {Tr("Fondu"), Tr("Glissement"), Tr("Zoom"), Tr("Aucune")}, 0,
                                 changed));
    c.items.push_back(PrefToggle(Tr("Animation de démarrage"),
                                 Tr("Le cube animé et le jingle au lancement de Dolphin."),
                                 QStringLiteral("bigpicture/intro"), true, changed));
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
    c.items.push_back(Toggle(Tr("Afficher les FPS"), {}, Config::GFX_SHOW_FPS));
    c.items.push_back(Toggle(Tr("Afficher la vitesse"), {}, Config::GFX_SHOW_SPEED));
    c.items.push_back(Toggle(Tr("Afficher la résolution interne"), {},
                             Config::GFX_SHOW_INTERNAL_RESOLUTION));
    cats.push_back(std::move(c));
  }

  // ---- Enhancements
  {
    BigPictureSettingCategory c{Tr("Améliorations"), {}};
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
    c.items.push_back(Action(Tr("Profil automatique (manette standard)"),
                             Tr("Xbox, PlayStation, Switch Pro… : A, B, X, Y, gâchettes, sticks "
                                "et croix réglés d'un coup."),
                             [port] { ApplyStandardGamepadProfile(*port); }));
    const auto configure = host.configure_controller;
    c.items.push_back(Action(Tr("Assistant bouton par bouton"),
                             Tr("Indispensable pour la manette GameCube Switch 2 et les "
                                "manettes peu courantes."),
                             [configure, port] { configure(*port); }));

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

bool ApplyStandardGamepadProfile(int port)
{
  auto* pad = Pad::GetConfig()->GetController(port);
  if (!pad)
    return false;
  const auto is_gamepad = [](const std::string& d) {
    return d.rfind("SDL/", 0) == 0 || d.rfind("XInput/", 0) == 0 ||
           d.rfind("WGInput/", 0) == 0 || d.rfind("evdev/", 0) == 0;
  };
  std::string device = pad->GetDefaultDevice().ToString();
  if (!is_gamepad(device) || !g_controller_interface.FindDevice(pad->GetDefaultDevice()))
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
