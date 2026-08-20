#include "trajectory.hpp"

#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/DashRingObject.hpp>
#include <Geode/binding/ForceBlockGameObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/binding/RotateGameplayGameObject.hpp>
#include <Geode/binding/SpawnTriggerGameObject.hpp>
#include <Geode/binding/TeleportPortalObject.hpp>

#include "../core/bot.hpp"
#include "../practice_fixes/practice_fixes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using namespace geode::prelude;

namespace {


ShowTrajectory& trajectory() {
    return ShowTrajectory::get();
}

uintptr_t ptrId(void const* ptr) {
    return reinterpret_cast<uintptr_t>(ptr);
}

void teleportPlayerForTrajectory(GJBaseGameLayer* layer, TeleportPortalObject* object, PlayerObject* player);


uint64_t quantize(float value) {
    return static_cast<uint64_t>(std::llround(static_cast<double>(value) * 1000.0));
}

uint64_t trajectoryRefreshSignature(Bot const& bot, PlayLayer* pl) {
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&signature](uint64_t value) {
        signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
    };
    auto mixBool = [&mix](bool value) {
        mix(value ? 1ull : 0ull);
    };
    auto mixFloat = [&mix](float value) {
        mix(quantize(value));
    };
    auto mixColor = [&mixFloat](cocos2d::ccColor4F const& color) {
        mixFloat(color.r);
        mixFloat(color.g);
        mixFloat(color.b);
        mixFloat(color.a);
    };
    auto mixPlayer = [&](PlayerObject* player) {
        mix(reinterpret_cast<uintptr_t>(player));
        if (!player)
            return;

        mixFloat(player->getPositionX());
        mixFloat(player->getPositionY());
        mixFloat(player->getScale());
        mixFloat(player->m_vehicleSize);
        mixFloat(player->getRotation());
        mixFloat(static_cast<float>(player->m_yVelocity));
        mixFloat(static_cast<float>(player->m_platformerXVelocity));
        mixBool(player->m_jumpBuffered);
        mixBool(player->m_holdingButtons[1]);
        mixBool(player->m_holdingButtons[2]);
        mixBool(player->m_holdingButtons[3]);
        mixBool(player->m_holdingLeft);
        mixBool(player->m_holdingRight);
        mixBool(player->m_isDead);
        mixBool(player->m_isDashing);
        mixBool(player->m_isUpsideDown);
        mixBool(player->m_isGoingLeft);
    };

    auto& t = trajectory();
    mix(reinterpret_cast<uintptr_t>(pl));
    mix(static_cast<uint64_t>(pl->m_gameState.m_currentProgress));
    mixFloat(pl->m_gameState.m_timeWarp);
    mixBool(pl->m_gameState.m_isDualMode);
    mixBool(pl->m_isPaused);
    mixBool(pl->m_isPracticeMode);
    mixBool(pl->m_isPlatformer);
    mixBool(pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode);
    mixBool(pl->m_levelSettings && pl->m_levelSettings->m_platformerMode);
    mixBool(bot.trajectoryBothSides);
    mix(static_cast<uint64_t>(std::max(t.length, 0)));
    mixFloat(Bot::getTPS());
    mixColor(t.color1);
    mixColor(t.color2);
    mixColor(t.color3);
    mixPlayer(pl->m_player1);
    mixPlayer(pl->m_player2);
    return signature;
}

int64_t g_lastTrajectoryRefreshMs = 0;

bool shouldLogSlowTrajectory(int64_t totalMs) {
    return totalMs >= 50;
}

cocos2d::ccColor4F colorForMode(int mode) {
    auto& t = trajectory();
    if (mode & ShowTrajectory::Hold)
        return t.color1;
    if (mode & ShowTrajectory::Swift)
        return t.color3;
    return t.color2;
}

cocos2d::ccColor4F invertedColor(cocos2d::ccColor4F color) {
    return cocos2d::ccc4f(1.f - color.r, 1.f - color.g, 1.f - color.b, color.a);
}


bool shouldApplyReplayInputsForPrediction(ShowTrajectory::PredictionConfig const& config) {
    return config.applyReplayInputs && Bot::get().state == state::playing;
}

void applyReplayButton(PlayerObject* player, ReplayInput const& input) {
    if (!player)
        return;

    auto button = static_cast<PlayerButton>(input.button);
    if (input.down)
        player->pushButton(button);
    else
        player->releaseButton(button);
}

void applyReplayInputsForPrediction(
    PlayLayer* pl,
    size_t& inputIndex,
    uint64_t frame,
    bool simulateBothPlayers,
    bool enabled
) {
    auto& bot = Bot::get();
    if (!enabled || !pl)
        return;

    auto const& inputs = bot.replay.inputs;
    while (inputIndex < inputs.size() && inputs[inputIndex].frame <= frame) {
        auto const& input = inputs[inputIndex];
        bool enginePlayer2 = !input.player2;
        PlayerObject* player = enginePlayer2 ?
            ShowTrajectory::fakePlayer(false) :
            ShowTrajectory::fakePlayer(true);
        PlayerObject* other = enginePlayer2 ?
            ShowTrajectory::fakePlayer(true) :
            ShowTrajectory::fakePlayer(false);

        if (static_cast<int>(input.frame) != bot.respawnFrame) {
            applyReplayButton(player, input);
            if (simulateBothPlayers && pl->m_gameState.m_isDualMode)
                applyReplayButton(other, input);
        }

        inputIndex++;
    }
}


struct LayerStateGuard {
    PlayLayer* pl = nullptr;
    GJGameState gameState;
    EffectManagerState effectState;
    gd::vector<SavedObjectStateRef> dynamicObjects;
    gd::vector<SavedActiveObjectState> activeObjects;
    gd::vector<SavedSpecialObjectState> specialObjects;
    gd::unordered_map<int, int> persistentItemMap;
    std::array<float, 2000> varianceValues = {};
    gd::vector<GameObject*> calcNonEffectObjects;
    int calcNonEffectObjectsSize = 0;
    uint64_t randomSeed = 0;
    bool hasEffectState = false;
    bool hasPersistentItems = false;
    int solidCount = 0;
    int hazardCount = 0;

    explicit LayerStateGuard(PlayLayer* layer) : pl(layer) {
        if (!pl)
            return;

        gameState = pl->m_gameState;
        solidCount = pl->m_solidCollisionObjectsCount;
        hazardCount = pl->m_hazardCollisionObjectsCount;
        pl->saveDynamicSaveObjects(dynamicObjects);
        pl->saveActiveSaveObjects(activeObjects, specialObjects);
        varianceValues = pl->m_varianceValues;
        calcNonEffectObjects = pl->m_calcNonEffectObjects;
        calcNonEffectObjectsSize = pl->m_calcNonEffectObjectsSize;
        randomSeed = GameToolbox::getfast_srand();
        if (pl->m_effectManager) {
            pl->m_effectManager->saveToState(effectState);
            persistentItemMap = pl->m_effectManager->m_persistentItemCountMap;
            hasEffectState = true;
            hasPersistentItems = true;
        }
    }

    ~LayerStateGuard() {
        restore();
    }

    void restore() {
        if (!pl)
            return;

        ShowTrajectory::restoreSnapshots();
        pl->loadDynamicSaveObjects(dynamicObjects);
        pl->loadActiveSaveObjects(activeObjects, specialObjects);
        pl->m_gameState = gameState;
        pl->m_solidCollisionObjectsCount = solidCount;
        pl->m_hazardCollisionObjectsCount = hazardCount;
        pl->m_varianceValues = varianceValues;
        pl->m_calcNonEffectObjects = calcNonEffectObjects;
        pl->m_calcNonEffectObjectsSize = calcNonEffectObjectsSize;
        GameToolbox::fast_srand(randomSeed);
        if (pl->m_effectManager && hasEffectState)
            pl->m_effectManager->loadFromState(effectState);
        if (pl->m_effectManager && hasPersistentItems)
            pl->m_effectManager->m_persistentItemCountMap = persistentItemMap;
    }
};


void clearCollisionLogs(PlayerObject* player) {
    if (!player)
        return;

    player->m_collisionLogTop->removeAllObjects();
    player->m_collisionLogBottom->removeAllObjects();
    player->m_collisionLogLeft->removeAllObjects();
    player->m_collisionLogRight->removeAllObjects();
}

void clearCollisionState(PlayerObject* player) {
    if (!player)
        return;

    player->m_collidedBottomMaxY = 0.0;
    player->m_collidedTopMinY = 0.0;
    player->m_collidedLeftMaxX = 0.0;
    player->m_collidedRightMinX = 0.0;
    player->m_unkA29 = false;
    clearCollisionLogs(player);
    player->m_unk50C = -1;
    player->m_unk510 = -1;
    player->m_lastCollisionBottom = -1;
    player->m_lastCollisionTop = -1;
    player->m_lastCollisionLeft = -1;
    player->m_lastCollisionRight = -1;
}

float gravitySign(PlayerObject* player) {
    return player && player->m_isUpsideDown ? -1.f : 1.f;
}

cocos2d::CCPoint pointForAngle(float angle) {
    return {std::cos(angle), std::sin(angle)};
}

void applyInitialInput(PlayLayer* pl, PlayerObject* player, PlayerObject* realPlayer, int mode) {
    if (!pl || !player || !realPlayer)
        return;
    if (mode == 0)
        return;

    switch (mode & (ShowTrajectory::Hold | ShowTrajectory::Swift | ShowTrajectory::Release)) {
    case ShowTrajectory::Hold:
        player->pushButton(PlayerButton::Jump);
        break;
    case ShowTrajectory::Swift:
        player->pushButton(PlayerButton::Jump);
        player->releaseButton(PlayerButton::Jump);
        break;
    case ShowTrajectory::Release:
        player->releaseButton(PlayerButton::Jump);
        player->m_jumpBuffered = false;
        break;
    default:
        break;
    }

    if (!pl->m_isPlatformer)
        return;

    switch (mode & (ShowTrajectory::Left | ShowTrajectory::Right)) {
    case ShowTrajectory::Left:
        player->releaseButton(PlayerButton::Right);
        player->pushButton(PlayerButton::Left);
        break;
    case ShowTrajectory::Right:
        player->releaseButton(PlayerButton::Left);
        player->pushButton(PlayerButton::Right);
        break;
    default:
        player->releaseButton(PlayerButton::Left);
        player->releaseButton(PlayerButton::Right);
        break;
    }
}

