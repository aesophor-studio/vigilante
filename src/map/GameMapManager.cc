// Copyright (c) 2018-2021 Marco Wang <m.aesophor@gmail.com>. All rights reserved.
#include "GameMapManager.h"

#include <thread>

#include <Box2D/Box2D.h>

#include "Assets.h"
#include "Audio.h"
#include "CallbackManager.h"
#include "Constants.h"
#include "character/Npc.h"
#include "character/Player.h"
#include "item/Equipment.h"
#include "scene/GameScene.h"
#include "scene/SceneManager.h"
#include "skill/MagicalMissile.h"
#include "util/box2d/b2BodyBuilder.h"

using namespace std;
USING_NS_CC;

namespace vigilante {

GameMapManager::GameMapManager(const b2Vec2& gravity)
    : _layer(Layer::create()),
      _worldContactListener(std::make_unique<WorldContactListener>()),
      _world(std::make_unique<b2World>(gravity)),
      _gameMap(),
      _player(),
      _npcSpawningBlacklist(),
      _areNpcsAllowedToAct(true),
      _allPortalStates() {
  _world->SetAllowSleeping(true);
  _world->SetContinuousPhysics(true);
  _world->SetContactListener(_worldContactListener.get());
}

void GameMapManager::update(float delta) {
  for (auto& actor : _gameMap->_dynamicActors) {
    actor->update(delta);
  }

  if (_player) {
    _player->update(delta);

    for (const auto& ally : _player->getAllies()) {
      ally->update(delta);
    }
  }
}

void GameMapManager::loadGameMap(const string& tmxMapFileName,
                                 const function<void ()>& afterLoadingGameMap) {
  auto shade = SceneManager::the().getCurrentScene<GameScene>()->getShade();
  printf("inside: shade->getImageView(): %p\n", shade->getImageView());

  auto workerThreadLambda = [this, shade, tmxMapFileName, afterLoadingGameMap]() {
    // Pauses all NPCs from acting, preventing new callbacks
    // from being generated.
    setNpcsAllowedToAct(false);

    // Block this thread with a spinlock until all callbacks have finished.
    while (CallbackManager::the().getPendingCount() > 0);

    // No pending callbacks. Now it's safe to load the new GameMap.
    shade->getImageView()->runAction(Sequence::createWithTwoActions(
        CallFunc::create([this, tmxMapFileName, afterLoadingGameMap]() {
          doLoadGameMap(tmxMapFileName);
          afterLoadingGameMap();
        }),
        FadeOut::create(Shade::kFadeOutTime)
    ));

    // Resume NPCs to act.
    setNpcsAllowedToAct(true);
  };

  // 1. Fade in the shade
  // 2. Create another thread which executes the above lambda independently.
  shade->getImageView()->runAction(Sequence::createWithTwoActions(
      FadeIn::create(Shade::kFadeInTime),
      CallFunc::create([workerThreadLambda]() {
        thread(workerThreadLambda).detach();
      })
  ));
}

GameMap* GameMapManager::doLoadGameMap(const string& tmxMapFileName) {
  string oldBgmFileName;

  // Remove deceased party member from player's party,
  // and remove their b2body and texture.
  if (_player) {
    for (auto ally : _player->getAllies()) {
      ally->onMapChanged();
    }
  }

  // Clean up previous GameMap.
  if (_gameMap) {
    oldBgmFileName = _gameMap->getBgmFileName();
    _layer->removeChild(_gameMap->getTmxTiledMap());
    _gameMap->deleteObjects();
    _gameMap.reset();  // deletes the underlying GameMap object and _gameMap = nullptr.
  }

  // Load the new GameMap.
  _gameMap = std::make_unique<GameMap>(_world.get(), tmxMapFileName);
  _gameMap->createObjects();
  _layer->addChild(_gameMap->getTmxTiledMap(), graphical_layers::kTmxTiledMap);

  // If the player object hasn't been created yet, then spawn it.
  if (!_player) {
    _player = _gameMap->createPlayer();
  }

  if (oldBgmFileName != _gameMap->getBgmFileName()) {
    Audio::the().stopBgm();
    Audio::the().playBgm(_gameMap->getBgmFileName());
  }

  return _gameMap.get();
}

bool GameMapManager::isNpcAllowedToSpawn(const string& jsonFileName) const {
  return _npcSpawningBlacklist.find(jsonFileName) == _npcSpawningBlacklist.end();
}

void GameMapManager::setNpcAllowedToSpawn(const string& jsonFileName, bool canSpawn) {
  if (!canSpawn && isNpcAllowedToSpawn(jsonFileName)) {
    _npcSpawningBlacklist.insert(jsonFileName);
  } else if (canSpawn && !isNpcAllowedToSpawn(jsonFileName)) {
    _npcSpawningBlacklist.erase(jsonFileName);
  }
}

bool GameMapManager::hasSavedPortalLockUnlockState(const string& tmxMapFileName,
                                                   int targetPortalId) const {
  auto mapIt = _allPortalStates.find(tmxMapFileName);
  if (mapIt == _allPortalStates.end()) {
    return false;
  }

  return std::find_if(mapIt->second.begin(),
                      mapIt->second.end(),
                      [targetPortalId](const pair<int, bool>& entry) {
                          return entry.first == targetPortalId;
                      }) != mapIt->second.end();
}

bool GameMapManager::isPortalLocked(const string& tmxMapFileName,
                                    int targetPortalId) const {
  // If we cannot find the associated vector for this tiled map
  // in the unordered_map, then simply return false.
  auto it = _allPortalStates.find(tmxMapFileName);
  if (it == _allPortalStates.end()) {
    VGLOG(LOG_WARN, "Unable to find the corresponding portal vector");
    return false;
  }

  // Otherwise, we've found the associated vector of this TiledMap.
  // Now we should find the corresponding entry in that vector
  // and return entry.second which holds the lock state of the Portal.
  for (const auto& entry : it->second) {
    if (entry.first == targetPortalId) {
      return entry.second;
    }
  }

  // If we end up here, then the vector `it->second` doesn't contain
  // the corresponding entry. We should simply return false.
  VGLOG(LOG_WARN, "Unable to find the corresponding entry in the portal vector");
  return false;
}

void GameMapManager::setPortalLocked(const string& tmxMapFileName,
                                     int targetPortalId, bool locked) {
  // If we cannot find the associated vector for this tiled map
  // in the unordered_map, then insert a new vector which is
  // initialized with {targetPortalId, locked} and return early.
  auto it = _allPortalStates.find(tmxMapFileName);
  if (it == _allPortalStates.end()) {
    _allPortalStates.insert({tmxMapFileName, {{targetPortalId, locked}}});
    return;
  }

  // Otherwise, we've found the associated vector of this TiledMap.
  // Now we should find the corresponding entry in that vector,
  // update that entry, and return early.
  for (auto& entry : it->second) {
    if (entry.first == targetPortalId) {
      entry.second = locked;
      return;
    }
  }

  // If we end up here, then the vector `it->second` doesn't contain
  // the corresponding entry, and thus we have to insert it manually.
  it->second.push_back({targetPortalId, locked});
}

}  // namespace vigilante
