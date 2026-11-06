#pragma once

// Fixed-width representations of the *original* Vice City 1.0 EN ABI.
// Layout facts verified against Plugin-SDK 15f15b60bbf74c106e1b496ff92c98764abf4605.
// These are shadow objects, never aliases of native reVC/librw classes.
#include <cstddef>
#include <cstdint>

namespace LegacyAsi {
namespace VC10 {

#pragma pack(push, 1)
struct Vector { float x, y, z; };
struct Matrix {
    Vector right; std::uint32_t flags;
    Vector forward; std::uint32_t pad1;
    Vector up; std::uint32_t pad2;
    Vector position; std::uint32_t pad3;
    std::uint32_t attachment, ownsAttachment;
};
struct Cam {
    std::uint8_t belowMinDistance, behindPlayerDesired, lookingAtVector, collisionChecks;
    std::uint8_t earlyFlags[8];
    std::int16_t mode;
    std::uint8_t gap00E[0xB4 - 0xE];
    float fov;
    std::uint8_t gap0B8[0x12C - 0xB8];
    Vector fixedTarget, fixedSource, fixedUp;
    Vector lastAboveWater, bufferedPlayerBodyOffset;
    Vector front, source, sourceBeforeLookBehind, up;
    Vector previousVectors[2];
    std::uint32_t targetEntity;
    float cameraDistance, idealAlpha, playerVelocity;
    std::uint32_t lastCarEntered, lastPedLookedAt;
    std::uint8_t firstPersonRunAbout, tail[3];
};
struct Camera {
    Matrix placeable;
    std::uint8_t gap048[0x5B - 0x48];
    std::uint8_t lookingAtPlayer, lookingAtVector;
    std::uint8_t gap05D[0x6C - 0x5D];
    std::uint8_t wantsWidescreenOff, widescreen;
    std::uint8_t gap06E[0x76 - 0x6E];
    std::uint8_t activeCam;
    std::uint8_t gap077[0x88 - 0x77];
    std::uint8_t transitionJustStarted, transitionState;
    std::uint8_t gap08A[0xD0 - 0x8A];
    std::int32_t controller;
    std::uint8_t gap0D4[0x188 - 0xD4];
    Cam cams[3];
    std::uint8_t gap6EC[0x730 - 0x6EC];
    Vector fixedTarget, fixedSource, fixedUp;
    std::uint8_t gap754[0x808 - 0x754];
    std::uint32_t rwCamera, targetEntity, pathArrays[4];
    Matrix cameraMatrix;
    std::uint8_t gap868[0x948 - 0x868];
    std::int16_t modeToGoTo, musicFadingDirection, typeOfSwitch;
    std::uint8_t tail94E[0x958 - 0x94E];
};
struct Controller { std::int16_t value[21]; };
struct Pad {
    Controller newState, oldState;
    std::int16_t steeringHistory[10];
    std::int32_t drunkHistoryUsed;
    Controller keyboard, joystick, mouse;
    std::int16_t phase, mode, shakeDuration;
    std::uint16_t disablePlayerControls;
    std::uint8_t shakeFrequency, hornHistory[5], hornPosition, justOutOfFrontend, applyBrakes;
    std::uint8_t cheatString[12], pad107;
    std::int32_t lastTimeTouched, averageWeapon, averageEntries;
};
struct Mouse {
    std::uint8_t left, right, middle, wheelUp, wheelDown, extra1, extra2, padding;
    float x, y;
};
struct Menu {
    std::uint8_t gap000[0x38];
    std::uint8_t active, wantRestart, firstTime, saveMenuActive, wantLoad;
    std::uint8_t gap03D[0x6C - 0x3D];
    std::uint8_t gameNotLoaded;
    std::uint8_t tail06D[0x684 - 0x6D];
};
// Only the non-virtual CPlaceable matrix view is supported for this proxy.
// Unsupported virtual calls are directed to a diagnostic failure, not a stub.
struct Entity {
    std::uint32_t vtable;
    Matrix placeable;
    std::uint32_t rwObject;
    std::uint8_t flags[8];
    std::uint16_t scanCode, randomSeed, modelIndex;
    std::uint8_t level, area;
    std::uint32_t firstReference;
};
struct Weapon {
    std::int32_t type, state;
    std::uint32_t ammoInClip, ammoTotal, nextShotTime;
    std::uint8_t addRotationOffset, padding[3];
};
// Only the fields independently audited in the original inventory ABI are
// exposed. In particular, model/animation/RW pointers remain opaque zeros.
struct PlayerPed {
    Entity entity;
    std::uint8_t gap064[0x244 - 0x64];
    std::int32_t pedState;
    std::uint8_t gap248[0x354 - 0x248];
    float health;
    std::uint8_t gap358[0x408 - 0x358];
    Weapon weapons[10];
    std::uint8_t gap4F8[0x504 - 0x4F8];
    std::uint8_t currentWeapon;
    std::uint8_t gap505[0x60C - 0x505];
    std::uint8_t selectedWeapon;
    std::uint8_t tail60D[0x6D8 - 0x60D];
};
#pragma pack(pop)

static_assert(sizeof(Vector) == 0xC && sizeof(Matrix) == 0x48);
static_assert(offsetof(Matrix, position) == 0x30);
static_assert(sizeof(Cam) == 0x1CC);
static_assert(offsetof(Cam, mode) == 0xC && offsetof(Cam, fov) == 0xB4);
static_assert(offsetof(Cam, fixedTarget) == 0x12C && offsetof(Cam, front) == 0x168);
static_assert(offsetof(Cam, source) == 0x174 && offsetof(Cam, up) == 0x18C);
static_assert(sizeof(Camera) == 0x958);
static_assert(offsetof(Camera, activeCam) == 0x76 && offsetof(Camera, controller) == 0xD0);
static_assert(offsetof(Camera, cams) == 0x188 && offsetof(Camera, fixedTarget) == 0x730);
static_assert(offsetof(Camera, rwCamera) == 0x808 && offsetof(Camera, cameraMatrix) == 0x820);
static_assert(offsetof(Camera, modeToGoTo) == 0x948 && offsetof(Camera, typeOfSwitch) == 0x94C);
static_assert(sizeof(Controller) == 0x2A && sizeof(Pad) == 0x114);
static_assert(offsetof(Pad, disablePlayerControls) == 0xF0);
static_assert(sizeof(Mouse) == 0x10 && offsetof(Mouse, x) == 8);
static_assert(sizeof(Menu) == 0x684 && offsetof(Menu, active) == 0x38);
static_assert(offsetof(Menu, wantLoad) == 0x3C && offsetof(Menu, gameNotLoaded) == 0x6C);
static_assert(sizeof(Entity) == 0x64 && offsetof(Entity, placeable) == 4);
static_assert(offsetof(Entity, placeable) + offsetof(Matrix, position) == 0x34);
static_assert(sizeof(Weapon) == 0x18 && offsetof(Weapon, nextShotTime) == 0x10);
static_assert(sizeof(PlayerPed) == 0x6D8 && offsetof(PlayerPed, entity) == 0);
static_assert(offsetof(PlayerPed, pedState) == 0x244 && offsetof(PlayerPed, health) == 0x354);
static_assert(offsetof(PlayerPed, weapons) == 0x408 && offsetof(PlayerPed, currentWeapon) == 0x504);
static_assert(offsetof(PlayerPed, selectedWeapon) == 0x60C);

constexpr std::uintptr_t CameraAddress = 0x7E4688;
constexpr std::uintptr_t PadsAddress = 0x7DBCB0;
constexpr std::uintptr_t MenuAddress = 0x869630;
constexpr std::uintptr_t MouseAddress = 0x94D788;
constexpr std::uintptr_t TemporaryMouseAddress = 0x7DD860;
} // namespace VC10

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
using FullSync = void (*)();
using CameraTranslator = void* (*)(void* nativeCamera);

// Map before FinalizeMemory; callbacks must synchronize all scalar/object/RW
// state, because a thiscall adapter may be nested inside an ASI event callback.
bool MapObjects(FullSync pullBeforeNative, FullSync pushAfterNative, CameraTranslator cameraToLegacy);
void PushObjects();
void PullObjects();
const char* ObjectsError();

// Caller-owned ordinary native object pointers never escape into the ASI.
// FindPlayerEntity resolves to the same shadow as FindPlayerPed while on foot;
// a separate read-position vehicle token is used when the player is in a car.
void* PlayerEntityToLegacy();
void* PlayerPedToLegacy();
void* PadToLegacy(void* nativePad);
#endif
} // namespace LegacyAsi