void drawPredictionSegment(
    cocos2d::CCDrawNode* drawNode,
    PlayerObject* player,
    cocos2d::CCPoint from,
    float width,
    cocos2d::ccColor4F color
) {
    if (!drawNode || !player)
        return;

    drawNode->drawSegment(from, player->getPosition(), width, color);
}

void resetFakePlayerFrom(PlayerObject* realPlayer, PlayerObject* fakePlayer) {
    if (!realPlayer || !fakePlayer)
        return;

    fakePlayer->copyAttributes(realPlayer);
    PlayerPracticeFixes::transfer(realPlayer, fakePlayer, true);
    fakePlayer->m_maybeReducedEffects = true;
    fakePlayer->m_playEffects = false;
    ShowTrajectory::hideFakePlayer(fakePlayer);
}


float advanceGameClock(PlayLayer* pl, PlayerObject* p1, PlayerObject* p2) {
    float tps = std::max(Bot::getTPS(), 1.f);
    double physicsSeconds = 1.0 / tps;
    float timeWarp = std::min(pl->m_gameState.m_timeWarp, 1.f);
    if (timeWarp <= 0.f)
        timeWarp = 1.f;

    pl->m_gameState.m_totalTime += physicsSeconds;
    pl->m_gameState.m_unkDouble3 += physicsSeconds / timeWarp;
    pl->m_gameState.m_currentProgress++;
    pl->m_gameState.m_unkUint5 += static_cast<int>(std::round(timeWarp * 1000.f));

    if (p1)
        p1->m_totalTime += physicsSeconds;
    if (p2)
        p2->m_totalTime += physicsSeconds;

    float playerSpeed = pl->m_gameState.m_timeModRelated;
    if (playerSpeed != 0.f) {
        pl->m_gameState.m_timeModRelated = 0;
        pl->m_gameState.m_timeModRelated2 = 0;
        if (p1)
            p1->updateTimeMod(playerSpeed, true);
        if (p2)
            p2->updateTimeMod(playerSpeed, true);
    }

    return (1.f / tps) * 60.f;
}


void updateFakePlayer(PlayerObject* player, float delta) {
    if (!player || ShowTrajectory::fakePlayerDead(player))
        return;

    clearCollisionLogs(player);
    player->m_playEffects = false;
    bool reducedEffects = player->m_maybeReducedEffects;
    player->m_maybeReducedEffects = false;
    player->update(delta);
    player->m_maybeReducedEffects = reducedEffects;
    player->m_unkUnused3 = player->getRotation();
    player->updateRotation(delta);
    player->m_shipRotation = player->getPosition();
}


void spiderTestJumpForTrajectory(PlayerObject* player) {
    if (!player)
        return;

    bool reducedEffects = player->m_maybeReducedEffects;
    player->m_maybeReducedEffects = false;
    player->spiderTestJump(false);
    player->m_maybeReducedEffects = reducedEffects;
}

bool isTrajectorySpawnObject(EffectGameObject* object) {
    if (!object)
        return false;

    switch (object->m_objectID) {
    case 200:
    case 201:
    case 202:
    case 203:
    case 1334:
    case 2066:
    case 2900:
    case 3022:
        return true;
    default:
        return false;
    }
}

void activateForTrajectory(EffectGameObject* object, PlayerObject* player) {
    ShowTrajectory::snapshotObject(object);
    ShowTrajectory::rememberActivated(player, object);
}

bool hasTrajectoryActivatedObject(PlayerObject* player, EnhancedGameObject* object) {
    return ShowTrajectory::hasActivated(player, object) ||
        ShowTrajectory::realPlayerHasActivated(player, object);
}

void rememberPortalActivation(PlayerObject* player, EffectGameObject* object) {
    player->m_lastPortalPos = object->getPosition();
    player->m_lastActivatedPortal = object;
    activateForTrajectory(object, player);
}


void flipGravityForTrajectory(PlayerObject* player, bool gravity) {
    if (!player || player->m_isUpsideDown == gravity)
        return;

    auto flipSingle = [](PlayerObject* target, bool targetGravity) {
        target->m_isUpsideDown = targetGravity;
        target->m_lastFlipTime = target->m_totalTime;
        if (target->m_wasOnSlope || target->m_isOnSlope)
            target->m_slopeFlipGravityRelated = !target->m_slopeFlipGravityRelated;

        clearCollisionState(target);
        target->m_yVelocity *= 0.5;
        target->m_isOnGround = false;

        if (target->m_isBall) {
            target->m_isRotating = false;
            target->m_isBallRotating = false;
            target->m_isBallRotating2 = false;
            target->m_rotationSpeed = 0.0;
            target->runBallRotation2();
        }
    };

    flipSingle(player, gravity);

    auto* layer = GJBaseGameLayer::get();
    if (!layer || !ShowTrajectory::isFakePlayer(player) || layer->m_gameState.m_unkBool31 ||
        !layer->m_gameState.m_isDualMode || layer->m_levelSettings->m_twoPlayerMode) {
        return;
    }

    PlayerObject* other = ShowTrajectory::otherFakePlayer(player);
    if (!other)
        return;

    if (!(player->m_isShip == other->m_isShip &&
          player->m_isBall == other->m_isBall &&
          player->m_isBird == other->m_isBird &&
          player->m_isSpider == other->m_isSpider &&
          player->m_isRobot == other->m_isRobot &&
          player->m_isSwing == other->m_isSwing)) {
        return;
    }

    flipSingle(other, !gravity);
}

void propellPlayerForTrajectory(PlayerObject* player, float force, bool, int) {
    if (!player)
        return;

    player->m_maybeIsBoosted = true;
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    player->m_isOnSlope = false;
    player->m_wasOnSlope = false;

    float sizeGravity = player->m_vehicleSize != 1.f ? 0.8f : 1.f;
    player->setYVelocity(force * sizeGravity * gravitySign(player) * 16.f, 0);

    if (player->m_isBall || player->m_isSpider || player->m_isSwing)
        player->m_yVelocity *= 0.6;

    if (!player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating = false;
        player->m_isBallRotating2 = false;
        player->m_rotationSpeed = 0.0;
        if (!player->m_isBall)
            player->runBallRotation(player->m_yVelocity);
        else
            player->runNormalRotation(0, 1.0);
    }

    player->m_lastGroundedPos = player->getPosition();
}

void bumpFakePlayerForTrajectory(
    PlayerObject* player,
    float force,
    int objectType,
    bool noEffects,
    GameObject* object
) {
    if (!player)
        return;

    if (player->m_isPlatformer || !player->m_fixRobotJump)
        player->m_touchedPad = true;

    if (objectType != static_cast<int>(GameObjectType::SpiderPad)) {
        propellPlayerForTrajectory(player, force, noEffects, objectType);
        player->m_isAccelerating = objectType == static_cast<int>(GameObjectType::RedJumpPad);
        if (player->m_isAccelerating)
            player->m_lastGroundedPos = cocos2d::CCPoint{0.f, 0.f};
        return;
    }

    if (object) {
        bool facing = player->m_isSideways ? object->isFacingLeft() : object->isFacingDown();
        if (player->m_isUpsideDown != facing)
            flipGravityForTrajectory(player, !player->m_isUpsideDown);
    }

    spiderTestJumpForTrajectory(player);
}

void startDashingForTrajectory(PlayerObject* player, DashRingObject* ring) {
    if (!player || !ring || player->m_isDashing)
        return;

    player->m_isDashing = true;
    player->m_lastLandTime = 0.0;
    player->m_isRotating = false;
    player->m_isBallRotating2 = false;
    player->m_isBallRotating = false;
    player->m_rotationSpeed = 0.0;
    player->m_dashRing = ring;
    player->m_dashStartTime = player->m_totalTime;

    float dashAngle = ring->getObjectRotation();
    if (ring->isFlipX())
        dashAngle += 180.f;
    if (std::fabs(ring->getRotationX() - ring->getRotationY()) > 179.f)
        dashAngle += 180.f;

    dashAngle = std::fmod(-dashAngle, 360.f);
    if (dashAngle < -180.f)
        dashAngle += 360.f;
    else if (dashAngle > 180.f)
        dashAngle -= 360.f;

    float sidewaysAngle = 0.f;
    if (player->m_isPlatformer) {
        sidewaysAngle = ring->m_dashSpeed * 5.77f;
    } else {
        if (player->m_isSideways)
            sidewaysAngle = -90.f;
        if (player->m_isGoingLeft)
            sidewaysAngle += 180.f;

        float angle = dashAngle + sidewaysAngle;
        if (angle < -180.f)
            angle += 360.f;
        else if (angle > 180.f)
            angle -= 360.f;

        if (std::fabs(angle) > 90.f) {
            float side = angle <= 0.f ? -180.f : 180.f;
            angle -= side;
        }

        dashAngle = std::clamp(angle, -70.f, 70.f) - sidewaysAngle;
        sidewaysAngle = 1.f;
    }

    cocos2d::CCPoint dir = pointForAngle(dashAngle * 0.01745329f) * sidewaysAngle;
    if (player->m_isSideways)
        std::swap(dir.x, dir.y);

    player->m_dashX = dir.x;
    player->m_dashY = dir.y;
    if (!player->m_isPlatformer) {
        double value = dir.y / std::fabs(dir.x == 0.f ? 1.f : dir.x);
        player->m_dashY = value;
        player->m_dashX = std::fabs(value);
    } else if (dir.x != 0.f) {
        player->doReversePlayer(dir.x < 0.f);
    }

    player->m_dashAngle = dashAngle;
    player->m_lastGroundedPos = cocos2d::CCPoint{0.f, 0.f};
}

