/*  LOOT

    A load order optimisation tool for Oblivion, Skyrim, Fallout 3 and
    Fallout: New Vegas.

    Copyright (C) 2014-2017    WrinklyNinja

    This file is part of LOOT.

    LOOT is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    LOOT is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LOOT.  If not, see
    <https://www.gnu.org/licenses/>.
    */

#include "gui/state/loot_state.h"

#include <unordered_set>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/locale.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>

#include "gui/state/game_detection_error.h"
#include "gui/state/loot_paths.h"
#include "gui/version.h"
#include "loot/api.h"
#include "loot/windows_encoding_converters.h"

#ifdef _WIN32
#include <windows.h>
#endif

using boost::format;
using boost::locale::translate;
using std::exception;
using std::locale;
using std::lock_guard;
using std::mutex;
using std::string;
using std::vector;

namespace fs = boost::filesystem;

namespace loot {
LootState::LootState() : unappliedChangeCounter_(0), currentGame_(installedGames_.end()) {}

void LootState::load(YAML::Node& settings) {
  lock_guard<mutex> guard(mutex_);

  LootSettings::load(settings);

  // Enable/disable debug logging in case it has changed.
  enableDebugLogging(isDebugLoggingEnabled());

  // Update existing games, add new games.
  std::unordered_set<string> newGameFolders;
  BOOST_LOG_TRIVIAL(trace) << "Updating existing games and adding new games.";
  for (const auto &gameSettings : getGameSettings()) {
    auto pos = find(installedGames_.begin(), installedGames_.end(), gameSettings);

    if (pos != installedGames_.end()) {
      pos->SetName(gameSettings.Name())
        .SetMaster(gameSettings.Master())
        .SetRepoURL(gameSettings.RepoURL())
        .SetRepoBranch(gameSettings.RepoBranch())
        .SetGamePath(gameSettings.GamePath())
        .SetRegistryKey(gameSettings.RegistryKey());
    } else {
      if (gui::Game::IsInstalled(gameSettings)) {
        BOOST_LOG_TRIVIAL(trace) << "Adding new installed game entry for: " << gameSettings.FolderName();
        installedGames_.push_back(gui::Game(gameSettings, LootPaths::getLootDataPath()));
        updateStoredGamePathSetting(installedGames_.back());
      }
    }

    newGameFolders.insert(gameSettings.FolderName());
  }

  // Remove deleted games. As the current game is stored using its index,
  // removing an earlier game may invalidate it.
  BOOST_LOG_TRIVIAL(trace) << "Removing deleted games.";
  for (auto it = installedGames_.begin(); it != installedGames_.end();) {
    if (newGameFolders.find(it->FolderName()) == newGameFolders.end()) {
      BOOST_LOG_TRIVIAL(trace) << "Removing game: " << it->FolderName();
      it = installedGames_.erase(it);
    } else
      ++it;
  }

  if (currentGame_ == end(installedGames_)) {
    selectGame("");
  }

  if (currentGame_ != end(installedGames_)) {
    // Re-initialise the current game in case the game path setting was changed.
    currentGame_->Init();
  }
}

void LootState::init(const std::string& cmdLineGame) {
    // Do some preliminary locale / UTF-8 support setup here, in case the settings file reading requires it.
    //Boost.Locale initialisation: Specify location of language dictionaries.
  boost::locale::generator gen;
  gen.add_messages_path(LootPaths::getL10nPath().string());
  gen.add_messages_domain("loot");

  //Boost.Locale initialisation: Generate and imbue locales.
  locale::global(gen("en.UTF-8"));
  loot::InitialiseLocale("en.UTF-8");
  boost::filesystem::path::imbue(locale());

  // Check if the LOOT local app data folder exists, and create it if not.
  if (!fs::exists(LootPaths::getLootDataPath())) {
    BOOST_LOG_TRIVIAL(info) << "Local app data LOOT folder doesn't exist, creating it.";
    try {
      fs::create_directory(LootPaths::getLootDataPath());
    } catch (exception& e) {
      initErrors_.push_back((format(translate("Error: Could not create LOOT settings file. %1%")) % e.what()).str());
    }
  }
  if (fs::exists(LootPaths::getSettingsPath())) {
    try {
      LootSettings::load(LootPaths::getSettingsPath());
    } catch (exception& e) {
      initErrors_.push_back((format(translate("Error: Settings parsing failed. %1%")) % e.what()).str());
    }
  }

  //Set up logging.
  boost::log::add_file_log(
    boost::log::keywords::file_name = LootPaths::getLogPath().string().c_str(),
    boost::log::keywords::auto_flush = true,
    boost::log::keywords::format = (
      boost::log::expressions::stream
      << "[" << boost::log::expressions::format_date_time< boost::posix_time::ptime >("TimeStamp", "%H:%M:%S.%f") << "]"
      << " [" << boost::log::trivial::severity << "]: "
      << boost::log::expressions::smessage
      )
  );
  boost::log::add_common_attributes();
  SetLogFile(LootPaths::getApiLogPath().string());
  enableDebugLogging(isDebugLoggingEnabled());

  // Log some useful info.
  BOOST_LOG_TRIVIAL(info) << "LOOT Version: " << gui::Version::string() << "+" << gui::Version::revision;
  BOOST_LOG_TRIVIAL(info) << "LOOT API Version: " << LootVersion::string() << "+" << LootVersion::revision;

#ifdef _WIN32
        // Check if LOOT is being run through Mod Organiser.
  bool runFromMO = GetModuleHandle(ToWinWide("hook.dll").c_str()) != NULL;
  if (runFromMO) {
    BOOST_LOG_TRIVIAL(info) << "LOOT is being run through Mod Organiser.";
  }
#endif

        // The CEF debug log is appended to, not overwritten, so it gets really long.
        // Delete the current CEF debug log.
  fs::remove(LootPaths::getLootDataPath() / "CEFDebugLog.txt");

  // Now that settings have been loaded, set the locale again to handle translations.
  if (getLanguage() != MessageContent::defaultLanguage) {
    BOOST_LOG_TRIVIAL(debug) << "Initialising language settings.";
    BOOST_LOG_TRIVIAL(debug) << "Selected language: " << getLanguage();

    //Boost.Locale initialisation: Generate and imbue locales.
    locale::global(gen(getLanguage() + ".UTF-8"));
    loot::InitialiseLocale(getLanguage() + ".UTF-8");
    boost::filesystem::path::imbue(locale());
  }

  // Detect games & select startup game
  //-----------------------------------

  //Detect installed games.
  BOOST_LOG_TRIVIAL(debug) << "Detecting installed games.";
  installedGames_.clear();
  for (const auto& gameSettings : getGameSettings()) {
    if (gui::Game::IsInstalled(gameSettings)) {
      BOOST_LOG_TRIVIAL(trace) << "Adding new installed game entry for: " << gameSettings.FolderName();
      installedGames_.push_back(gui::Game(gameSettings, LootPaths::getLootDataPath()));
      updateStoredGamePathSetting(installedGames_.back());
    }
  }

  try {
    BOOST_LOG_TRIVIAL(debug) << "Selecting game.";
    selectGame(cmdLineGame);
    BOOST_LOG_TRIVIAL(debug) << "Game selected is " << currentGame_->Name();
    BOOST_LOG_TRIVIAL(debug) << "Initialising game-specific settings.";
    currentGame_->Init();
  } catch (std::exception& e) {
    BOOST_LOG_TRIVIAL(error) << "Game-specific settings could not be initialised. " << e.what();
    initErrors_.push_back((format(translate("Error: Game-specific settings could not be initialised. %1%")) % e.what()).str());
  }
}

const std::vector<std::string>& LootState::getInitErrors() const {
  return initErrors_;
}

void LootState::save(const boost::filesystem::path & file) {
  storeLastGame(currentGame_->FolderName());
  updateLastVersion();
  LootSettings::save(file);
}

void LootState::changeGame(const std::string& newGameFolder) {
  lock_guard<mutex> guard(mutex_);

  BOOST_LOG_TRIVIAL(debug) << "Changing current game to that with folder: " << newGameFolder;
  currentGame_ = find_if(installedGames_.begin(), installedGames_.end(), [&](const gui::Game& game) {
    return boost::iequals(newGameFolder, game.FolderName());
  });
  currentGame_->Init();
  BOOST_LOG_TRIVIAL(debug) << "New game is " << currentGame_->Name();
}

gui::Game& LootState::getCurrentGame() {
  lock_guard<mutex> guard(mutex_);

  return *currentGame_;
}

std::vector<std::string> LootState::getInstalledGames() const {
  vector<string> installedGames;
  for (const auto &game : installedGames_) {
    installedGames.push_back(game.FolderName());
  }
  return installedGames;
}

bool LootState::hasUnappliedChanges() const {
  return unappliedChangeCounter_ > 0;
}

void LootState::incrementUnappliedChangeCounter() {
  ++unappliedChangeCounter_;
}

void LootState::decrementUnappliedChangeCounter() {
  if (unappliedChangeCounter_ > 0)
    --unappliedChangeCounter_;
}

void LootState::selectGame(std::string preferredGame) {
  if (preferredGame.empty()) {
      // Get preferred game from settings.
    if (getGame() != "auto")
      preferredGame = getGame();
    else if (getLastGame() != "auto")
      preferredGame = getLastGame();
  }

  // Get iterator to preferred game.
  currentGame_ = find_if(begin(installedGames_), end(installedGames_), [&](gui::Game& game) {
    return preferredGame.empty() || preferredGame == game.FolderName();
  });
  // If the preferred game cannot be found, get the first installed game.
  if (currentGame_ == end(installedGames_)) {
    currentGame_ = begin(installedGames_);
  }

  // If no game can be selected, throw an exception.
  if (currentGame_ == end(installedGames_)) {
    throw GameDetectionError("None of the supported games were detected.");
  }
}

void LootState::enableDebugLogging(bool enable) {
  if (enable) {
    boost::log::core::get()->reset_filter();
    SetLoggingVerbosity(LogVerbosity::trace);
  } else {
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::warning);
    SetLoggingVerbosity(LogVerbosity::warning);
  }
}

void LootState::updateStoredGamePathSetting(const gui::Game& game) {
  auto gameSettings = getGameSettings();
  auto pos = find_if(begin(gameSettings), end(gameSettings), [&](const GameSettings& gameSettings) {
    return boost::iequals(game.FolderName(), gameSettings.FolderName());
  });
  if (pos == end(gameSettings)) {
    BOOST_LOG_TRIVIAL(error) << "Could not find the settings for the current game (" << game.Name() << ")";
  } else {
    pos->SetGamePath(game.GamePath());
    storeGameSettings(gameSettings);
  }
}
}
