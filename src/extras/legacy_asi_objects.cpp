#include "legacy_asi_objects.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"
#include "Camera.h"
#include "Entity.h"
#include "Frontend.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Weapon.h"
#include "Vehicle.h"
#include "main.h"
#include "legacy_asi_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace LegacyAsi {
namespace {

static_assert(sizeof(void*) == 4 && sizeof(bool) == 1);
static_assert(CCam::MODE_FIXED == 15, "The fixed-camera mode value must match VC 1.0");
static_assert(TOTAL_WEAPON_SLOTS == 10 && WEAPONTYPE_CAMERA == 36);
static_assert(std::is_trivially_copyable_v<CControllerState>);
static_assert(offsetof(CControllerState, NetworkTalk) == 0x28,
    "Translate controller members explicitly if their native ordering changes");

FullSync pullAll, pushAll;
CameraTranslator translateCamera;
VC10::Camera* camera;
VC10::Pad* pads;
VC10::Menu* menu;
VC10::Mouse* mouse;
VC10::Mouse* temporaryMouse;
VC10::Camera oldCamera;
VC10::Pad oldPads[2];
VC10::Menu oldMenu;
VC10::Mouse oldMouse;
VC10::Mouse oldTemporaryMouse;
VC10::PlayerPed playerPed, oldPlayerPed;
VC10::Entity vehicleEntity;
CPlayerPed* synchronizedPlayer;
void (*entityVtable[64])();
bool mapped;
unsigned rejectedWrites;
char errorText[256];

bool Fail(const char* message)
{
    std::snprintf(errorText, sizeof(errorText), "%s", message);
    return false;
}

[[noreturn]] void BadObject(const char* message)
{
    std::fprintf(stderr, "[legacy-asi] Unsupported object operation: %s\n", message);
    RaiseException(0xE04C4153, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

void Rejected(const char* field)
{
    if (rejectedWrites++ < 16)
        std::fprintf(stderr, "[legacy-asi] Rejected invalid object field: %s\n", field);
}

VC10::Vector VectorFromNative(const CVector& vector) { return {vector.x, vector.y, vector.z}; }
CVector NativeVector(const VC10::Vector& vector) { return CVector(vector.x, vector.y, vector.z); }
bool Finite(const VC10::Vector& vector)
{
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}
template<class T> bool Changed(const T& current, const T& previous)
{
    return std::memcmp(&current, &previous, sizeof(T)) != 0;
}
bool MergeVector(const VC10::Vector& value, const VC10::Vector& previous, CVector& native, const char* name)
{
    if (!Changed(value, previous)) return false;
    if (!Finite(value)) { Rejected(name); return false; }
    native = NativeVector(value);
    return true;
}
void MergeBool(std::uint8_t value, std::uint8_t previous, bool& native, const char* name)
{
    if (value == previous) return;
    if (value <= 1) native = value != 0;
    else Rejected(name);
}
void MergeFov(float value, float previous, float& native)
{
    if (!Changed(value, previous)) return;
    if (std::isfinite(value) && value > 1.0f && value < 179.0f) native = value;
    else Rejected("CCam::FOV");
}
void MergeMode(std::int16_t value, std::int16_t previous, int16& native)
{
    if (value == previous) return;
    if (value >= CCam::MODE_NONE && value <= CCam::MODE_CAMERA) native = value;
    else Rejected("camera mode");
}

VC10::Matrix MatrixFromNative(CMatrix& native)
{
    VC10::Matrix result = {};
    result.right = VectorFromNative(native.GetRight());
    result.forward = VectorFromNative(native.GetForward());
    result.up = VectorFromNative(native.GetUp());
    result.position = VectorFromNative(native.GetPosition());
    // The original matrix's attachment points to another original RW object;
    // native librw pointers/ownership bits must never leak into this view.
    std::memcpy(&result.flags, &native.rw, sizeof(result.flags));
    return result;
}
void MergeMatrix(const VC10::Matrix& current, const VC10::Matrix& previous, CMatrix& native)
{
    bool changed = MergeVector(current.right, previous.right, native.GetRight(), "matrix::right");
    changed |= MergeVector(current.forward, previous.forward, native.GetForward(), "matrix::forward");
    changed |= MergeVector(current.up, previous.up, native.GetUp(), "matrix::up");
    changed |= MergeVector(current.position, previous.position, native.GetPosition(), "matrix::position");
    // Copy accepted direct writes outward to an attached native RW matrix.
    // Update() would do the opposite and read stale attachment values back.
    // UpdateRW itself safely handles an unattached CMatrix.
    if (changed) native.UpdateRW();
    if (current.attachment != previous.attachment || current.ownsAttachment != previous.ownsAttachment)
        Rejected("matrix attachment/ownership is not a native pointer");
}

void ReadController(VC10::Controller& target, const CControllerState& source)
{
    // The first 21 native short members are the original controller channels;
    // improved input appends bWalk, which is intentionally not exposed here.
    std::memcpy(target.value, &source.LeftStickX, sizeof(target.value));
}
void MergeController(const VC10::Controller& current, const VC10::Controller& previous, CControllerState& native)
{
    auto* bytes = reinterpret_cast<unsigned char*>(&native);
    for (unsigned i = 0; i < 21; ++i)
        if (current.value[i] != previous.value[i])
            std::memcpy(bytes + i * sizeof(std::int16_t), &current.value[i], sizeof(std::int16_t));
}

void PushPad(VC10::Pad& target, CPad& native)
{
    ReadController(target.newState, native.NewState);
    ReadController(target.oldState, native.OldState);
    ReadController(target.keyboard, native.PCTempKeyState);
    ReadController(target.joystick, native.PCTempJoyState);
    ReadController(target.mouse, native.PCTempMouseState);
    std::memcpy(target.steeringHistory, native.SteeringLeftRightBuffer, sizeof(target.steeringHistory));
    target.drunkHistoryUsed = native.DrunkDrivingBufferUsed;
    target.phase = native.Phase;
    target.mode = native.Mode;
    target.shakeDuration = native.ShakeDur;
    target.disablePlayerControls = native.DisablePlayerControls;
#ifdef IMPROVED_MENU_AND_INPUT
    target.shakeFrequency = native.ShakeHighFreq;
#else
    target.shakeFrequency = native.ShakeFreq;
#endif
    for (unsigned i = 0; i < 5; ++i) target.hornHistory[i] = native.bHornHistory[i];
    target.hornPosition = native.iCurrHornHistory;
    target.justOutOfFrontend = native.JustOutOfFrontend;
    target.applyBrakes = native.bApplyBrakes;
    std::memcpy(target.cheatString, native.CheatString, sizeof(target.cheatString));
    target.lastTimeTouched = native.LastTimeTouched;
    target.averageWeapon = native.AverageWeapon;
    target.averageEntries = native.AverageEntries;
}
void PullPad(const VC10::Pad& current, const VC10::Pad& previous, CPad& native)
{
    MergeController(current.newState, previous.newState, native.NewState);
    MergeController(current.oldState, previous.oldState, native.OldState);
    MergeController(current.keyboard, previous.keyboard, native.PCTempKeyState);
    MergeController(current.joystick, previous.joystick, native.PCTempJoyState);
    MergeController(current.mouse, previous.mouse, native.PCTempMouseState);
    if (current.disablePlayerControls != previous.disablePlayerControls)
        native.DisablePlayerControls = current.disablePlayerControls;
    if (current.mode != previous.mode) native.Mode = current.mode;
}

void PushMouse(VC10::Mouse& target, const CMouseControllerState& native)
{
    target.left = native.LMB; target.right = native.RMB; target.middle = native.MMB;
    target.wheelUp = native.WHEELUP; target.wheelDown = native.WHEELDN;
    target.extra1 = native.MXB1; target.extra2 = native.MXB2;
    target.x = native.x; target.y = native.y;
}
void PullMouse(const VC10::Mouse& current, const VC10::Mouse& previous, CMouseControllerState& native)
{
    MergeBool(current.left, previous.left, native.LMB, "mouse::left");
    MergeBool(current.right, previous.right, native.RMB, "mouse::right");
    MergeBool(current.middle, previous.middle, native.MMB, "mouse::middle");
    MergeBool(current.wheelUp, previous.wheelUp, native.WHEELUP, "mouse::wheelUp");
    MergeBool(current.wheelDown, previous.wheelDown, native.WHEELDN, "mouse::wheelDown");
    MergeBool(current.extra1, previous.extra1, native.MXB1, "mouse::extra1");
    MergeBool(current.extra2, previous.extra2, native.MXB2, "mouse::extra2");
    if (Changed(current.x, previous.x)) {
        if (std::isfinite(current.x)) native.x = current.x;
        else Rejected("mouse::x");
    }
    if (Changed(current.y, previous.y)) {
        if (std::isfinite(current.y)) native.y = current.y;
        else Rejected("mouse::y");
    }
}

std::int32_t OriginalPedState(PedState native)
{
    // reVC extensions insert states into the enum. Exposing their shifted
    // integers would make a living player look dead to an original ASI.
    std::int32_t result = static_cast<std::int32_t>(native);
#ifdef CUSTOM_SWIMMING
    if (native == PED_SWIM) return 1; // no original representation: idle
    if (native > PED_SWIM) --result;
#endif
#ifdef CLIMBING
    if (native == PED_CLIMBING) return 1;
    if (native > PED_CLIMBING) --result;
#endif
#ifdef CROUCH
    if (native == PED_ROLL) return 1;
    if (native > PED_ROLL) --result;
#endif
    return result;
}

void PushPlayer()
{
    CPlayerPed* native = FindPlayerPed();
    if (native != synchronizedPlayer) playerPed = {};
    synchronizedPlayer = native;
    if (!native) { oldPlayerPed = playerPed; return; }
    playerPed.entity.placeable = MatrixFromNative(native->GetMatrix());
    playerPed.entity.vtable = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entityVtable));
    playerPed.pedState = OriginalPedState(native->GetPedState());
    playerPed.health = native->m_fHealth;
    playerPed.currentWeapon = native->m_currentWeapon;
    playerPed.selectedWeapon = static_cast<std::uint8_t>(native->m_nSelectedWepSlot);
    for (unsigned i = 0; i < TOTAL_WEAPON_SLOTS; ++i) {
        const CWeapon& source = native->m_weapons[i];
        VC10::Weapon& target = playerPed.weapons[i];
        target.type = source.m_eWeaponType;
        target.state = source.m_eWeaponState;
        target.ammoInClip = static_cast<std::uint32_t>(source.m_nAmmoInClip);
        target.ammoTotal = static_cast<std::uint32_t>(source.m_nAmmoTotal);
        target.nextShotTime = source.m_nTimer;
        target.addRotationOffset = source.m_bAddRotOffset;
    }
    oldPlayerPed = playerPed;
}

void PullPlayer()
{
    CPlayerPed* native = FindPlayerPed();
    if (!native || native != synchronizedPlayer) return;
    // This profile reads health/state/position; no speculative writes to the
    // native ped lifecycle, transforms, vtable or unknown pointer fields.
    if (Changed(playerPed.entity, oldPlayerPed.entity) ||
        playerPed.pedState != oldPlayerPed.pedState || Changed(playerPed.health, oldPlayerPed.health))
        Rejected("player identity, position, health and ped state are read-only");
    for (unsigned i = 0; i < TOTAL_WEAPON_SLOTS; ++i) {
        const VC10::Weapon& current = playerPed.weapons[i];
        const VC10::Weapon& previous = oldPlayerPed.weapons[i];
        CWeapon& target = native->m_weapons[i];
        if (current.type != previous.type) {
            if (current.type >= WEAPONTYPE_UNARMED && current.type <= WEAPONTYPE_CAMERA)
                target.m_eWeaponType = static_cast<eWeaponType>(current.type);
            else Rejected("weapon::type");
        }
        if (current.state != previous.state) {
            if (current.state >= WEAPONSTATE_READY && current.state <= WEAPONSTATE_MELEE_MADECONTACT)
                target.m_eWeaponState = static_cast<eWeaponState>(current.state);
            else Rejected("weapon::state");
        }
        if (current.ammoInClip != previous.ammoInClip) {
            if (current.ammoInClip <= static_cast<std::uint32_t>(std::numeric_limits<int32>::max()))
                target.m_nAmmoInClip = static_cast<int32>(current.ammoInClip);
            else Rejected("weapon::ammoInClip");
        }
        if (current.ammoTotal != previous.ammoTotal) {
            if (current.ammoTotal <= static_cast<std::uint32_t>(std::numeric_limits<int32>::max()))
                target.m_nAmmoTotal = static_cast<int32>(current.ammoTotal);
            else Rejected("weapon::ammoTotal");
        }
        if (current.nextShotTime != previous.nextShotTime) target.m_nTimer = current.nextShotTime;
        MergeBool(current.addRotationOffset, previous.addRotationOffset, target.m_bAddRotOffset, "weapon::rotationOffset");
    }
    if (playerPed.currentWeapon != oldPlayerPed.currentWeapon) {
        if (playerPed.currentWeapon < TOTAL_WEAPON_SLOTS) native->m_currentWeapon = playerPed.currentWeapon;
        else Rejected("player::currentWeapon");
    }
    if (playerPed.selectedWeapon != oldPlayerPed.selectedWeapon) {
        if (playerPed.selectedWeapon < TOTAL_WEAPON_SLOTS || playerPed.selectedWeapon == 0xFF)
            native->m_nSelectedWepSlot = static_cast<int8>(playerPed.selectedWeapon);
        else Rejected("player::selectedWeapon");
    }
}

void __fastcall UnsupportedEntityVirtual(void*, void*) { BadObject("entity virtual method is not implemented"); }

void RequireCamera(void* object)
{
    if (!mapped || object != reinterpret_cast<void*>(VC10::CameraAddress))
        BadObject("CCamera this pointer does not identify the registered camera shadow");
}
void __fastcall SetFixedPosition(void* object, void*, const VC10::Vector* source, const VC10::Vector* up)
{
    RequireCamera(object);
    if (!source || !up || !Finite(*source) || !Finite(*up)) BadObject("invalid fixed camera vectors");
    pullAll();
    TheCamera.SetCamPositionForFixedMode(NativeVector(*source), NativeVector(*up));
    pushAll();
}
void __fastcall TakeControlNoEntity(void* object, void*, const VC10::Vector* target, int16 type, int32 controller)
{
    RequireCamera(object);
    if (!target || !Finite(*target) || controller < 0 || controller > 2 || type < 0 || type > 2)
        BadObject("invalid TakeControlNoEntity arguments");
    pullAll();
    TheCamera.TakeControlNoEntity(NativeVector(*target), type, controller);
    pushAll();
}
void __fastcall RestoreJumpCut(void* object, void*)
{
    RequireCamera(object); pullAll(); TheCamera.RestoreWithJumpCut(); pushAll();
}
void __fastcall WidescreenOn(void* object, void*)
{
    RequireCamera(object); pullAll(); TheCamera.SetWideScreenOn(); pushAll();
}
void __fastcall WidescreenOff(void* object, void*)
{
    RequireCamera(object); pullAll(); TheCamera.SetWideScreenOff(); pushAll();
}
void __fastcall DrawWidescreen(void* object, void*)
{
    RequireCamera(object); pullAll(); TheCamera.DrawBordersForWideScreen(); pushAll();
}
VC10::Pad* __cdecl GetPad(int index)
{
    if (!mapped || index < 0 || index >= 2) return nullptr;
    return pads + index;
}
void* __cdecl FindEntity() { return PlayerEntityToLegacy(); }
void* __cdecl FindPed()
{
    pullAll();
    pushAll();
    return PlayerPedToLegacy();
}
void* __cdecl FindVehicle()
{
    pullAll();
    CEntity* native = FindPlayerPed() ? FindPlayerVehicle() : nullptr;
    pushAll();
    if (!native) return nullptr;
    vehicleEntity.placeable = MatrixFromNative(native->GetMatrix());
    vehicleEntity.vtable = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entityVtable));
    return &vehicleEntity;
}
CPlayerPed* RequirePlayer(void* object)
{
    CPlayerPed* native = FindPlayerPed();
    if (!mapped || object != &playerPed || !native || native != synchronizedPlayer)
        BadObject("player this pointer does not identify the active player shadow");
    return native;
}
void __fastcall SetPlayerWeapon(void* object, void*, int slot)
{
    if (slot < 0 || slot >= TOTAL_WEAPON_SLOTS) BadObject("invalid player weapon slot");
    pullAll();
    CPlayerPed* native = RequirePlayer(object);
    native->SetCurrentWeapon(slot);
    pushAll();
}
void __fastcall ChangePlayerWeapon(void* object, void*, int slot)
{
    if (slot < 0 || slot >= TOTAL_WEAPON_SLOTS) BadObject("invalid player weapon slot");
    pullAll();
    CPlayerPed* native = RequirePlayer(object);
    native->MakeChangesForNewWeapon(slot);
    pushAll();
}
CPad* RequirePad(void* object)
{
    if (mapped)
        for (int i = 0; i < 2; ++i)
            if (object == pads + i) return CPad::GetPad(i);
    BadObject("pad this pointer does not identify a registered pad shadow");
}
int __fastcall LookLeftRight(void* object, void*)
{
    pullAll();
    const int result = RequirePad(object)->LookAroundLeftRight();
    pushAll();
    return result;
}
int __fastcall LookUpDown(void* object, void*)
{
    pullAll();
    const int result = RequirePad(object)->LookAroundUpDown();
    pushAll();
    return result;
}
void __cdecl UpdateMouse()
{
    pullAll();
    CPad::GetPad(0)->UpdateMouse();
    pushAll();
}
int __fastcall CycleLeft(void* object, void*)
{
    pullAll();
    const int result = RequirePad(object)->CycleWeaponLeftJustDown();
    pushAll();
    return result;
}
int __fastcall CycleRight(void* object, void*)
{
    pullAll();
    const int result = RequirePad(object)->CycleWeaponRightJustDown();
    pushAll();
    return result;
}
} // namespace