void stopDashingForTrajectory(PlayerObject* player) {
    if (!player || !player->m_isDashing)
        return;

    player->m_isDashing = false;
    player->m_lastLandTime = 0.0;
    if (player->m_isPlatformer && player->m_dashRing) {
        cocos2d::CCPoint boosted = {
            static_cast<float>(player->m_dashX * player->m_dashRing->m_endBoost),
            static_cast<float>(player->m_dashY * player->m_dashRing->m_endBoost),
        };
        player->m_isAccelerating = true;
        player->m_yVelocity = boosted.y;
        player->m_platformerXVelocity = boosted.x;
        player->m_affectedByForces = true;

        if (player->m_dashRing->m_stopSlide) {
            player->m_isAccelerating = false;
            player->m_affectedByForces = false;
        }
    }
    player->m_dashRing = nullptr;

    if (player->m_isPlatformer) {
        float normal = std::sqrt(player->m_dashX * player->m_dashX + player->m_dashY * player->m_dashY);
        if (normal <= 17.3f)
            normal = (normal / 17.3f) * 1.5f + 0.5f;
        else
            normal = 2.f;

        float sizeMod = player->m_vehicleSize == 1.f ? 0.433333f : 0.333333f;
        float sidewaysMod = player->m_isSideways ? -1.f : 1.f;
        int gravityMod = player->m_isUpsideDown ? -0xb4 : 0xb4;
        int leftMod = player->m_isGoingLeft ? -1 : 1;
        player->m_rotationSpeed = (gravityMod * leftMod * sidewaysMod * player->m_gravityMod * normal) / sizeMod;
        player->m_isRotating = true;
    }

    if (player->m_isBall) {
        if (player->m_isPlatformer) {
            player->m_isRotating = false;
            player->m_isBallRotating = false;
            player->m_rotationSpeed = 0.0;
        }
        player->runBallRotation(1.0);
    }
}

void togglePlayerScaleForTrajectory(PlayerObject* player, bool smallSize) {
    if (!player)
        return;
    if (player->m_vehicleSize == 1.f && !smallSize)
        return;
    if (smallSize && player->m_vehicleSize != 1.f)
        return;

    if (smallSize) {
        player->m_vehicleSize = 0.6f;
    } else {
        if (player->m_isPlatformer)
            player->m_stateScale = 2;
        player->m_vehicleSize = 1.f;
    }

    player->m_spriteWidthScale = player->m_vehicleSize;
    player->m_spriteHeightScale = player->m_vehicleSize;
    player->setScaleX(player->m_vehicleSize);
    player->setScaleY(player->m_vehicleSize);
    player->updatePlayerScale();
}

void switchGameModeForTrajectory(PlayerObject* player, GameObjectType portalType) {
    if (!player)
        return;

    bool wasBall = player->m_isBall;
    bool wasFlying = player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;

    // Every gamemode boolean gets explicitly set here, so an unrecognized
    // or CubePortal type correctly clears all of them back to cube instead
    // of silently falling through and leaving the previous mode's flags set.
    player->m_isShip = portalType == GameObjectType::ShipPortal;
    player->m_isBall = portalType == GameObjectType::BallPortal;
    player->m_isBird = portalType == GameObjectType::UfoPortal;
    player->m_isDart = portalType == GameObjectType::WavePortal;
    player->m_isRobot = portalType == GameObjectType::RobotPortal;
    player->m_isSpider = portalType == GameObjectType::SpiderPortal;
    player->m_isSwing = portalType == GameObjectType::SwingPortal;

    bool nowFlying = player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;

    clearCollisionState(player);
    player->m_isOnGround = false;
    player->m_isOnGround2 = false;
    player->m_isOnSlope = false;
    player->m_wasOnSlope = false;
    player->m_isAccelerating = false;

    if (!player->m_isBall && !player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating2 = false;
        player->m_isBallRotating = false;
        player->m_rotationSpeed = 0.0;
        player->runNormalRotation(0, 1.0);
    } else {
        player->runBallRotation2();
    }

    if (player->m_isBall && !wasBall)
        player->runBallRotation2();

    // Matches how GD caps vertical speed when a fall/rise carries you into
    // a flying mode mid-air; ground modes keep whatever velocity they had,
    // the next physics step settles it against the ground normally.
    if (nowFlying && !wasFlying)
        player->m_yVelocity = std::clamp(player->m_yVelocity, -8.0, 8.0);

    if (player->m_isSpider)
        spiderTestJumpForTrajectory(player);

    player->m_lastGroundedPos = player->getPosition();
}

void activatePortalForTrajectory(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal) {
    if (!layer || !player || !portal || !layer->canBeActivatedByPlayer(player, portal))
        return;

    layer->playerWillSwitchMode(player, portal);

    cocos2d::CCPoint position = player->getPosition();
    player->switchedToMode(portal->m_objectType);
    switchGameModeForTrajectory(player, portal->m_objectType);

    player->setPosition(position);
    if (player->m_iconSprite)
        player->m_iconSprite->setPosition(position);
    if (player->m_vehicleSprite)
        player->m_vehicleSprite->setPosition(position);
    player->m_lastPortalPos = portal->getPosition();
    player->m_lastActivatedPortal = portal;
    activateForTrajectory(portal, player);
}

void bumpPlayerForTrajectory(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* object) {
    if (!layer || !player || !object || !layer->canBeActivatedByPlayer(player, object))
        return;

    float force = 1.f;
    if (object->m_objectType == GameObjectType::PinkJumpPad) {
        if (player->m_isShip)
            force = 0.35f;
        else if (player->m_isBird)
            force = 0.4f;
        else if (player->m_isBall || player->m_isSpider)
            force = 0.7f;
        else
            force = 0.65f;
    } else if (object->m_objectType == GameObjectType::RedJumpPad) {
        if (player->m_isShip)
            force = player->m_vehicleSize >= 1.f ? 0.63f : 0.95f;
        else if (player->m_isBird)
            force = player->m_vehicleSize >= 1.f ? 0.6f : 0.98f;
        else
            force = 1.25f;
    }

    player->m_lastPortalPos = object->getPosition();
    player->m_lastActivatedPortal = object;
    activateForTrajectory(object, player);
    bumpFakePlayerForTrajectory(player, force, static_cast<int>(object->m_objectType), true, object);
}

void ringJumpForTrajectory(PlayerObject* player, RingObject* ring) {
    if (!player || !ring || player->m_isDead)
        return;
    if (player->m_ringRelatedSet.find(ring->m_uniqueID) != player->m_ringRelatedSet.end())
        return;
    if (!player->m_stateRingJump2 || player->m_isDashing || !player->m_stateJumpBuffered)
        return;

    bool custom = ring->m_objectType == GameObjectType::CustomRing;
    bool teleport = ring->m_objectType == GameObjectType::TeleportOrb;
    if ((player->m_touchedRing || custom || teleport) &&
        (player->m_touchedCustomRing || ring->m_objectType != GameObjectType::CustomRing)) {
        if (player->m_touchedGravityPortal || ring->m_objectType != GameObjectType::TeleportOrb)
            return;
    }

    if (player->m_touchingRings && !player->m_touchingRings->containsObject(ring))
        player->m_touchingRings->addObject(ring);
    player->m_touchedRings.insert(ring->m_uniqueID);

    ShowTrajectory::snapshotObject(ring);
    ShowTrajectory::rememberActivated(player, ring);

    if (ring->m_isReverse)
        player->reversePlayer(ring);

    player->m_ringJumpRelated = true;
    player->m_ringRelatedSet.insert(ring->m_uniqueID);
    if (custom)
        player->m_touchedCustomRing = true;
    else if (teleport)
        player->m_touchedGravityPortal = true;
    else
        player->m_touchedRing = true;

    if (player->m_touchingRings)
        player->m_touchingRings->removeObject(ring, true);
    if (!custom && !teleport)
        player->m_padRingRelated = true;

    if (custom)
        return;

    if (teleport) {
        if (auto* teleportPortal = typeinfo_cast<TeleportPortalObject*>(ring))
            teleportPlayerForTrajectory(player->m_gameLayer, teleportPortal, player);
        return;
    }

    if (ring->m_objectType == GameObjectType::SpiderOrb) {
        bool facing = player->m_isSideways ? ring->isFacingLeft() : ring->isFacingDown();
        if (facing != player->m_isUpsideDown)
            flipGravityForTrajectory(player, !player->m_isUpsideDown);
        spiderTestJumpForTrajectory(player);
        return;
    }

    if (ring->m_objectType == GameObjectType::DashRing) {
        if (auto* dashRing = typeinfo_cast<DashRingObject*>(ring))
            startDashingForTrajectory(player, dashRing);
        return;
    }

    if (ring->m_objectType == GameObjectType::GravityDashRing) {
        flipGravityForTrajectory(player, !player->m_isUpsideDown);
        if (auto* dashRing = typeinfo_cast<DashRingObject*>(ring))
            startDashingForTrajectory(player, dashRing);
        return;
    }

    player->m_stateRingJump = false;
    if (ring->m_objectType == GameObjectType::DropRing) {
        float velocity = gravitySign(player) * -15.f;
        if (player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing)
            velocity = gravitySign(player) * -14.f;
        if (player->m_isBird)
            velocity *= 0.8f;
        else if (!player->m_isRobot && player->m_isSpider)
            velocity *= 1.1f;
        player->setYVelocity(velocity, 1);
        if (!player->m_isBall && !player->m_isLocked && !player->m_isDashing) {
            player->m_isRotating = false;
            player->m_isBallRotating2 = false;
            player->m_isBallRotating = false;
            player->m_rotationSpeed = 0.0;
            player->runNormalRotation(0, 1.0);
        } else {
            player->runBallRotation2();
        }
        player->m_hasEverHitRing = true;
        player->m_isAccelerating = true;
        if (player->m_isBall || player->m_isSwing)
            player->m_jumpBuffered = false;
        return;
    }

    float yStart = player->m_yStart;
    if (ring->m_objectType == GameObjectType::GravityRing)
        yStart *= 0.8f;
    else if (ring->m_objectType == GameObjectType::GreenRing && player->m_isShip)
        yStart *= 0.7f;
    else if (ring->m_objectType == GameObjectType::PinkJumpRing)
        yStart *= player->m_isShip ? 0.37f : player->m_isBird ? 0.42f : player->m_isBall ? 0.77f : 0.72f;
    else if (ring->m_objectType == GameObjectType::RedJumpRing) {
        player->m_isAccelerating = true;
        yStart *= player->m_isShip ? 1.4f :
            player->m_isBird ? (player->m_vehicleSize == 1.f ? 1.02f : 1.36f) :
            player->m_isBall ? 1.34f :
            player->m_isRobot ? 1.28f :
            player->m_isSpider ? 1.34f :
            1.38f;
    } else if (player->m_isRobot) {
        yStart *= 0.9f;
    }

    if (ring->m_objectType == GameObjectType::GreenRing)
        flipGravityForTrajectory(player, !player->m_isUpsideDown);

    float sizeGravity = player->m_vehicleSize != 1.f ? 0.8f : 1.f;
    player->setYVelocity(gravitySign(player) * yStart * sizeGravity, gravitySign(player));
    player->m_maybeIsBoosted = true;
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    player->m_lastGroundedPos = player->getPosition();
    player->m_hasEverHitRing = true;

    if (!player->m_isBall && !player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating2 = false;
        player->m_isBallRotating = false;
        player->m_rotationSpeed = 0.0;
        player->runNormalRotation(0, 1.0);
    } else {
        player->runBallRotation2();
    }

    if (player->m_isSwing)
        player->m_yVelocity *= 0.6;
    else if (player->m_isBall || player->m_isSpider)
        player->m_yVelocity *= 0.7;

    if (ring->m_objectType == GameObjectType::GravityRing)
        flipGravityForTrajectory(player, !player->m_isUpsideDown);
    if (ring->m_objectType == GameObjectType::RedJumpRing)
        player->m_isAccelerating = true;
}

void redirectPlayerForceForTrajectory(
    PlayerObject* player,
    float force,
    float,
    float,
    float
) {
    if (!player)
        return;

    cocos2d::CCPoint velocity = {
        static_cast<float>(player->m_platformerXVelocity),
        static_cast<float>(player->m_yVelocity)
    };
    cocos2d::CCPoint redirected = {};

    float angle = force * 0.017453292f - std::atan2(velocity.y, velocity.x);
    if (angle != 0.f) {
        float s = std::sin(angle);
        float c = std::cos(angle);
        redirected = cocos2d::CCPoint{
            (velocity.x * s) - (velocity.y * c),
            (velocity.x * c) + (velocity.y * s)
        };
    }

    if (player->m_isSideways)
        std::swap(redirected.x, redirected.y);

    player->m_yVelocity = redirected.y;
    player->m_isAccelerating = true;
    if (player->m_isPlatformer) {
        player->m_platformerXVelocity = redirected.x;
        player->m_affectedByForces = true;
    }
}

void teleportPlayerForTrajectory(GJBaseGameLayer* layer, TeleportPortalObject* object, PlayerObject* player) {
    if (!layer || !object || !player)
        return;

    player->m_wasTeleported = true;
    if (object->m_orangePortal) {
        cocos2d::CCPoint bluePortalStartPos = object->getStartPos();
        cocos2d::CCPoint orangePortalStartPos = object->m_orangePortal->getStartPos();
        object->m_teleportYOffset = orangePortalStartPos.y - bluePortalStartPos.y;
    }

    cocos2d::CCPoint playerPos = player->getPosition();
    TeleportPortalObject* destination = object->m_orangePortal;
    if (!destination && object->m_targetGroupID > 0) {
        if (auto* group = layer->getGroup(object->m_targetGroupID)) {
            unsigned int groupLength = group->count();
            if (groupLength > 0) {
                float selector = groupLength == 1 ? 0.f : static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
                if (selector == 1.f)
                    selector = 0.f;
                destination = static_cast<TeleportPortalObject*>(
                    group->objectAtIndex(static_cast<unsigned int>(selector * groupLength))
                );
            }
        }
    }

    if (destination) {
        cocos2d::CCPoint portalPos = object->getPosition();
        cocos2d::CCPoint destinationPos;
        player->m_lastPortalPos = object->getPosition();
        player->m_lastActivatedPortal = object;
        player->m_fallStartY = 0.0;

        if (object->m_objectID == 0x2eb) {
            float dy = object->m_teleportYOffset;
            portalPos = object->getRealPosition();
            destinationPos = cocos2d::CCPoint{player->getPosition().x, portalPos.y + dy};
        } else {
            destinationPos = destination->getPosition();
        }

        if (object->m_saveOffset) {
            portalPos = object->getRealPosition();
            portalPos -= playerPos;
            destinationPos -= portalPos;
        }
        if (object->m_ignoreX)
            destinationPos.x = playerPos.x;
        if (object->m_ignoreY)
            destinationPos.y = playerPos.y;

        player->setPosition(destinationPos);
    }

    if (object->m_gravityMode > 0) {
        bool gravity = object->m_gravityMode == 2;
        if (object->m_gravityMode == 3)
            gravity = !player->m_isUpsideDown;
        flipGravityForTrajectory(player, gravity);
    }

    float forceAngle;
    if (destination) {
        float gravityForceMod = 90.f;
        if (object->m_objectID == 0x26 ||
            object->m_objectID == 0x2eb ||
            object->m_objectID == 0x2ed ||
            object->m_objectID == 0x810 ||
            object->m_objectID == 0xb56) {
            gravityForceMod = 180.f;
        }

        float flipMod = static_cast<float>(object->isFlipX() ? 0xb4 : 1);
        forceAngle = flipMod + (gravityForceMod - destination->getRotationX());
    } else {
        float flipMod = static_cast<float>(object->isFlipX() ? 0xb4 : 1);
        forceAngle = flipMod - object->getRotationX();
    }

    if (object->m_redirectForceEnabled) {
        redirectPlayerForceForTrajectory(
            player,
            forceAngle,
            object->m_redirectForceMod,
            object->m_redirectForceMin,
            object->m_redirectForceMax
        );
    } else if (object->m_staticForceEnabled) {
        float force = object->m_staticForce;
        if (force == 0.f && !object->m_staticForceAdditive) {
            player->m_isAccelerating = false;
            player->m_yVelocity = 0.0;
            if (player->m_isPlatformer) {
                player->m_platformerXVelocity = 0.0;
                player->m_affectedByForces = false;
            }
        } else {
            float forceMod = forceAngle * 0.01745329f;
            cocos2d::CCPoint direction = {std::cos(forceMod), std::sin(forceMod)};
            float length = direction.getLength();
            if (length > 0.f) {
                cocos2d::CCPoint forceVector = direction * (force / length);
                if (player->m_isSideways)
                    std::swap(forceVector.x, forceVector.y);

                player->m_isAccelerating = true;
                double newYVelocity = forceVector.y;
                if (object->m_staticForceAdditive)
                    newYVelocity += player->m_yVelocity;
                player->m_yVelocity = newYVelocity;

                if (player->m_isPlatformer) {
                    double newXVelocity = forceVector.x;
                    if (object->m_staticForceAdditive)
                        newXVelocity += player->m_platformerXVelocity;
                    player->m_platformerXVelocity = newXVelocity;
                    player->m_affectedByForces = true;
                }
            }
        }
    }

    if (object->m_snapGround)
        player->m_lastGroundedPos = player->getPosition();
}

void triggerObjectForTrajectory(EffectGameObject* object, GJBaseGameLayer* layer, PlayerObject* player) {
    if (!object || !layer || !player || !isTrajectorySpawnObject(object))
        return;

    switch (object->m_objectID) {
    case 200:
        layer->m_gameState.m_timeModRelated = 0.7f;
        break;
    case 201:
        layer->m_gameState.m_timeModRelated = 0.9f;
        break;
    case 202:
        layer->m_gameState.m_timeModRelated = 1.1f;
        break;
    case 203:
        layer->m_gameState.m_timeModRelated = 1.3f;
        break;
    case 1334:
        layer->m_gameState.m_timeModRelated = 1.6f;
        break;
    case 2066: {
        if (object->m_followCPP)
            break;

        bool isP2 = ShowTrajectory::fakePlayer(false) == player;
        if (!object->m_targetPlayer2 && !isP2)
            player->m_gravityMod = object->m_gravityValue;
        if (object->m_targetPlayer2 && isP2)
            player->m_gravityMod = object->m_gravityValue;
        break;
    }
    case 2900: {
        auto* rotate = typeinfo_cast<RotateGameplayGameObject*>(object);
        if (!rotate)
            break;

        player->rotateGameplay(
            rotate->m_moveDirection,
            rotate->m_groundDirection,
            rotate->m_editVelocity,
            rotate->m_velocityModX,
            rotate->m_velocityModY,
            rotate->m_overrideVelocity,
            rotate->m_dontSlide
        );
        break;
    }
    case 3022: {
        auto* portal = typeinfo_cast<TeleportPortalObject*>(object);
        if (portal)
            teleportPlayerForTrajectory(layer, portal, player);
        break;
    }
    default:
        break;
    }

    activateForTrajectory(object, player);
}

bool shouldSkipTrajectoryObject(GameObject* object) {
    if (!object)
        return true;

    return object->m_objectType == GameObjectType::Decoration ||
        object->m_objectType == GameObjectType::CollisionObject ||
        object->m_objectType == GameObjectType::SecretCoin ||
        object->m_objectType == GameObjectType::UserCoin ||
        object->m_objectType == GameObjectType::Collectible ||
        object->m_objectType == GameObjectType::EnterEffectObject ||
        object->m_objectID == 286 ||
        object->m_objectID == 287 ||
        object->m_isGroupDisabled ||
        object->m_isDisabled;
}

bool isSolidTrajectoryObject(GameObject* object) {
    return object && (
        object->m_objectType == GameObjectType::Solid ||
        object->m_objectType == GameObjectType::Breakable
    );
}

bool isHazardTrajectoryObject(GameObject* object) {
    return object && (
        object->m_objectType == GameObjectType::Hazard ||
        object->m_objectType == GameObjectType::AnimatedHazard
    );
}