void* PlayerPedToLegacy()
{
    CPlayerPed* native = FindPlayerPed();
    if (!mapped || !native || native != synchronizedPlayer) return nullptr;
    return &playerPed;
}

void* PadToLegacy(void* nativePad)
{
    if (mapped)
        for (int i = 0; i < 2; ++i)
            if (nativePad == CPad::GetPad(i)) return pads + i;
    return nullptr;
}

void* PlayerEntityToLegacy()
{
    // The native helper assumes an active player and dereferences it. DLL
    // initialization/menu callbacks can legitimately run before one exists.
    if (!FindPlayerPed()) return nullptr;
    CEntity* native = FindPlayerEntity();
    if (!native) return nullptr;
    auto* target = native == FindPlayerPed() ? &playerPed.entity : &vehicleEntity;
    target->placeable = MatrixFromNative(native->GetMatrix());
    target->vtable = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entityVtable));
    return target;
}

bool MapObjects(FullSync pullBeforeNative, FullSync pushAfterNative, CameraTranslator cameraToLegacy)
{
    if (mapped) return Fail("Object views are already mapped");
    if (!pullBeforeNative || !pushAfterNative) return Fail("Full scalar/object synchronization callbacks are required");
    pullAll = pullBeforeNative;
    pushAll = pushAfterNative;
    translateCamera = cameraToLegacy;
    const VC10::Camera initialCamera = {};
    const VC10::Pad initialPads[2] = {};
    const VC10::Menu initialMenu = {};
    const VC10::Mouse initialMouse = {};
    if (!MapMutableData(VC10::CameraAddress, &initialCamera, sizeof(initialCamera)) ||
        !MapMutableData(VC10::PadsAddress, initialPads, sizeof(initialPads)) ||
        !MapMutableData(VC10::MenuAddress, &initialMenu, sizeof(initialMenu)) ||
        !MapMutableData(VC10::MouseAddress, &initialMouse, sizeof(initialMouse)) ||
        !MapMutableData(VC10::TemporaryMouseAddress, &initialMouse, sizeof(initialMouse)))
        return Fail(MemoryError());
    camera = static_cast<VC10::Camera*>(MutableMemory(VC10::CameraAddress, sizeof(initialCamera)));
    pads = static_cast<VC10::Pad*>(MutableMemory(VC10::PadsAddress, sizeof(initialPads)));
    menu = static_cast<VC10::Menu*>(MutableMemory(VC10::MenuAddress, sizeof(initialMenu)));
    mouse = static_cast<VC10::Mouse*>(MutableMemory(VC10::MouseAddress, sizeof(initialMouse)));
    temporaryMouse = static_cast<VC10::Mouse*>(MutableMemory(VC10::TemporaryMouseAddress, sizeof(initialMouse)));
    if (!camera || !pads || !menu || !mouse || !temporaryMouse) return Fail(MemoryError());
#define MAP_FUNCTION(Address, Function) if (!MapFunction(Address, reinterpret_cast<void (*)()>(&Function))) return Fail(MemoryError())
    MAP_FUNCTION(0x46BA72, SetFixedPosition);
    MAP_FUNCTION(0x46A494, TakeControlNoEntity);
    MAP_FUNCTION(0x46BB24, RestoreJumpCut);
    MAP_FUNCTION(0x46B875, WidescreenOn);
    MAP_FUNCTION(0x46B863, WidescreenOff);
    MAP_FUNCTION(0x46FC1C, DrawWidescreen);
    MAP_FUNCTION(0x4AB060, GetPad);
    MAP_FUNCTION(0x4BC1B0, FindEntity);
    MAP_FUNCTION(0x4BC120, FindPed);
    MAP_FUNCTION(0x4BC1E0, FindVehicle);
    MAP_FUNCTION(0x4FF900, SetPlayerWeapon);
    MAP_FUNCTION(0x534580, ChangePlayerWeapon);
    MAP_FUNCTION(0x4A9A80, LookLeftRight);
    MAP_FUNCTION(0x4A98F0, LookUpDown);
    MAP_FUNCTION(0x4AD820, UpdateMouse);
    MAP_FUNCTION(0x4AA560, CycleLeft);
    MAP_FUNCTION(0x4AA530, CycleRight);
#undef MAP_FUNCTION
    for (auto& function : entityVtable)
        function = reinterpret_cast<void (*)()>(&UnsupportedEntityVirtual);
    mapped = true;
    PushObjects();
    return true;
}