bool trajectoryObjectIntersectsPlayer(
    GJBaseGameLayer* layer,
    PlayerObject* player,
    GameObject* object,
    cocos2d::CCRect const& playerRect
) {
    cocos2d::CCRect objectRect = object->m_objectType == GameObjectType::Slope ?
        object->getObjectRect(2.f, 2.f) :
        object->getObjectRect();

    if (object->m_objectRadius <= 0.f) {
        if (!playerRect.intersectsRect(objectRect))
            return false;
    } else if (!layer->playerCircleCollision(player, object)) {
        return false;
    }

    if (!object->m_shouldUseOuterOb ||
        (layer->m_levelSettings->m_fixRadiusCollision && object->m_objectRadius > 0.f)) {
        return true;
    }

    OBB2D* objectBox = object->getOrientedBox();
    player->updateOrientedBox();
    OBB2D* playerBox = player->GameObject::getOrientedBox();
    return objectBox && playerBox && objectBox->overlaps1Way(playerBox);
}

void handleSlopeCollisionForTrajectory(PlayerObject* player, GameObject* object, float dt) {
    if (!player->m_isSideways) {
        player->collidedWithSlopeInternal(dt, object, false);
        return;
    }

    cocos2d::CCRect emptyRect = {0.f, 0.f, 0.f, 0.f};
    player->handleRotatedCollisionInternal(dt, object, emptyRect, false, false, true);
}

bool handleGravityPadForTrajectory(GJBaseGameLayer* layer, PlayerObject* player, GameObject* object, EffectGameObject* effect) {
    bool facingDown = player->m_isSideways ? object->isFacingLeft() : object->isFacingDown();
    if (player->m_isUpsideDown != facingDown || !layer->canBeActivatedByPlayer(player, effect))
        return false;

    if (effect->m_isReverse)
        player->reversePlayer(effect);
    rememberPortalActivation(player, effect);
    propellPlayerForTrajectory(player, 0.8f, true, static_cast<int>(GameObjectType::GravityPad));
    flipGravityForTrajectory(player, !facingDown);
    player->m_padRingRelated = true;
    return true;
}

bool handleSizePortalForTrajectory(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* effect, bool smallSize) {
    if (!layer->canBeActivatedByPlayer(player, effect))
        return false;

    rememberPortalActivation(player, effect);
    togglePlayerScaleForTrajectory(player, smallSize);
    return true;
}

bool handleSpecialObjectForTrajectory(PlayerObject* player, GameObject* object) {
    if (object->m_objectID == 0x743)
        player->m_stateHitHead = 2;
    else if (object->m_objectID == 0x6db)
        player->m_stateDartSlide = 2;
    else if (object->m_objectID == 0x715)
        player->m_stateNoAutoJump = 2;
    else if (object->m_objectID == 0x725 && player->m_isDashing) {
        stopDashingForTrajectory(player);
        player->m_jumpBuffered = false;
    } else if (object->m_objectID == 0xb32)
        player->m_stateFlipGravity = 2;
    else if (object->m_objectID == 2069 || object->m_objectID == 3645) {
        player->m_stateForce = 2;
        auto* forceBlock = typeinfo_cast<ForceBlockGameObject*>(object);
        if (!forceBlock)
            return false;

        int forceID = forceBlock->m_forceID;
        if (forceID > 0) {
            if (player->m_jumpPadRelated.contains(forceID) &&
                player->m_jumpPadRelated.at(forceID)) {
                return false;
            }
            player->m_jumpPadRelated.insert({forceID, true});
        }
        player->m_stateForceVector += forceBlock->calculateForceToTarget(player);
    }

    return false;
}

bool handleEffectCollisionForTrajectory(
    GJBaseGameLayer* layer,
    PlayerObject* player,
    GameObject* object,
    EffectGameObject* effect
) {
    switch (object->m_objectType) {
    case GameObjectType::InverseGravityPortal:
        rememberPortalActivation(player, effect);
        flipGravityForTrajectory(player, true);
        return true;
    case GameObjectType::NormalGravityPortal:
        rememberPortalActivation(player, effect);
        flipGravityForTrajectory(player, false);
        return true;
    case GameObjectType::GravityTogglePortal:
        rememberPortalActivation(player, effect);
        flipGravityForTrajectory(player, !player->m_isUpsideDown);
        return true;
    case GameObjectType::TeleportPortal:
        if (layer->canBeActivatedByPlayer(player, effect)) {
            if (auto* portal = typeinfo_cast<TeleportPortalObject*>(object))
                teleportPlayerForTrajectory(layer, portal, player);
            activateForTrajectory(effect, player);
            return true;
        }
        break;
    case GameObjectType::CustomRing:
    case GameObjectType::DashRing:
    case GameObjectType::DropRing:
    case GameObjectType::GravityDashRing:
    case GameObjectType::GravityRing:
    case GameObjectType::GreenRing:
    case GameObjectType::PinkJumpRing:
    case GameObjectType::RedJumpRing:
    case GameObjectType::SpiderOrb:
    case GameObjectType::YellowJumpRing:
    case GameObjectType::TeleportOrb:
        if (auto* ring = typeinfo_cast<RingObject*>(object))
            ringJumpForTrajectory(player, ring);
        break;
    case GameObjectType::YellowJumpPad:
    case GameObjectType::PinkJumpPad:
    case GameObjectType::RedJumpPad:
    case GameObjectType::SpiderPad:
        bumpPlayerForTrajectory(layer, player, effect);
        return true;
    case GameObjectType::GravityPad:
        return handleGravityPadForTrajectory(layer, player, object, effect);
    case GameObjectType::MiniSizePortal:
        return handleSizePortalForTrajectory(layer, player, effect, true);
    case GameObjectType::RegularSizePortal:
        return handleSizePortalForTrajectory(layer, player, effect, false);
    case GameObjectType::CubePortal:
    case GameObjectType::ShipPortal:
    case GameObjectType::BallPortal:
    case GameObjectType::UfoPortal:
    case GameObjectType::WavePortal:
    case GameObjectType::SpiderPortal:
    case GameObjectType::SwingPortal:
    case GameObjectType::RobotPortal:
        activatePortalForTrajectory(layer, player, effect);
        return true;
    case GameObjectType::Special:
        return handleSpecialObjectForTrajectory(player, object);
    case GameObjectType::EnterEffectObject:
    case GameObjectType::Modifier:
        ShowTrajectory::snapshotObject(effect);
        effect->activatedByPlayer(player);
        if (effect->m_isTouchTriggered)
            triggerObjectForTrajectory(effect, layer, player);
        break;
    default:
        break;
    }

    return false;
}

void queueCollisionObject(gd::vector<GameObject*>& objects, int& count, int& capacity, GameObject* object) {
    if (count < capacity)
        objects.at(count) = object;
    else {
        objects.push_back(object);
        capacity++;
    }
    count++;
}

void collisionCheckObjectsForTrajectory(
    GJBaseGameLayer* layer,
    PlayerObject* player,
    gd::vector<GameObject*>* objects,
    int objectCount,
    float dt
) {
    if (!layer || !player || !objects || objectCount <= 0)
        return;

    auto refreshPlayerRect = [&player]() {
        return player->getObjectRect();
    };

    cocos2d::CCRect playerRect = refreshPlayerRect();
    for (int i = 0; i < objectCount; i++) {
        GameObject* object = objects->at(i);
        if (shouldSkipTrajectoryObject(object))
            continue;

        if (isSolidTrajectoryObject(object)) {
            queueCollisionObject(
                layer->m_solidCollisionObjects,
                layer->m_solidCollisionObjectsCount,
                layer->m_solidCollisionObjectsIndex,
                object
            );
            continue;
        }

        if (object == layer->m_anticheatSpike)
            continue;

        if (isHazardTrajectoryObject(object)) {
            queueCollisionObject(
                layer->m_hazardCollisionObjects,
                layer->m_hazardCollisionObjectsCount,
                layer->m_hazardCollisionObjectsIndex,
                object
            );
            continue;
        }

        if (!trajectoryObjectIntersectsPlayer(layer, player, object, playerRect))
            continue;

        if (object->m_objectType == GameObjectType::Slope) {
            handleSlopeCollisionForTrajectory(player, object, dt);
            playerRect = refreshPlayerRect();
            continue;
        }

        auto* effect = typeinfo_cast<EffectGameObject*>(object);
        if (!effect)
            continue;

        if (hasTrajectoryActivatedObject(player, effect))
            continue;

        if (handleEffectCollisionForTrajectory(layer, player, object, effect))
            playerRect = refreshPlayerRect();
    }
}

void checkSpawnObjectsForTrajectory(GJBaseGameLayer* layer, PlayerObject* player) {
    if (!layer || !player || !layer->m_spawnObjects)
        return;

    cocos2d::CCArray* objects = static_cast<cocos2d::CCArray*>(
        layer->m_spawnObjects->objectForKey(layer->m_gameState.m_currentChannel)
    );
    if (!objects)
        return;

    int startingIndex = layer->m_gameState.m_spawnChannelRelated0.at(
        layer->m_gameState.m_currentChannel
    );
    bool goingBack = layer->m_gameState.m_spawnChannelRelated1.at(
        layer->m_gameState.m_currentChannel
    );

    cocos2d::CCPoint position = player->getPosition();
    for (int i = startingIndex; static_cast<unsigned int>(i) < objects->count(); i++) {
        auto* object = static_cast<SpawnTriggerGameObject*>(objects->objectAtIndex(i));
        if (!isTrajectorySpawnObject(object))
            continue;

        cocos2d::CCPoint objectPos = object->m_speedStart;
        if (player->m_isSideways) {
            if (goingBack) {
                if (objectPos.y < position.y)
                    break;
            } else if (objectPos.y > position.y) {
                break;
            }
        } else {
            if (goingBack) {
                if (objectPos.x < position.x)
                    break;
            } else if (objectPos.x > position.x) {
                break;
            }
        }

        if (object->m_isGroupDisabled)
            continue;
        if (ShowTrajectory::hasActivated(player, object) ||
            ShowTrajectory::realPlayerHasActivated(player, object)) {
            continue;
        }

        if (!object->m_isTouchTriggered)
            triggerObjectForTrajectory(object, layer, player);
    }
}


bool stepSimulationFrame(
    PlayLayer* pl,
    PlayerObject* mainPlayer,
    PlayerObject* otherPlayer,
    bool simulateBothPlayers,
    ShowTrajectory::BranchResult& branch,
    cocos2d::CCDrawNode* drawNode,
    float width,
    cocos2d::ccColor4F color,
    cocos2d::ccColor4F otherColor
) {
    if (!pl || !mainPlayer)
        return true;

    cocos2d::CCPoint prevMain = mainPlayer->getPosition();
    cocos2d::CCPoint prevOther = otherPlayer ? otherPlayer->getPosition() : cocos2d::CCPoint{};
    float delta = advanceGameClock(pl, mainPlayer, simulateBothPlayers ? otherPlayer : nullptr);

    updateFakePlayer(mainPlayer, delta);
    if (simulateBothPlayers)
        updateFakePlayer(otherPlayer, delta);

    if (!ShowTrajectory::fakePlayerDead(mainPlayer) &&
        pl->checkCollisions(mainPlayer, delta, false) == 1) {
        ShowTrajectory::markFakePlayerDead(mainPlayer);
    }

    if (simulateBothPlayers && otherPlayer && !ShowTrajectory::fakePlayerDead(otherPlayer) &&
        pl->checkCollisions(otherPlayer, delta, false) == 1) {
        ShowTrajectory::markFakePlayerDead(otherPlayer);
    }

    checkSpawnObjectsForTrajectory(pl, mainPlayer);
    if (simulateBothPlayers && otherPlayer && !ShowTrajectory::fakePlayerDead(otherPlayer))
        checkSpawnObjectsForTrajectory(pl, otherPlayer);

    if (pl->m_effectManager)
        pl->m_effectManager->postCollisionCheck();

    if (!ShowTrajectory::fakePlayerDead(mainPlayer))
        drawPredictionSegment(drawNode, mainPlayer, prevMain, width, color);

    if (simulateBothPlayers && otherPlayer && !ShowTrajectory::fakePlayerDead(otherPlayer))
        drawPredictionSegment(drawNode, otherPlayer, prevOther, width, otherColor);

    return ShowTrajectory::fakePlayerDead(mainPlayer) &&
        (!simulateBothPlayers || !otherPlayer || ShowTrajectory::fakePlayerDead(otherPlayer));
}

void drawStoredHitbox(ShowTrajectory::PredictionResult const& prediction, cocos2d::CCDrawNode* drawNode);

ShowTrajectory::BranchResult runPredictionBranch(
    PlayLayer* pl,
    bool primaryPlayer,
    int mode,
    bool simulateBothPlayers,
    ShowTrajectory::PredictionConfig config
) {
    ShowTrajectory::BranchResult branch;
    branch.prediction.player1 = primaryPlayer;
    branch.prediction.holding = (mode & ShowTrajectory::Hold) != 0;
    if (!pl)
        return branch;

    PlayerObject* predictionPlayer = ShowTrajectory::fakePlayer(primaryPlayer);
    PlayerObject* sourcePlayer = primaryPlayer ? pl->m_player1 : pl->m_player2;
    PlayerObject* otherPredictionPlayer = ShowTrajectory::fakePlayer(!primaryPlayer);
    PlayerObject* otherSourcePlayer = primaryPlayer ? pl->m_player2 : pl->m_player1;
    if (!predictionPlayer || !sourcePlayer)
        return branch;

    LayerStateGuard state(pl);

    resetFakePlayerFrom(sourcePlayer, predictionPlayer);
    if (simulateBothPlayers && otherPredictionPlayer && otherSourcePlayer)
        resetFakePlayerFrom(otherSourcePlayer, otherPredictionPlayer);

    auto& t = trajectory();
    t.cancelTrajectory = false;
    t.m_deadP1 = false;
    t.m_deadP2 = false;
    t.m_activatedObjectsP1.clear();
    t.m_activatedObjectsP2.clear();
    t.m_objSnapshot.clear();
    t.m_snapshotObjects.clear();

    cocos2d::ccColor4F color = colorForMode(mode);
    cocos2d::ccColor4F otherColor = invertedColor(color);

    int predictionLength = std::min(std::max(t.length, 0), std::max(config.maxLength, 0));
    auto* drawNode = config.draw ? ShowTrajectory::trajectoryNode() : nullptr;
    float width = t.width;
    if (pl->m_gameState.m_cameraZoom > 0.f)
        width /= pl->m_gameState.m_cameraZoom;
    bool applyReplayInputs = shouldApplyReplayInputsForPrediction(config);
    size_t replayInputIndex = 0;
    if (applyReplayInputs)
        replayInputIndex = Bot::get().currentAction;

    uint64_t startFrame = static_cast<uint64_t>(Bot::getCurrentFrame());
    if (!applyReplayInputs) {
        cocos2d::CCPoint initialMainPosition = predictionPlayer->getPosition();
        cocos2d::CCPoint initialOtherPosition = otherPredictionPlayer ?
            otherPredictionPlayer->getPosition() :
            cocos2d::CCPoint{};

        applyInitialInput(pl, predictionPlayer, sourcePlayer, mode);
        if (mode != 0)
            drawPredictionSegment(drawNode, predictionPlayer, initialMainPosition, width, color);

        if (mode != 0 && simulateBothPlayers && otherPredictionPlayer && otherSourcePlayer) {
            applyInitialInput(pl, otherPredictionPlayer, otherSourcePlayer, mode);
            drawPredictionSegment(drawNode, otherPredictionPlayer, initialOtherPosition, width, otherColor);
        }
    }

    for (int i = 0; i < predictionLength; i++) {
        if (i >= predictionLength - 40)
            color.a = (predictionLength - i) / 40.f;
        otherColor.a = color.a;

        applyReplayInputsForPrediction(
            pl,
            replayInputIndex,
            startFrame + static_cast<uint64_t>(i),
            simulateBothPlayers,
            applyReplayInputs
        );

        if (simulateBothPlayers && !pl->m_gameState.m_isDualMode)
            simulateBothPlayers = false;

        branch.stopped = stepSimulationFrame(
            pl,
            predictionPlayer,
            otherPredictionPlayer,
            simulateBothPlayers,
            branch,
            drawNode,
            width,
            color,
            otherColor
        );
        branch.prediction.score++;
        if (branch.stopped)
            break;
    }

    t.m_deathRotation = predictionPlayer->getRotation();
    branch.prediction.position = predictionPlayer->getPosition();
    branch.prediction.hitbox = predictionPlayer->getObjectRect();
    branch.prediction.innerHitbox = predictionPlayer->getObjectRect(0.3, 0.3);
    branch.prediction.rotation = predictionPlayer->getRotation();
    branch.hasHitbox = true;
    if (drawNode)
        drawStoredHitbox(branch.prediction, drawNode);
    ShowTrajectory::hideFakePlayer(predictionPlayer);
    if (otherPredictionPlayer)
        ShowTrajectory::hideFakePlayer(otherPredictionPlayer);

    return branch;
}

void runPredictionBranchesForPlayer(
    PlayLayer* pl,
    bool primaryPlayer,
    bool simulateBothPlayers,
    bool includePlatformerDirections
) {
    ShowTrajectory::PredictionConfig config;
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Hold, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Swift, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Release, simulateBothPlayers, config);

    if (!includePlatformerDirections)
        return;

    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Hold | ShowTrajectory::Left, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Swift | ShowTrajectory::Left, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Release | ShowTrajectory::Left, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Hold | ShowTrajectory::Right, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Swift | ShowTrajectory::Right, simulateBothPlayers, config);
    runPredictionBranch(pl, primaryPlayer, ShowTrajectory::Release | ShowTrajectory::Right, simulateBothPlayers, config);
}

std::vector<cocos2d::CCPoint> rectVertices(cocos2d::CCRect rect) {
    return {
        {rect.getMinX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMinY()},
        {rect.getMinX(), rect.getMinY()},
    };
}

void rotateVertices(std::vector<cocos2d::CCPoint>& vertices, cocos2d::CCPoint center, float rotation) {
    float angle = CC_DEGREES_TO_RADIANS(rotation * -1.f);
    for (auto& vertex : vertices) {
        float x = vertex.x - center.x;
        float y = vertex.y - center.y;
        vertex.x = center.x + (x * std::cos(angle)) - (y * std::sin(angle));
        vertex.y = center.y + (x * std::sin(angle)) + (y * std::cos(angle));
    }
}

void drawStoredHitbox(ShowTrajectory::PredictionResult const& prediction, cocos2d::CCDrawNode* drawNode) {
    auto drawRect = [&](cocos2d::CCRect rect, float rotation, cocos2d::ccColor4F fill, cocos2d::ccColor4F outline, float width) {
        auto vertices = rectVertices(rect);
        cocos2d::CCPoint center = {rect.getMidX(), rect.getMidY()};
        rotateVertices(vertices, center, rotation);
        drawNode->drawPolygon(vertices.data(), vertices.size(), fill, width, outline);
    };

    auto& t = trajectory();
    drawRect(prediction.hitbox, prediction.rotation, cocos2d::ccc4f(t.color2.r, t.color2.g, t.color2.b, 0.2f), t.color2, 0.5f);
    drawRect(
        prediction.innerHitbox,
        prediction.rotation,
        cocos2d::ccc4f(t.color3.r, t.color3.g, t.color3.b, 0.2f),
        cocos2d::ccc4f(t.color3.r, t.color3.g, t.color3.b, 0.55f),
        0.35f
    );
}

}

$execute {
    auto& t = trajectory();
    t.color1 = ccc4FFromccc3B(Mod::get()->getSavedValue<cocos2d::ccColor3B>("trajectory_color1"));
    t.color2 = ccc4FFromccc3B(Mod::get()->getSavedValue<cocos2d::ccColor3B>("trajectory_color2"));
    t.length = geode::utils::numFromString<int>(Mod::get()->getSavedValue<std::string>("trajectory_length")).unwrapOr(0);
    t.width = Mod::get()->getSavedValue<float>("trajectory_width");
    if (t.width <= 0.f)
        t.width = 0.5f;
    t.updateMergedColor();
};

void ShowTrajectory::clearTrajectory() {
    if (auto* node = trajectoryNode(false)) {
        node->clear();
        node->setVisible(false);
    }
}