void PushObjects()
{
    if (!mapped) return;
    camera->placeable = MatrixFromNative(TheCamera.GetMatrix());
    camera->lookingAtPlayer = TheCamera.m_bLookingAtPlayer;
    camera->lookingAtVector = TheCamera.m_bLookingAtVector;
    camera->wantsWidescreenOff = TheCamera.m_bWantsToSwitchWidescreenOff;
    camera->widescreen = TheCamera.m_WideScreenOn;
    camera->activeCam = TheCamera.ActiveCam;
    camera->transitionJustStarted = TheCamera.m_uiTransitionJUSTStarted;
    camera->transitionState = TheCamera.m_uiTransitionState;
    camera->controller = TheCamera.WhoIsInControlOfTheCamera;
    camera->fixedTarget = VectorFromNative(TheCamera.m_vecFixedModeVector);
    camera->fixedSource = VectorFromNative(TheCamera.m_vecFixedModeSource);
    camera->fixedUp = VectorFromNative(TheCamera.m_vecFixedModeUpOffSet);
    camera->rwCamera = translateCamera ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(translateCamera(TheCamera.m_pRwCamera))) : 0;
    camera->cameraMatrix = MatrixFromNative(TheCamera.m_cameraMatrix);
    camera->modeToGoTo = TheCamera.m_iModeToGoTo;
    camera->typeOfSwitch = TheCamera.m_iTypeOfSwitch;
    for (unsigned i = 0; i < 3; ++i) {
        VC10::Cam& target = camera->cams[i];
        const CCam& native = TheCamera.Cams[i];
        target.belowMinDistance = native.bBelowMinDist;
        target.behindPlayerDesired = native.bBehindPlayerDesired;
        target.lookingAtVector = native.m_bCamLookingAtVector;
        target.collisionChecks = native.m_bCollisionChecksOn;
        target.mode = native.Mode;
        target.fov = native.FOV;
        target.fixedTarget = VectorFromNative(native.m_cvecCamFixedModeVector);
        target.fixedSource = VectorFromNative(native.m_cvecCamFixedModeSource);
        target.fixedUp = VectorFromNative(native.m_cvecCamFixedModeUpOffSet);
        target.front = VectorFromNative(native.Front);
        target.source = VectorFromNative(native.Source);
        target.sourceBeforeLookBehind = VectorFromNative(native.SourceBeforeLookBehind);
        target.up = VectorFromNative(native.Up);
    }
    for (unsigned i = 0; i < 2; ++i) PushPad(pads[i], *CPad::GetPad(i));
    menu->active = FrontEndMenuManager.m_bMenuActive;
    menu->wantRestart = FrontEndMenuManager.m_bWantToRestart;
    menu->wantLoad = FrontEndMenuManager.m_bWantToLoad;
    menu->gameNotLoaded = FrontEndMenuManager.m_bGameNotLoaded;
    PushMouse(*mouse, CPad::NewMouseControllerState);
    PushMouse(*temporaryMouse, CPad::PCTempMouseControllerState);
    PushPlayer();
    oldCamera = *camera;
    std::memcpy(oldPads, pads, sizeof(oldPads));
    oldMenu = *menu;
    oldMouse = *mouse;
    oldTemporaryMouse = *temporaryMouse;
}