ShowTrajectory::PredictionResult ShowTrajectory::simulate(
    PlayLayer* pl,
    bool player1,
    int mode,
    bool simulateBothPlayers,
    PredictionConfig config
) {
    PredictionResult result;
    result.player1 = player1;
    result.holding = (mode & Hold) != 0;
    auto& t = trajectory();
    if (!pl || !pl->m_player1)
        return result;
    initForLayer(pl);

    auto& bot = Bot::get();
    bool wasCreatingTrajectory = t.creatingTrajectory;
    bool wasBotCreatingTrajectory = bot.creatingTrajectory;
    t.creatingTrajectory = true;
    bot.creatingTrajectory = true;

    if (ensureFakePlayer(pl, player1)) {
        bool canSimulateBoth = simulateBothPlayers && pl->m_gameState.m_isDualMode &&
            !pl->m_levelSettings->m_twoPlayerMode;
        if (canSimulateBoth && pl->m_player2)
            ensureFakePlayer(pl, false);

        if (config.draw) {
            if (auto* node = trajectoryNode()) {
                node->clear();
                node->setVisible(true);
            }
        }
        auto branch = runPredictionBranch(pl, player1, mode, canSimulateBoth, config);
        result = branch.prediction;
    }

    hideFakePlayer(t.m_fakePlayer1);
    hideFakePlayer(t.m_fakePlayer2);
    t.creatingTrajectory = wasCreatingTrajectory;
    bot.creatingTrajectory = wasBotCreatingTrajectory;
    return result;
}

void ShowTrajectory::updateTrajectory(PlayLayer* pl) {
    auto& t = trajectory();
    if (!pl || !pl->m_player1)
        return;

    geode::utils::Timer totalTimer;
    auto& bot = Bot::get();
    if (!bot.showTrajectory) {
        clearTrajectory();
        return;
    }
    initForLayer(pl);
    auto* node = trajectoryNode(false);
    if (!node)
        return;
    node->clear();
    node->setVisible(true);
    bool platformerBothSides = bot.trajectoryBothSides;
    bool previousSafeMode = bot.safeMode;

    bot.safeMode = true;
    t.creatingTrajectory = true;
    bot.creatingTrajectory = true;

    if (ensureFakePlayer(pl, true)) {
        bool simulateBoth = pl->m_gameState.m_isDualMode && !pl->m_levelSettings->m_twoPlayerMode;
        if (simulateBoth && pl->m_player2)
            ensureFakePlayer(pl, false);

        runPredictionBranchesForPlayer(
            pl,
            true,
            simulateBoth,
            pl->m_levelSettings->m_platformerMode && platformerBothSides
        );
    }

    if (pl->m_player2 && pl->m_gameState.m_isDualMode && pl->m_levelSettings->m_twoPlayerMode) {
        if (ensureFakePlayer(pl, false)) {
            runPredictionBranchesForPlayer(
                pl,
                false,
                false,
                pl->m_levelSettings->m_platformerMode && platformerBothSides
            );
        }
    }

    hideFakePlayer(t.m_fakePlayer1);
    hideFakePlayer(t.m_fakePlayer2);

    auto totalMs = totalTimer.elapsed<>();
    g_lastTrajectoryRefreshMs = totalMs;
    if (shouldLogSlowTrajectory(totalMs)) {
        log::info(
            "Trajectory slow: total={}ms branches={} length={} dual={} tp={} bothSides={}",
            totalMs,
            pl->m_levelSettings->m_platformerMode && platformerBothSides ? 18 : 6,
            t.length,
            pl->m_gameState.m_isDualMode,
            pl->m_levelSettings ? pl->m_levelSettings->m_twoPlayerMode : false,
            platformerBothSides
        );
    }

    t.creatingTrajectory = false;
    bot.creatingTrajectory = false;
    bot.safeMode = previousSafeMode;
}

PlayerObject* ShowTrajectory::fakePlayer(bool player1) {
    auto& t = trajectory();
    return player1 ? t.m_fakePlayer1 : t.m_fakePlayer2;
}

PlayerObject* ShowTrajectory::otherFakePlayer(PlayerObject* player) {
    auto& t = trajectory();
    if (player == t.m_fakePlayer1)
        return t.m_fakePlayer2;
    if (player == t.m_fakePlayer2)
        return t.m_fakePlayer1;
    return nullptr;
}

bool ShowTrajectory::isFakePlayer(PlayerObject* player) {
    auto& t = trajectory();
    return player && (player == t.m_fakePlayer1 || player == t.m_fakePlayer2);
}

void ShowTrajectory::markFakePlayerDead(PlayerObject* player) {
    auto& t = trajectory();
    if (player == t.m_fakePlayer1) {
        t.m_deadP1 = true;
        t.m_deathRotation = player->getRotation();
    } else if (player == t.m_fakePlayer2) {
        t.m_deadP2 = true;
        t.m_deathRotation = player->getRotation();
    }
}

bool ShowTrajectory::fakePlayerDead(PlayerObject* player) {
    auto& t = trajectory();
    if (player == t.m_fakePlayer1)
        return t.m_deadP1;
    if (player == t.m_fakePlayer2)
        return t.m_deadP2;
    return false;
}

PlayerObject* ShowTrajectory::ensureFakePlayer(PlayLayer* pl, bool player1) {
    if (!pl)
        return nullptr;

    auto& t = trajectory();
    initForLayer(pl);

    auto& fake = player1 ? t.m_fakePlayer1 : t.m_fakePlayer2;
    if (!fake) {
        auto* player = PlayerObject::create(1, 1, pl, pl, true);
        if (!player)
            return nullptr;

        fake = player;
        fake->retain();
        fake->setPosition({0.f, 105.f});
        fake->setID(player1 ? "xdbot-trajectory-fake-player1" : "xdbot-trajectory-fake-player2");
        if (pl->m_objectLayer) {
            pl->m_objectLayer->addChild(fake);
            log::debug("Trajectory fake player created: player{} fake={}", player1 ? 1 : 2, ptrId(fake));
        } else {
            log::warn("Trajectory fake player created without objectLayer: player{} fake={}", player1 ? 1 : 2, ptrId(fake));
        }
    } else if (!fake->getParent() && pl->m_objectLayer) {
        pl->m_objectLayer->addChild(fake);
    }

    hideFakePlayer(fake);
    return fake;
}

void ShowTrajectory::hideFakePlayer(PlayerObject* player) {
    if (!player)
        return;

    player->setVisible(false);
    player->setOpacity(0);
    player->m_playEffects = false;
    player->m_maybeReducedEffects = true;
}

bool ShowTrajectory::hasActivated(PlayerObject* player, EnhancedGameObject* object) {
    auto& t = trajectory();
    if (!player || !object || object->m_isMultiActivate)
        return false;

    auto key = reinterpret_cast<uintptr_t>(object);
    if (player == t.m_fakePlayer1)
        return t.m_activatedObjectsP1.contains(key);
    if (player == t.m_fakePlayer2)
        return t.m_activatedObjectsP2.contains(key);
    return false;
}

bool ShowTrajectory::realPlayerHasActivated(PlayerObject* player, EnhancedGameObject* object) {
    if (!player || !object)
        return false;

    auto hasBeenActivatedByPlayer = [](PlayerObject* p, EnhancedGameObject* obj) -> bool {
        if (!p || !obj)
            return false;
        if ((!p->m_isPlatformer && obj->m_isMultiActivate) || (p->m_isPlatformer && obj->m_isNoMultiActivate))
            return false;
        return !p->m_isSecondPlayer ? obj->m_activatedByPlayer1 : obj->m_activatedByPlayer2;
    };

    if (!isFakePlayer(player))
        return hasBeenActivatedByPlayer(player, object);

    auto* pl = PlayLayer::get();
    if (!pl)
        return false;

    PlayerObject* realPlayer = player == fakePlayer(true) ? pl->m_player1 : pl->m_player2;
    return hasBeenActivatedByPlayer(realPlayer, object);
}

void ShowTrajectory::rememberActivated(PlayerObject* player, EnhancedGameObject* object) {
    auto& t = trajectory();
    if (!player || !object)
        return;

    auto key = reinterpret_cast<uintptr_t>(object);
    if (player == t.m_fakePlayer1)
        t.m_activatedObjectsP1.insert_or_assign(key, true);
    else if (player == t.m_fakePlayer2)
        t.m_activatedObjectsP2.insert_or_assign(key, true);
}

void ShowTrajectory::snapshotObject(GameObject* object) {
    auto& t = trajectory();
    if (!object)
        return;

    auto key = reinterpret_cast<uintptr_t>(object);
    if (t.m_snapshotObjects.contains(key))
        return;
    t.m_snapshotObjects.insert(key);

    auto* effect = typeinfo_cast<EffectGameObject*>(object);
    auto* ring = typeinfo_cast<RingObject*>(object);
    t.m_objSnapshot.push_back({
        object,
        ring,
        effect,
        effect ? effect->m_isActivated : false,
        effect ? effect->m_activatedByPlayer1 : false,
        effect ? effect->m_activatedByPlayer2 : false,
        ring ? ring->m_claimTouch : false,
        object->m_isDisabled,
        object->m_isDisabled2,
        object->getOpacity(),
        object->isVisible(),
    });
}

void ShowTrajectory::restoreSnapshots() {
    auto& t = trajectory();
    for (auto const& snapshot : t.m_objSnapshot) {
        if (!snapshot.obj)
            continue;

        if (snapshot.effect) {
            snapshot.effect->m_isActivated = snapshot.isActivated;
            snapshot.effect->m_activatedByPlayer1 = snapshot.activatedByP1;
            snapshot.effect->m_activatedByPlayer2 = snapshot.activatedByP2;
        }
        snapshot.obj->m_isDisabled = snapshot.isDisabled;
        snapshot.obj->m_isDisabled2 = snapshot.isDisabled2;
        snapshot.obj->setOpacity(snapshot.opacity);
        snapshot.obj->setVisible(snapshot.visible);

        if (snapshot.ring)
            snapshot.ring->m_claimTouch = snapshot.claimTouch;
    }

    t.m_objSnapshot.clear();
    t.m_snapshotObjects.clear();
}

std::vector<cocos2d::CCPoint> ShowTrajectory::getVertices(
    PlayerObject* player,
    cocos2d::CCRect rect,
    float rotation
) {
    auto vertices = rectVertices(rect);
    cocos2d::CCPoint center = {rect.getMidX(), rect.getMidY()};
    float size = static_cast<int>(rect.getMaxX() - rect.getMinX());

    if (player && (size == 18 || size == 5) && player->getScale() == 1.f) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) / 0.6f;
            vertex.y = center.y + (vertex.y - center.y) / 0.6f;
        }
    }

    if (player && (size == 7 || size == 30 || size == 29 || size == 9) && player->getScale() != 1.f) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) * 0.6f;
            vertex.y = center.y + (vertex.y - center.y) * 0.6f;
        }
    }

    if (player && player->m_isDart) {
        for (auto& vertex : vertices) {
            vertex.x = center.x + (vertex.x - center.x) * 0.3f;
            vertex.y = center.y + (vertex.y - center.y) * 0.3f;
        }
    }

    rotateVertices(vertices, center, rotation);
    return vertices;
}

void ShowTrajectory::drawPlayerHitbox(
    PlayerObject* player,
    cocos2d::CCDrawNode* drawNode,
    cocos2d::ccColor4F outer,
    cocos2d::ccColor4F inner
) {
    if (!player || !drawNode)
        return;

    auto vertices = getVertices(player, player->GameObject::getObjectRect(), player->getRotation());
    drawNode->drawPolygon(vertices.data(), vertices.size(), cocos2d::ccc4f(outer.r, outer.g, outer.b, 0.2f), 0.5f, outer);

    vertices = getVertices(player, player->GameObject::getObjectRect(0.3, 0.3), player->getRotation());
    drawNode->drawPolygon(vertices.data(), vertices.size(), cocos2d::ccc4f(inner.r, inner.g, inner.b, 0.2f), 0.35f, cocos2d::ccc4f(inner.r, inner.g, inner.b, 0.55f));
}

void ShowTrajectory::updateMergedColor() {
    cocos2d::ccColor4F newColor = {0.f, 0.f, 0.f, 1.f};
    newColor.r = (color1.r + color2.r) / 2.f;
    newColor.g = (color1.g + color2.g) / 2.f;
    newColor.b = (color1.b + color2.b) / 2.f;
    newColor.r = std::min(1.f, newColor.r + 0.45f);
    newColor.g = std::min(1.f, newColor.g + 0.45f);
    newColor.b = std::min(1.f, newColor.b + 0.45f);
    color3 = newColor;
}

void ShowTrajectory::setColor1(cocos2d::ccColor3B color) {
    color1 = ccc4FFromccc3B(color);
    updateMergedColor();
}

void ShowTrajectory::setColor2(cocos2d::ccColor3B color) {
    color2 = ccc4FFromccc3B(color);
    updateMergedColor();
}

cocos2d::CCDrawNode* ShowTrajectory::trajectoryNode(bool create) {
    auto& t = trajectory();
    if (!create)
        return t.m_node;

    if (!t.m_node) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return nullptr;

        initForLayer(pl);
    }

    return t.m_node;
}

void ShowTrajectory::initForLayer(PlayLayer* pl) {
    auto& t = trajectory();
    if (!pl)
        return;

    if (!t.m_node) {
        t.m_node = TrajectoryNode::create();
        if (t.m_node) {
            t.m_node->retain();
            t.m_node->setID("xdbot-trajectory-node");
            t.m_node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            t.m_node->setVisible(false);
        }
    }

    if (t.m_node && pl->m_debugDrawNode && pl->m_debugDrawNode->getParent() && !t.m_node->getParent()) {
        int zOrder = pl->m_uiLayer ? pl->m_uiLayer->getZOrder() + 10000 : 10000;
        pl->m_debugDrawNode->getParent()->addChild(t.m_node, zOrder);
    }
}

void ShowTrajectory::uninit() {
    auto& t = trajectory();
    t.cancelTrajectory = false;
    t.creatingTrajectory = false;
    t.m_deadP1 = false;
    t.m_deadP2 = false;
    t.m_deathRotation = 0.f;
    t.m_activatedObjectsP1.clear();
    t.m_activatedObjectsP2.clear();
    t.m_objSnapshot.clear();
    t.m_snapshotObjects.clear();
    t.m_node = nullptr;
    t.m_fakePlayer1 = nullptr;
    t.m_fakePlayer2 = nullptr;
}

class $modify(TrajectoryPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        ShowTrajectory::uninit();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        ShowTrajectory::initForLayer(this);
        return true;
    }

    void playGravityEffect(bool flip) {
        if (!trajectory().creatingTrajectory)
            PlayLayer::playGravityEffect(flip);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        ShowTrajectory::initForLayer(this);
    }

    void resetLevel() {
        log::debug("Trajectory resetLevel begin: pl={} nodeExists={}", ptrId(this), ptrId(ShowTrajectory::trajectoryNode(false)));
        PlayLayer::resetLevel();
        ShowTrajectory::initForLayer(this);
        ShowTrajectory::updateTrajectory(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (ShowTrajectory::isFakePlayer(player)) {
            ShowTrajectory::markFakePlayerDead(player);
            trajectory().cancelTrajectory = true;
            return;
        }

        if (trajectory().creatingTrajectory)
            return;

        PlayLayer::destroyPlayer(player, gameObject);
    }

    void onQuit() {
        ShowTrajectory::uninit();
        PlayLayer::onQuit();
    }

    void playEndAnimationToPos(cocos2d::CCPoint pos) {
        if (!trajectory().creatingTrajectory)
            PlayLayer::playEndAnimationToPos(pos);
    }
};

class $modify(TrajectoryPauseLayer, PauseLayer) {
    void goEdit() {
        ShowTrajectory::uninit();
        PauseLayer::goEdit();
    }
};

class $modify(TrajectoryBaseGameLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (ShowTrajectory::isFakePlayer(player)) {
            collisionCheckObjectsForTrajectory(this, player, objects, objectCount, dt);
            return;
        }

        GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (ShowTrajectory::isFakePlayer(player)) {
            teleportPlayerForTrajectory(this, object, player);
            return;
        }

        GJBaseGameLayer::teleportPlayer(object, player);
    }

    void flipGravity(PlayerObject* player, bool gravity, bool noEffects) {
        if (ShowTrajectory::isFakePlayer(player)) {
            flipGravityForTrajectory(player, gravity);
            return;
        }

        GJBaseGameLayer::flipGravity(player, gravity, noEffects);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (ShowTrajectory::isFakePlayer(player)) {
            if (ShowTrajectory::hasActivated(player, object) ||
                ShowTrajectory::realPlayerHasActivated(player, object)) {
                return false;
            }

            ShowTrajectory::snapshotObject(object);
            bool activated = GJBaseGameLayer::canBeActivatedByPlayer(player, object);
            if (activated)
                ShowTrajectory::rememberActivated(player, object);
            return activated;
        }

        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (ShowTrajectory::isFakePlayer(player)) {
            ShowTrajectory::snapshotObject(ring);
            ringJumpForTrajectory(player, ring);
            return;
        }

        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (ShowTrajectory::isFakePlayer(player)) {
            triggerObjectForTrajectory(object, this, player);
            return;
        }

        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (!trajectory().creatingTrajectory)
            GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (!trajectory().creatingTrajectory)
            GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void gameEventTriggered(GJGameEvent event, int p1, int p2) {
        if (!trajectory().creatingTrajectory)
            GJBaseGameLayer::gameEventTriggered(event, p1, p2);
    }

    void destroyObject(GameObject* object) {
        if (trajectory().creatingTrajectory)
            return;

        GJBaseGameLayer::destroyObject(object);
    }
};

class $modify(TrajectoryPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (!trajectory().creatingTrajectory)
            PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (ShowTrajectory::isFakePlayer(this))
            return;
        if (!trajectory().creatingTrajectory)
            PlayerObject::incrementJumps();
    }

    void playDeathEffect() {
        if (!trajectory().creatingTrajectory)
            PlayerObject::playDeathEffect();
    }

    void playSpawnEffect() {
        if (!trajectory().creatingTrajectory)
            PlayerObject::playSpawnEffect();
    }

    void ringJump(RingObject* ring, bool unk) {
        if (ShowTrajectory::isFakePlayer(this)) {
            ringJumpForTrajectory(this, ring);
            return;
        }

        PlayerObject::ringJump(ring, unk);
    }

    void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
        if (ShowTrajectory::isFakePlayer(this)) {
            bumpFakePlayerForTrajectory(this, force, objectType, playEffect, object);
            return;
        }

        PlayerObject::bumpPlayer(force, objectType, playEffect, object);
    }

    void propellPlayer(float force, bool dontPlayEffect, int objectType) {
        if (ShowTrajectory::isFakePlayer(this)) {
            propellPlayerForTrajectory(this, force, dontPlayEffect, objectType);
            return;
        }

        PlayerObject::propellPlayer(force, dontPlayEffect, objectType);
    }

    void startDashing(DashRingObject* object) {
        if (ShowTrajectory::isFakePlayer(this)) {
            startDashingForTrajectory(this, object);
            return;
        }

        PlayerObject::startDashing(object);
    }

    void togglePlayerScale(bool smallSize, bool noEffects) {
        if (ShowTrajectory::isFakePlayer(this)) {
            togglePlayerScaleForTrajectory(this, smallSize);
            return;
        }

        PlayerObject::togglePlayerScale(smallSize, noEffects);
    }

    void flipGravity(bool gravity, bool noEffects) {
        if (ShowTrajectory::isFakePlayer(this)) {
            flipGravityForTrajectory(this, gravity);
            return;
        }

        PlayerObject::flipGravity(gravity, noEffects);
    }

    void stopDashing() {
        if (ShowTrajectory::isFakePlayer(this)) {
            stopDashingForTrajectory(this);
            return;
        }

        PlayerObject::stopDashing();
    }
};

class $modify(TrajectoryHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (!trajectory().creatingTrajectory)
            HardStreak::addPoint(point);
    }
};

class $modify(TrajectoryGameObject, GameObject) {
    void playShineEffect() {
        if (!trajectory().creatingTrajectory)
            GameObject::playShineEffect();
    }
};

class $modify(TrajectoryEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int id, gd::vector<int> const* groups) {
        if (trajectory().creatingTrajectory) {
            ShowTrajectory::snapshotObject(this);
            return;
        }

        EffectGameObject::triggerObject(layer, id, groups);
    }
};