void PullObjects()
{
    if (!mapped) return;
    PullPlayer();
    MergeMatrix(camera->placeable, oldCamera.placeable, TheCamera.GetMatrix());
    MergeBool(camera->lookingAtPlayer, oldCamera.lookingAtPlayer, TheCamera.m_bLookingAtPlayer, "camera::lookingAtPlayer");
    MergeBool(camera->lookingAtVector, oldCamera.lookingAtVector, TheCamera.m_bLookingAtVector, "camera::lookingAtVector");
    MergeBool(camera->wantsWidescreenOff, oldCamera.wantsWidescreenOff, TheCamera.m_bWantsToSwitchWidescreenOff, "camera::wantsWidescreenOff");
    MergeBool(camera->widescreen, oldCamera.widescreen, TheCamera.m_WideScreenOn, "camera::widescreen");
    if (camera->activeCam != oldCamera.activeCam) {
        if (camera->activeCam < 3) TheCamera.ActiveCam = camera->activeCam;
        else Rejected("camera::activeCam");
    }
    if (camera->controller != oldCamera.controller) {
        if (camera->controller >= 0 && camera->controller <= 2) TheCamera.WhoIsInControlOfTheCamera = camera->controller;
        else Rejected("camera::controller");
    }
    if (camera->transitionJustStarted != oldCamera.transitionJustStarted)
        TheCamera.m_uiTransitionJUSTStarted = camera->transitionJustStarted;
    if (camera->transitionState != oldCamera.transitionState) {
        if (camera->transitionState <= 3) TheCamera.m_uiTransitionState = camera->transitionState;
        else Rejected("camera::transitionState");
    }
    MergeVector(camera->fixedTarget, oldCamera.fixedTarget, TheCamera.m_vecFixedModeVector, "camera::fixedTarget");
    MergeVector(camera->fixedSource, oldCamera.fixedSource, TheCamera.m_vecFixedModeSource, "camera::fixedSource");
    MergeVector(camera->fixedUp, oldCamera.fixedUp, TheCamera.m_vecFixedModeUpOffSet, "camera::fixedUp");
    MergeMatrix(camera->cameraMatrix, oldCamera.cameraMatrix, TheCamera.m_cameraMatrix);
    MergeMode(camera->modeToGoTo, oldCamera.modeToGoTo, TheCamera.m_iModeToGoTo);
    if (camera->typeOfSwitch != oldCamera.typeOfSwitch) {
        if (camera->typeOfSwitch >= 0 && camera->typeOfSwitch <= 2) TheCamera.m_iTypeOfSwitch = camera->typeOfSwitch;
        else Rejected("camera::typeOfSwitch");
    }
    if (camera->rwCamera != oldCamera.rwCamera) Rejected("camera::rwCamera replacement is unsupported");
    for (unsigned i = 0; i < 3; ++i) {
        const VC10::Cam& current = camera->cams[i];
        const VC10::Cam& previous = oldCamera.cams[i];
        CCam& native = TheCamera.Cams[i];
        MergeBool(current.lookingAtVector, previous.lookingAtVector, native.m_bCamLookingAtVector, "cam::lookingAtVector");
        MergeBool(current.collisionChecks, previous.collisionChecks, native.m_bCollisionChecksOn, "cam::collisionChecks");
        MergeMode(current.mode, previous.mode, native.Mode);
        MergeFov(current.fov, previous.fov, native.FOV);
        MergeVector(current.fixedTarget, previous.fixedTarget, native.m_cvecCamFixedModeVector, "cam::fixedTarget");
        MergeVector(current.fixedSource, previous.fixedSource, native.m_cvecCamFixedModeSource, "cam::fixedSource");
        MergeVector(current.fixedUp, previous.fixedUp, native.m_cvecCamFixedModeUpOffSet, "cam::fixedUp");
        MergeVector(current.front, previous.front, native.Front, "cam::front");
        MergeVector(current.source, previous.source, native.Source, "cam::source");
        MergeVector(current.sourceBeforeLookBehind, previous.sourceBeforeLookBehind, native.SourceBeforeLookBehind, "cam::sourceBeforeLookBehind");
        MergeVector(current.up, previous.up, native.Up, "cam::up");
    }
    for (unsigned i = 0; i < 2; ++i) PullPad(pads[i], oldPads[i], *CPad::GetPad(i));
    MergeBool(menu->active, oldMenu.active, FrontEndMenuManager.m_bMenuActive, "menu::active");
    MergeBool(menu->wantRestart, oldMenu.wantRestart, FrontEndMenuManager.m_bWantToRestart, "menu::wantRestart");
    MergeBool(menu->wantLoad, oldMenu.wantLoad, FrontEndMenuManager.m_bWantToLoad, "menu::wantLoad");
    MergeBool(menu->gameNotLoaded, oldMenu.gameNotLoaded, FrontEndMenuManager.m_bGameNotLoaded, "menu::gameNotLoaded");
    PullMouse(*mouse, oldMouse, CPad::NewMouseControllerState);
    PullMouse(*temporaryMouse, oldTemporaryMouse, CPad::PCTempMouseControllerState);
}

const char* ObjectsError() { return errorText; }
} // namespace LegacyAsi
#endif
