// Mediumcore death for Prey (2017), Chairloader mod code.
//
// How the game dies (reverse engineered, see README):
//   * Health is a player stat. When it reaches 0, ArkPlayerHealthComponent::DoDeath() holsters the weapon,
//     puts the movement FSM into the Death state (ragdoll / death camera) and arms m_timeRemainingBeforeDeathMenu.
//   * ArkPlayerHealthComponent::Update() is the ONLY place that opens the death menu: once the timer expires it
//     fires the "open death screen" UI event and sets m_bDeathMenuOpened.
//   * The death screen has a developer "Revive" button whose handler is simply ArkPlayer::Revive():
//     it reloads the player model, re-attaches the weapon, resets the movement FSM, restores health and
//     clears the death-menu flag. That is our respawn primitive.
//   * Inventory items are entities. CArkItem::Drop(count, &position) removes them from the inventory (splitting
//     stacks), un-hides and physicalizes them at the given position.
//
// We hook ArkPlayerHealthComponent::Update: while the player is dead and the mod is enabled we swallow the
// call (so the death menu never opens), wait the configured delay, drop the inventory at the death spot,
// teleport to the respawn point and Revive().
#include "Mediumcore.h"
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryScriptSystem/IScriptSystem.h>
#include <Prey/CryRenderer/IRenderer.h>
#include <Prey/CryRenderer/IRenderAuxGeom.h>
#include <Prey/CryPhysics/physinterface.h>
#include <Prey/CrySystem/IConsole.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerCamera.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerHealthComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerWeaponComponent.h>
#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/arkitem.h>
#include <Prey/GameDll/ark/ArkItemSystem.h>
#include <Prey/GameDll/ark/ArkGame.h>
#include <Prey/GameDll/ark/ui/ArkPauseMenu.h>
#include <Prey/GameDll/ark/player/ArkPlayerSignalReceiver.h>
#include <Prey/GameDll/ark/player/ArkPsiComponent.h>
#include <Prey/GameDll/ark/ArkFabricator.h>
#include <Prey/GameDll/ark/player/ArkPlayerInput.h>
#include <Prey/GameDll/ark/iface/IArkItem.h>
#include <Prey/GameDll/ark/player/ArkPlayerStatusComponent.h>
#include <Prey/GameDll/ark/player/trauma/ArkTraumaBase.h>
#include <Prey/GameDll/ark/player/ArkPOIComponent.h>
#include <Prey/ArkEnums.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <pugixml.hpp>
#include <algorithm>
#include <cstring>

MediumcoreDeath* gMediumcore = nullptr;

// ArkItemSystem::GiveArchetype(pickerId, archetypeName, quantity) returns a std::vector by value. As a MEMBER
// function the MSVC x64 convention is (this, hidden return pointer, args...); the SDK header declares it as
// returning the vector directly, which puts the return pointer first and crashes. Declared here at file scope:
// PreyFunction objects must exist before the DLL is initialised (they are rebased then).
static auto s_fnGiveArchetype = PreyFunction<std::vector<unsigned>*(const ArkItemSystem* _this, std::vector<unsigned>* _ret, unsigned _pickerId, const char* _archetypeName, int _quantity)>(0x1442000);

//---------------------------------------------------------------------------------
// Hook
//---------------------------------------------------------------------------------
static auto s_hookHealthUpdate = ArkPlayerHealthComponent::FUpdate.MakeHook();
static void ArkPlayerHealthComponent_Update_Hook(ArkPlayerHealthComponent* const _this, const float _frameTime)
{
    if (gMediumcore && gMediumcore->OnHealthUpdate(_this, _frameTime))
        return;
    s_hookHealthUpdate.InvokeOrig(_this, _frameTime);
}

// A level-to-level transition (door / airlock / elevator) just placed the player in the new level.
static auto s_hookTransitionFinished = ArkGame::FOnLevelTransitionFinished.MakeHook();
static void ArkGame_OnLevelTransitionFinished_Hook(ArkGame* const _this)
{
    s_hookTransitionFinished.InvokeOrig(_this);
    if (gMediumcore)
        gMediumcore->OnLevelTransitionFinished();
}

//---------------------------------------------------------------------------------
// Save rule hooks. ArkGame::CanManualSave() is the game's own single gate for quicksaves and the pause
// menu's Save entry; CanAutoSave() calls it internally, so autosaves are told apart with a flag.
//---------------------------------------------------------------------------------
static auto s_hookCanManualSave = ArkGame::FCanManualSave.MakeHook();
static bool ArkGame_CanManualSave_Hook()
{
    if (!s_hookCanManualSave.InvokeOrig())
        return false;
    return !gMediumcore || gMediumcore->AllowManualSave();
}

static auto s_hookCanAutoSave = ArkGame::FCanAutoSave.MakeHook();
static bool ArkGame_CanAutoSave_Hook()
{
    const bool prev = gMediumcore ? gMediumcore->InAutoSaveCheck() : false;
    if (gMediumcore) gMediumcore->SetInAutoSaveCheck(true);
    const bool r = s_hookCanAutoSave.InvokeOrig();
    if (gMediumcore) gMediumcore->SetInAutoSaveCheck(prev);
    return r;
}

static auto s_hookLoadLastSave = ArkGame::FLoadLastSave.MakeHook();
static ELoadGameResult ArkGame_LoadLastSave_Hook(ArkGame* const _this, IArkGameLoadSaveListener* _pListener)
{
    if (gMediumcore && !gMediumcore->AllowQuickLoad())
        return eLGR_NoSavesExist; // the quiet failure code: the game shows nothing, we show our message
    return s_hookLoadLastSave.InvokeOrig(_this, _pListener);
}

static auto s_hookOpenSaveLoadMenu = ArkPauseMenu::FOpenSaveLoadMenu.MakeHook();
static void ArkPauseMenu_OpenSaveLoadMenu_Hook(ArkPauseMenu* const _this, bool _bSave)
{
    if (gMediumcore && !gMediumcore->AllowSaveLoadMenu(_bSave))
        return;
    s_hookOpenSaveLoadMenu.InvokeOrig(_this, _bSave);
}

static auto s_hookOnActionQuickSave = ArkPlayerInput::FOnActionQuickSave.MakeHook();
static bool ArkPlayerInput_OnActionQuickSave_Hook(ArkPlayerInput* const _this, unsigned _entityId, const CCryName& _actionId, int _activationMode, float _value)
{
    if (gMediumcore && (_activationMode & eAAM_OnPress))
        gMediumcore->OnQuickSaveAction();
    return s_hookOnActionQuickSave.InvokeOrig(_this, _entityId, _actionId, _activationMode, _value);
}

static auto s_hookQuickSave = ArkGame::FQuickSave.MakeHook();
static void ArkGame_QuickSave_Hook(ArkGame* const _this)
{
    s_hookQuickSave.InvokeOrig(_this);
    if (gMediumcore) gMediumcore->OnManualSaveDone();
}

static auto s_hookManualSave = ArkGame::FManualSave.MakeHook();
static bool ArkGame_ManualSave_Hook(ArkGame* const _this)
{
    const bool r = s_hookManualSave.InvokeOrig(_this);
    if (r && gMediumcore) gMediumcore->OnManualSaveDone();
    return r;
}

static auto s_hookOnSaveGame = ArkGame::FOnSaveGame.MakeHook();
static void ArkGame_OnSaveGame_Hook(ArkGame* const _this)
{
    s_hookOnSaveGame.InvokeOrig(_this);
    if (gMediumcore) gMediumcore->OnGameSaved();
}

//---------------------------------------------------------------------------------
// Resource hooks
//---------------------------------------------------------------------------------
static auto s_hookOnReceiveSignal = ArkPlayerSignalReceiver::FOnReceiveSignal.MakeHook();
static void ArkPlayerSignalReceiver_OnReceiveSignal_Hook(ArkPlayerSignalReceiver* const _this, const ArkSignalSystem::Package& _package)
{
    const bool prev = gMediumcore ? gMediumcore->InPlayerSignal() : false;
    if (gMediumcore) gMediumcore->SetInPlayerSignal(true);
    s_hookOnReceiveSignal.InvokeOrig(_this, _package);
    if (gMediumcore) gMediumcore->SetInPlayerSignal(prev);
}

static auto s_hookSetHealth = ArkPlayerHealthComponent::FSetHealth.MakeHook();
static void ArkPlayerHealthComponent_SetHealth_Hook(ArkPlayerHealthComponent* const _this, const float _health, const bool _bDamagedByRecyclerGrenade)
{
    float h = _health;
    if (gMediumcore && gMediumcore->InPlayerSignal())
        h = gMediumcore->ScaleHealthChange(_this->GetHealth(), _health);
    s_hookSetHealth.InvokeOrig(_this, h, _bDamagedByRecyclerGrenade);
}

static auto s_hookReduceStatus = ArkPlayerStatusComponent::FReduceStatus.MakeHook();
static void ArkPlayerStatusComponent_ReduceStatus_Hook(ArkPlayerStatusComponent* const _this, uint64_t _signalId, float _amount)
{
    float a = _amount;
    if (gMediumcore && gMediumcore->InPlayerSignal())
        a = gMediumcore->ScaleStatusReduction(_signalId, _amount);
    s_hookReduceStatus.InvokeOrig(_this, _signalId, a);
}

static auto s_hookIncrementPoints = CArkPsiComponent::FIncrementPoints.MakeHook();
static void CArkPsiComponent_IncrementPoints_Hook(CArkPsiComponent* const _this, const float _points)
{
    float p = _points;
    if (gMediumcore && gMediumcore->InPlayerSignal())
        p = gMediumcore->ScalePsi(_points);
    s_hookIncrementPoints.InvokeOrig(_this, p);
}

static auto s_hookInitializeCount = CArkItem::FInitializeCount.MakeHook();
static void CArkItem_InitializeCount_Hook(CArkItem* const _this)
{
    s_hookInitializeCount.InvokeOrig(_this);
    if (gMediumcore)
        gMediumcore->OnItemCountInitialized(_this);
}

static auto s_hookFabSpawnItem = ArkFabricator::FSpawnItem.MakeHook();
static IEntity* ArkFabricator_SpawnItem_Hook(ArkFabricator* const _this)
{
    IEntity* pEnt = s_hookFabSpawnItem.InvokeOrig(_this);
    if (gMediumcore && pEnt)
        gMediumcore->OnFabricatorSpawnedItem(pEnt);
    return pEnt;
}

static auto s_hookFabricationCount = CArkItem::FGetFabricationCount.MakeHook();
static int CArkItem_GetFabricationCount_Hook(IEntityArchetype* const _pArchetype)
{
    const int n = s_hookFabricationCount.InvokeOrig(_pArchetype);
    return gMediumcore ? gMediumcore->ScaleLootCount(_pArchetype, n) : n;
}

void MediumcoreDeath::InitHooks()
{
    s_hookOnReceiveSignal.SetHookFunc(&ArkPlayerSignalReceiver_OnReceiveSignal_Hook);
    s_hookSetHealth.SetHookFunc(&ArkPlayerHealthComponent_SetHealth_Hook);
    s_hookReduceStatus.SetHookFunc(&ArkPlayerStatusComponent_ReduceStatus_Hook);
    s_hookIncrementPoints.SetHookFunc(&CArkPsiComponent_IncrementPoints_Hook);
    s_hookInitializeCount.SetHookFunc(&CArkItem_InitializeCount_Hook);
    s_hookFabSpawnItem.SetHookFunc(&ArkFabricator_SpawnItem_Hook);
    s_hookFabricationCount.SetHookFunc(&CArkItem_GetFabricationCount_Hook);
    s_hookHealthUpdate.SetHookFunc(&ArkPlayerHealthComponent_Update_Hook);
    s_hookTransitionFinished.SetHookFunc(&ArkGame_OnLevelTransitionFinished_Hook);
    s_hookCanManualSave.SetHookFunc(&ArkGame_CanManualSave_Hook);
    s_hookCanAutoSave.SetHookFunc(&ArkGame_CanAutoSave_Hook);
    s_hookLoadLastSave.SetHookFunc(&ArkGame_LoadLastSave_Hook);
    s_hookOpenSaveLoadMenu.SetHookFunc(&ArkPauseMenu_OpenSaveLoadMenu_Hook);
    s_hookOnActionQuickSave.SetHookFunc(&ArkPlayerInput_OnActionQuickSave_Hook);
    s_hookQuickSave.SetHookFunc(&ArkGame_QuickSave_Hook);
    s_hookManualSave.SetHookFunc(&ArkGame_ManualSave_Hook);
    s_hookOnSaveGame.SetHookFunc(&ArkGame_OnSaveGame_Hook);
}

//---------------------------------------------------------------------------------
// CVars
//---------------------------------------------------------------------------------
void MediumcoreDeath::RegisterCVars()
{
    MediumcoreSettings& s = m_s;
    m_hookInstalled = s_hookHealthUpdate.IsHooked();
    m_saveHooksInstalled = s_hookCanManualSave.IsHooked() && s_hookCanAutoSave.IsHooked() && s_hookLoadLastSave.IsHooked() && s_hookOpenSaveLoadMenu.IsHooked();
    REGISTER_CVAR2("mc_enabled", &s.enabled, s.enabled, VF_DUMPTOCHAIR, "Mediumcore: dying drops your inventory and respawns you instead of the death menu (0/1)");
    REGISTER_CVAR2("mc_delay", &s.delay, s.delay, VF_DUMPTOCHAIR, "Mediumcore: seconds between dying and respawning");
    REGISTER_CVAR2("mc_respawn_mode", &s.respawnMode, s.respawnMode, VF_DUMPTOCHAIR, "Mediumcore: respawn at 0 = level entry (remembered per level), 1 = your spawn point (per level; falls back to 0), 2 = level entrance nearest to the death spot, 3 = where this session loaded you, 4 = where you died");
    REGISTER_CVAR2("mc_suit_integrity", &s.suitIntegrity, s.suitIntegrity, VF_DUMPTOCHAIR, "Mediumcore: suit integrity % after respawn (-1 = leave)");
    REGISTER_CVAR2("mc_give_wrench", &s.giveWrench, s.giveWrench, VF_DUMPTOCHAIR, "Mediumcore: always respawn with a wrench (0/1)");
    REGISTER_CVAR2("mc_equip_wrench", &s.equipWrench, s.equipWrench, VF_DUMPTOCHAIR, "Mediumcore: equip the wrench after respawning (0/1)");
    REGISTER_CVAR2("mc_weapon_damage", &s.weaponDamage, s.weaponDamage, VF_DUMPTOCHAIR, "Mediumcore: weapon condition lost on death (points 0..100, or % with mc_weapon_damage_mode 1)");
    REGISTER_CVAR2("mc_weapon_damage_mode", &s.weaponDamageMode, s.weaponDamageMode, VF_DUMPTOCHAIR, "Mediumcore: 0 = flat condition points, 1 = percent of current condition");
    REGISTER_CVAR2("mc_weapon_damage_respect_difficulty", &s.weaponDamageRespectDifficulty, s.weaponDamageRespectDifficulty, VF_DUMPTOCHAIR, "Mediumcore: only damage weapons that degrade with the current difficulty settings (0/1)");
    REGISTER_CVAR2("mc_weapon_damage_kept", &s.weaponDamageKept, s.weaponDamageKept, VF_DUMPTOCHAIR, "Mediumcore: also damage the weapons you keep (0/1)");
    REGISTER_CVAR2("mc_marker", &s.marker, s.marker, VF_DUMPTOCHAIR, "Mediumcore: put a marker on the dropped gear (0/1)");
    REGISTER_CVAR2("mc_marker_poi", &s.markerPoi, s.markerPoi, VF_DUMPTOCHAIR, "Mediumcore: marker icon. 0 corpse, 1 debris, 2 workstation, 3 oxygen station");
    REGISTER_CVAR2("mc_marker_time", &s.markerTime, s.markerTime, VF_DUMPTOCHAIR, "Mediumcore: seconds until the marker disappears (0 = when the item is gone)");
    REGISTER_CVAR2("mc_spawn_key", &s.spawnKey, s.spawnKey, VF_DUMPTOCHAIR, "Mediumcore: EKeyId of the key that sets the manual spawn point (0 = none)");
    REGISTER_CVAR2("mc_spawn_key_consume", &s.spawnKeyConsume, s.spawnKeyConsume, VF_DUMPTOCHAIR, "Mediumcore: swallow the spawn key so the game ignores it (0/1)");
    REGISTER_CVAR2("mc_spawn_height", &s.spawnHeightOffset, s.spawnHeightOffset, VF_DUMPTOCHAIR, "Mediumcore: meters added above the respawn point");
    REGISTER_CVAR2("mc_health_percent", &s.healthPercent, s.healthPercent, VF_DUMPTOCHAIR, "Mediumcore: health after respawn, % of max");
    REGISTER_CVAR2("mc_grace_period", &s.gracePeriod, s.gracePeriod, VF_DUMPTOCHAIR, "Mediumcore: seconds of invulnerability after respawn");
    REGISTER_CVAR2("mc_keep_equipped", &s.keepEquippedWeapon, s.keepEquippedWeapon, VF_DUMPTOCHAIR, "Mediumcore: keep the weapon you were holding (0/1)");
    REGISTER_CVAR2("mc_drop_weapons", &s.dropWeapons, s.dropWeapons, VF_DUMPTOCHAIR, "Mediumcore: drop weapons (0/1)");
    REGISTER_CVAR2("mc_drop_ammo", &s.dropAmmo, s.dropAmmo, VF_DUMPTOCHAIR, "Mediumcore: drop ammo (0/1)");
    REGISTER_CVAR2("mc_drop_grenades", &s.dropGrenades, s.dropGrenades, VF_DUMPTOCHAIR, "Mediumcore: drop grenades (0/1)");
    REGISTER_CVAR2("mc_drop_consumables", &s.dropConsumables, s.dropConsumables, VF_DUMPTOCHAIR, "Mediumcore: drop food, drink, medkits, suit patches, oxygen (0/1)");
    REGISTER_CVAR2("mc_drop_neuromods", &s.dropNeuromods, s.dropNeuromods, VF_DUMPTOCHAIR, "Mediumcore: drop neuromods (0/1)");
    REGISTER_CVAR2("mc_drop_materials", &s.dropMaterials, s.dropMaterials, VF_DUMPTOCHAIR, "Mediumcore: drop recycler junk and crafting materials (0/1)");
    REGISTER_CVAR2("mc_drop_chipsets", &s.dropChipsets, s.dropChipsets, VF_DUMPTOCHAIR, "Mediumcore: drop suit/psychoscope/weapon chipsets that are in the inventory (0/1)");
    REGISTER_CVAR2("mc_drop_plans", &s.dropPlans, s.dropPlans, VF_DUMPTOCHAIR, "Mediumcore: drop fabrication plans (0/1)");
    REGISTER_CVAR2("mc_drop_keycards_notes", &s.dropKeycardsNotes, s.dropKeycardsNotes, VF_DUMPTOCHAIR, "Mediumcore: drop keycards, notes and pages (0/1)");
    REGISTER_CVAR2("mc_drop_quest_items", &s.dropQuestItems, s.dropQuestItems, VF_DUMPTOCHAIR, "Mediumcore: drop quest / special items (0/1)");
    REGISTER_CVAR2("mc_drop_other", &s.dropOther, s.dropOther, VF_DUMPTOCHAIR, "Mediumcore: drop everything not covered by the other categories (0/1)");
    REGISTER_CVAR2("mc_drop_plot_critical", &s.dropPlotCritical, s.dropPlotCritical, VF_DUMPTOCHAIR, "Mediumcore: ALSO drop items the game marks plot critical - can break progression (0/1)");
    REGISTER_CVAR2("mc_drop_percent", &s.dropPercent, s.dropPercent, VF_DUMPTOCHAIR, "Mediumcore: % of each stack that is dropped");
    REGISTER_CVAR2("mc_drop_scatter", &s.scatterRadius, s.scatterRadius, VF_DUMPTOCHAIR, "Mediumcore: scatter radius around the death spot in meters");
    REGISTER_CVAR2("mc_drop_height", &s.dropHeight, s.dropHeight, VF_DUMPTOCHAIR, "Mediumcore: drop height above the death spot in meters");
    REGISTER_CVAR2("mc_autosave", &s.autosave, s.autosave, VF_DUMPTOCHAIR, "Mediumcore: save after respawning. 0 = no, 1 = autosave, 2 = quicksave");
    REGISTER_CVAR2("mc_autosave_delay", &s.autosaveDelay, s.autosaveDelay, VF_DUMPTOCHAIR, "Mediumcore: seconds after the respawn before that save is written");
    REGISTER_CVAR2("mc_message", &s.message, s.message, VF_DUMPTOCHAIR, "Mediumcore: show a HUD message after respawning (0/1)");
    REGISTER_CVAR2("mc_message_time", &s.messageTime, s.messageTime, VF_DUMPTOCHAIR, "Mediumcore: HUD message duration in seconds");
    REGISTER_CVAR2("mc_log_items", &s.logItems, s.logItems, VF_DUMPTOCHAIR, "Mediumcore: log every dropped item (0/1)");
    // destroyed instead of dropped
    REGISTER_CVAR2("mc_destroy_weapons", &s.destroyWeapons, s.destroyWeapons, VF_DUMPTOCHAIR, "Mediumcore: % of dropped weapons that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_ammo", &s.destroyAmmo, s.destroyAmmo, VF_DUMPTOCHAIR, "Mediumcore: % of dropped ammo that is destroyed instead");
    REGISTER_CVAR2("mc_destroy_grenades", &s.destroyGrenades, s.destroyGrenades, VF_DUMPTOCHAIR, "Mediumcore: % of dropped grenades that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_consumables", &s.destroyConsumables, s.destroyConsumables, VF_DUMPTOCHAIR, "Mediumcore: % of dropped consumables (food, medkits, patches, oxygen) that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_neuromods", &s.destroyNeuromods, s.destroyNeuromods, VF_DUMPTOCHAIR, "Mediumcore: % of dropped neuromods that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_materials", &s.destroyMaterials, s.destroyMaterials, VF_DUMPTOCHAIR, "Mediumcore: % of dropped materials / junk that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_chipsets", &s.destroyChipsets, s.destroyChipsets, VF_DUMPTOCHAIR, "Mediumcore: % of dropped chipsets that are destroyed instead");
    REGISTER_CVAR2("mc_destroy_other", &s.destroyOther, s.destroyOther, VF_DUMPTOCHAIR, "Mediumcore: % of other dropped items (plans, notes, quest items...) that are destroyed instead");
    // trauma
    REGISTER_CVAR2("mc_trauma_mode", &s.traumaMode, s.traumaMode, VF_DUMPTOCHAIR, "Mediumcore: trauma after respawn. 0 none, 1 one random from the enabled list, 2 all enabled");
    REGISTER_CVAR2("mc_trauma_level", &s.traumaLevel, s.traumaLevel, VF_DUMPTOCHAIR, "Mediumcore: trauma severity level (1 mild .. 3)");
    REGISTER_CVAR2("mc_trauma_bleeding", &s.traumaBleeding, s.traumaBleeding, VF_DUMPTOCHAIR, "Mediumcore: bleeding is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_burning", &s.traumaBurning, s.traumaBurning, VF_DUMPTOCHAIR, "Mediumcore: burns are a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_concussion", &s.traumaConcussion, s.traumaConcussion, VF_DUMPTOCHAIR, "Mediumcore: concussion is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_crippled", &s.traumaCrippled, s.traumaCrippled, VF_DUMPTOCHAIR, "Mediumcore: crippled leg is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_disruption", &s.traumaDisruption, s.traumaDisruption, VF_DUMPTOCHAIR, "Mediumcore: disruption is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_fear", &s.traumaFear, s.traumaFear, VF_DUMPTOCHAIR, "Mediumcore: fear is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_psychoshock", &s.traumaPsychoShock, s.traumaPsychoShock, VF_DUMPTOCHAIR, "Mediumcore: psychoshock is a possible respawn trauma (0/1)");
    REGISTER_CVAR2("mc_trauma_radiation", &s.traumaRadiation, s.traumaRadiation, VF_DUMPTOCHAIR, "Mediumcore: radiation is a possible respawn trauma (0/1)");
    // saving & loading
    REGISTER_CVAR2("mc_save_mode", &s.saveMode, s.saveMode, VF_DUMPTOCHAIR, "Save rules: 0 game default, 1 manual saves only near a save station, 2 no manual saves (autosaves only)");
    REGISTER_CVAR2("mc_save_station_recycler", &s.stationRecycler, s.stationRecycler, VF_DUMPTOCHAIR, "Save rules: recyclers are save stations (0/1)");
    REGISTER_CVAR2("mc_save_station_fabricator", &s.stationFabricator, s.stationFabricator, VF_DUMPTOCHAIR, "Save rules: fabricators are save stations (0/1)");
    REGISTER_CVAR2("mc_save_station_dispenser", &s.stationDispenser, s.stationDispenser, VF_DUMPTOCHAIR, "Save rules: operator dispensers are save stations (0/1)");
    REGISTER_CVAR2("mc_save_station_oxygen", &s.stationOxygen, s.stationOxygen, VF_DUMPTOCHAIR, "Save rules: oxygen refill stations are save stations (0/1)");
    REGISTER_CVAR2("mc_save_station_operators", &s.stationOperators, s.stationOperators, VF_DUMPTOCHAIR, "Save rules: roaming operators are save stations (0/1)");
    REGISTER_CVAR2("mc_save_station_radius", &s.stationRadius, s.stationRadius, VF_DUMPTOCHAIR, "Save rules: how close to a save station you must be, meters");
    REGISTER_CVAR2("mc_save_cooldown", &s.saveCooldown, s.saveCooldown, VF_DUMPTOCHAIR, "Save rules: minutes between manual saves (0 = none)");
    REGISTER_CVAR2("mc_save_block_quickload", &s.blockQuickload, s.blockQuickload, VF_DUMPTOCHAIR, "Save rules: block quick load (F9 and the pause menu entry) (0/1)");
    REGISTER_CVAR2("mc_save_block_load_menu", &s.blockLoadMenu, s.blockLoadMenu, VF_DUMPTOCHAIR, "Save rules: block the pause menu's Load Game (the main menu still works) (0/1)");
    REGISTER_CVAR2("mc_save_timed", &s.timedAutosave, s.timedAutosave, VF_DUMPTOCHAIR, "Save rules: minutes between the mod's own autosaves (0 = off)");
    REGISTER_CVAR2("mc_save_timed_min_health", &s.timedMinHealth, s.timedMinHealth, VF_DUMPTOCHAIR, "Save rules: no timed autosave below this health %");
    REGISTER_CVAR2("mc_save_timed_calm", &s.timedCalmSeconds, s.timedCalmSeconds, VF_DUMPTOCHAIR, "Save rules: no timed autosave within this many seconds after taking damage");
    REGISTER_CVAR2("mc_save_messages", &s.saveMessages, s.saveMessages, VF_DUMPTOCHAIR, "Save rules: HUD message when a save or load was blocked (0/1)");
    // resources
    REGISTER_CVAR2("mc_res_heal", &s.healMult, s.healMult, VF_DUMPTOCHAIR, "Resources: multiplier for healing from medkits, food, drinks and medical operators");
    REGISTER_CVAR2("mc_res_suit_repair", &s.suitRepairMult, s.suitRepairMult, VF_DUMPTOCHAIR, "Resources: multiplier for suit integrity restored by suit repair kits");
    REGISTER_CVAR2("mc_res_psi", &s.psiMult, s.psiMult, VF_DUMPTOCHAIR, "Resources: multiplier for psi restored by psi hypos");
    REGISTER_CVAR2("mc_res_ammo_found", &s.ammoFoundMult, s.ammoFoundMult, VF_DUMPTOCHAIR, "Resources: multiplier for ammo found in the world and in containers");
    REGISTER_CVAR2("mc_res_ammo_loot", &s.ammoLootMult, s.ammoLootMult, VF_DUMPTOCHAIR, "Resources: multiplier for ammo dropped by enemies");
    REGISTER_CVAR2("mc_res_ammo_fab", &s.ammoFabMult, s.ammoFabMult, VF_DUMPTOCHAIR, "Resources: multiplier for ammo per fabrication");
    REGISTER_CVAR2("mc_res_consumables_found", &s.consumablesFoundMult, s.consumablesFoundMult, VF_DUMPTOCHAIR, "Resources: multiplier for consumables (medkits, food, patches, hypos) found in the world and in containers");
}

//---------------------------------------------------------------------------------
// Helpers
//---------------------------------------------------------------------------------
static bool StartsWith(const char* s, const char* prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static ArkPlayerHealthComponent* GetHealth(ArkPlayer* pPlayer)
{
    return pPlayer ? &pPlayer->m_playerComponent.GetHealthComponent() : nullptr;
}

void MediumcoreDeath::ShowMessage(const std::string& text)
{
    m_st.message = text;
    m_st.messageTimer = max(m_s.messageTime, 0.0f);
    CryLog("Mediumcore: {}", text);
}

static std::string CurrentLevelName()
{
    if (gEnv && gEnv->pGame && gEnv->pGame->GetIGameFramework())
    {
        const char* n = gEnv->pGame->GetIGameFramework()->GetLevelName();
        if (n && *n)
            return n;
    }
    return "";
}

static fs::path SpawnsPath()
{
    fs::path dir;
    if (gCL && gCL->conf)
        dir = gCL->conf->getConfigPath("Vee.Mediumcore").parent_path();
    if (dir.empty() && gCL && gCL->cl)
        dir = gCL->cl->GetModsPath() / "config";
    return dir / "Vee.Mediumcore.spawns.xml";
}

static void ReadPoint(pugi::xml_node n, const char* prefix, bool& valid, Vec3& pos, Quat& rot)
{
    auto a = [&](const char* suffix, float d) { return n.attribute((std::string(prefix) + suffix).c_str()).as_float(d); };
    valid = n.attribute((std::string(prefix) + "valid").c_str()).as_bool(false);
    pos = Vec3(a("x", 0), a("y", 0), a("z", 0));
    rot = Quat::CreateRotationZ(a("yaw", 0));
}

static void WritePoint(pugi::xml_node n, const char* prefix, bool valid, const Vec3& pos, const Quat& rot)
{
    auto a = [&](const char* suffix, float v) { n.append_attribute((std::string(prefix) + suffix).c_str()) = v; };
    n.append_attribute((std::string(prefix) + "valid").c_str()) = valid;
    a("x", pos.x); a("y", pos.y); a("z", pos.z);
    a("yaw", Ang3(rot).z);
}

void MediumcoreDeath::LoadSpawns()
{
    m_spawnsLoaded = true;
    m_levels.clear();
    pugi::xml_document doc;
    if (!doc.load_file(SpawnsPath().c_str()))
        return;
    for (pugi::xml_node n : doc.child("Spawns").children("Level"))
    {
        const char* name = n.attribute("name").as_string("");
        if (!*name)
            continue;
        LevelSpawnRecord r;
        ReadPoint(n, "entry_", r.entryValid, r.entryPos, r.entryRot);
        ReadPoint(n, "manual_", r.manualValid, r.manualPos, r.manualRot);
        m_levels[name] = r;
    }
    CryLog("Mediumcore: loaded spawn records for {} level(s)", m_levels.size());
}

void MediumcoreDeath::SaveSpawns()
{
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("Spawns");
    root.append_attribute("comment") = "Mediumcore respawn points per level. entry_*: where you entered the level through a transition. manual_*: the point you set.";
    for (auto& kv : m_levels)
    {
        pugi::xml_node n = root.append_child("Level");
        n.append_attribute("name") = kv.first.c_str();
        WritePoint(n, "entry_", kv.second.entryValid, kv.second.entryPos, kv.second.entryRot);
        WritePoint(n, "manual_", kv.second.manualValid, kv.second.manualPos, kv.second.manualRot);
    }
    if (!doc.save_file(SpawnsPath().c_str()))
        CryError("Mediumcore: failed to save {}", SpawnsPath().u8string());
}

LevelSpawnRecord& MediumcoreDeath::CurrentLevelRecord()
{
    if (!m_spawnsLoaded)
        LoadSpawns();
    return m_levels[m_st.levelName.empty() ? std::string("_unknown") : m_st.levelName];
}

const LevelSpawnRecord* MediumcoreDeath::FindLevelRecord() const
{
    auto it = m_levels.find(m_st.levelName.empty() ? std::string("_unknown") : m_st.levelName);
    return it == m_levels.end() ? nullptr : &it->second;
}

// The level's transition arrival points: "SpawnPoint" entities (Properties.destinationName = "From_<Level>").
// Cheat/debug spawns are skipped.
void MediumcoreDeath::ScanSpawnPoints()
{
    m_spawnPoints.clear();
    m_spawnPointsLevel = m_st.levelName;
    if (!gEnv || !gEnv->pEntitySystem)
        return;
    IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
    if (!it)
        return;
    it->MoveFirst();
    while (IEntity* pEnt = it->Next())
    {
        if (!pEnt->GetClass() || strcmp(pEnt->GetClass()->GetName(), "SpawnPoint") != 0)
            continue;
        SpawnPointInfo sp;
        sp.entityId = pEnt->GetId();
        sp.pos = pEnt->GetWorldPos();
        sp.rot = pEnt->GetWorldRotation();
        sp.name = pEnt->GetName() ? pEnt->GetName() : "";
        if (IScriptTable* pTable = pEnt->GetScriptTable())
        {
            SmartScriptTable props;
            if (pTable->GetValue("Properties", props))
            {
                const char* dest = "";
                if (props->GetValue("destinationName", dest) && dest)
                    sp.destination = dest;
                bool initial = false;
                if (props->GetValue("bInitialSpawn", initial))
                    sp.initial = initial;
            }
        }
        if (strstr(sp.destination.c_str(), "Cheat") || strstr(sp.name.c_str(), "Cheat"))
            continue;
        m_spawnPoints.push_back(sp);
    }
    CryLog("Mediumcore: found {} spawn point(s) in '{}'", m_spawnPoints.size(), m_st.levelName);
}

const SpawnPointInfo* MediumcoreDeath::NearestSpawnPoint(const Vec3& to, float maxDist) const
{
    const SpawnPointInfo* best = nullptr;
    float bestD = maxDist > 0.0f ? maxDist : 1e30f;
    for (const SpawnPointInfo& sp : m_spawnPoints)
    {
        const float d = (sp.pos - to).GetLength();
        if (d < bestD)
        {
            bestD = d;
            best = &sp;
        }
    }
    return best;
}

void MediumcoreDeath::OnLevelTransitionFinished()
{
    // The player gets positioned by the transition logic; read the position a few frames later.
    m_st.captureEntryFrames = 3;
    m_st.transitionPending = true;
}

//---------------------------------------------------------------------------------
// Save rules
//---------------------------------------------------------------------------------
static bool IsStaticStationClass(const MediumcoreSettings& s, const char* cls)
{
    if (!cls) return false;
    if (s.stationRecycler && strcmp(cls, "ArkRecycler") == 0) return true;
    if (s.stationFabricator && strcmp(cls, "ArkFabricator") == 0) return true;
    if (s.stationDispenser && strcmp(cls, "ArkOperatorDispenser") == 0) return true;
    if (s.stationOxygen && strcmp(cls, "ArkOxygenRefillStation") == 0) return true;
    return false;
}

static bool IsRoamingOperatorClass(const char* cls)
{
    return cls && StartsWith(cls, "ArkOperator") && strcmp(cls, "ArkOperatorDispenser") != 0;
}

void MediumcoreDeath::ScanStations()
{
    m_stations.clear();
    m_stationsLevel = m_st.levelName;
    if (!gEnv || !gEnv->pEntitySystem)
        return;
    IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
    if (!it)
        return;
    it->MoveFirst();
    while (IEntity* pEnt = it->Next())
    {
        const char* cls = pEnt->GetClass() ? pEnt->GetClass()->GetName() : nullptr;
        if (IsStaticStationClass(m_s, cls))
            m_stations.push_back(pEnt->GetWorldPos());
    }
    CryLog("Mediumcore: found {} save station(s) in '{}'", m_stations.size(), m_st.levelName);
}

void MediumcoreDeath::UpdateSaveRules(float dt, ArkPlayer* pPlayer)
{
    MediumcoreState& st = m_st;
    st.timeSinceManualSave += dt;
    st.timeSinceAnySave += dt;
    st.timeSinceDamage += dt;

    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    ArkPlayerHealthComponent* pHealth = GetHealth(pPlayer);
    if (!pEnt || !pHealth)
    {
        st.lastHealth = -1.0f;
        st.stationDistance = 1e9f;
        return;
    }

    // Damage tracking (for the "calm" condition of timed autosaves)
    const float h = pHealth->GetHealth();
    if (st.lastHealth >= 0.0f && h < st.lastHealth - 0.01f)
        st.timeSinceDamage = 0.0f;
    st.lastHealth = h;

    // Nearest save station (static list per level + optional live scan for roaming operators)
    st.stationScanTimer -= dt;
    if (m_s.saveMode == 1 && st.stationScanTimer <= 0.0f)
    {
        st.stationScanTimer = 0.5f;
        if (m_stationsLevel != st.levelName)
            ScanStations();
        const Vec3 me = pEnt->GetWorldPos();
        float best = 1e9f;
        for (const Vec3& p : m_stations)
            best = min(best, (p - me).GetLength());
        if (m_s.stationOperators && gEnv && gEnv->pEntitySystem)
        {
            IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
            if (it)
            {
                it->MoveFirst();
                while (IEntity* pE = it->Next())
                {
                    const char* cls = pE->GetClass() ? pE->GetClass()->GetName() : nullptr;
                    if (IsRoamingOperatorClass(cls) && !pE->IsHidden())
                        best = min(best, (pE->GetWorldPos() - me).GetLength());
                }
            }
        }
        st.stationDistance = best;
    }

    // Timed autosave
    if (m_s.timedAutosave > 0.0f && !st.pendingDeath && !pHealth->IsDead() && st.graceTimer <= 0.0f)
    {
        const float interval = m_s.timedAutosave * 60.0f;
        if (st.timeSinceAnySave >= interval)
        {
            const float pct = pHealth->GetMaxHealth() > 0.0f ? 100.0f * h / pHealth->GetMaxHealth() : 100.0f;
            const bool calm = st.timeSinceDamage >= m_s.timedCalmSeconds;
            if (pct >= m_s.timedMinHealth && calm && !pPlayer->m_bInTrackview)
            {
                if (ArkGame::GetArkGame())
                {
                    CryLog("Mediumcore: timed autosave ({:.0f} s since the last save)", st.timeSinceAnySave);
                    ModAutoSave(false);
                    st.timeSinceAnySave = 0.0f; // even if the game refused (e.g. mid-transition): try again in a while, not every frame
                    st.timedSaves++;
                }
            }
        }
    }
}

void MediumcoreDeath::ModAutoSave(bool immediate)
{
    ArkGame* pGame = ArkGame::GetArkGame();
    if (!pGame)
        return;
    m_modSaving = true;
    pGame->AutoSave(immediate);
    m_modSaving = false;
}

const char* MediumcoreDeath::SaveBlockReason() const
{
    if (m_s.saveMode == 2)
        return "Manual saving is disabled - the game saves at level transitions and after deaths.";
    if (m_s.saveMode == 1 && !NearSaveStation())
        return "You can only save next to a save station.";
    if (m_s.saveCooldown > 0.0f && m_st.timeSinceManualSave < m_s.saveCooldown * 60.0f)
        return "Saved too recently.";
    return nullptr;
}

bool MediumcoreDeath::AllowManualSave()
{
    if (m_inAutoSaveCheck || m_modSaving)
        return true; // autosaves (the game's and ours) are never restricted here
    return SaveBlockReason() == nullptr;
}

void MediumcoreDeath::OnQuickSaveAction()
{
    if (const char* why = SaveBlockReason())
    {
        m_st.blockedSaves++;
        if (m_s.saveMessages)
        {
            std::string msg = why;
            if (m_s.saveMode == 1 && !NearSaveStation() && m_st.stationDistance < 1e8f)
            {
                char buf[96];
                snprintf(buf, sizeof(buf), " Nearest station: %.0f m.", m_st.stationDistance);
                msg += buf;
            }
            else if (m_s.saveCooldown > 0.0f && m_st.timeSinceManualSave < m_s.saveCooldown * 60.0f && m_s.saveMode != 2)
            {
                char buf[96];
                snprintf(buf, sizeof(buf), " Next save in %.0f s.", m_s.saveCooldown * 60.0f - m_st.timeSinceManualSave);
                msg += buf;
            }
            ShowMessage(msg);
        }
    }
}

bool MediumcoreDeath::AllowSaveLoadMenu(bool bSave)
{
    if (bSave)
    {
        if (const char* why = SaveBlockReason())
        {
            m_st.blockedSaves++;
            if (m_s.saveMessages)
                ShowMessage(why);
            return false;
        }
        return true;
    }
    if (m_s.blockLoadMenu && ArkPlayer::GetInstancePtr())
    {
        m_st.blockedLoads++;
        if (m_s.saveMessages)
            ShowMessage("Loading is disabled during play. Quit to the main menu if you really need to load a save.");
        return false;
    }
    return true;
}

bool MediumcoreDeath::AllowQuickLoad()
{
    // Only while playing: the main menu's Continue must keep working whatever the rules say.
    if (!m_s.blockQuickload || !ArkPlayer::GetInstancePtr())
        return true;
    m_st.blockedLoads++;
    if (m_s.saveMessages)
        ShowMessage("Quick load is disabled. Live with it - or quit to the main menu to load a save.");
    return false;
}

void MediumcoreDeath::OnGameSaved()
{
    m_st.timeSinceAnySave = 0.0f;
}

void MediumcoreDeath::OnManualSaveDone()
{
    m_st.timeSinceManualSave = 0.0f;
    m_st.timeSinceAnySave = 0.0f;
}

//---------------------------------------------------------------------------------
// Per-frame
//---------------------------------------------------------------------------------
void MediumcoreDeath::Update(float dt)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;

    if (!m_spawnsLoaded)
        LoadSpawns();

    // A different level than the one we know about (save loaded into another level, level transition):
    // everything remembered about the current level is stale.
    if (pEnt && m_st.playerSeen)
    {
        const std::string now = CurrentLevelName();
        if (!now.empty() && now != m_st.levelName && m_st.captureEntryFrames == 0)
        {
            CryLog("Mediumcore: level changed '{}' -> '{}'", m_st.levelName, now);
            m_st.playerSeen = false;
            m_st.respawnSaveTimer = 0.0f;
        }
    }

    // Session position: the first frame the player exists after not existing (load, new game, transition).
    if (pEnt)
    {
        if (!m_st.playerSeen && pEnt->GetWorldPos().GetLengthSquared() < 5000.0f * 5000.0f) // not while the player is parked far away during a load
        {
            m_st.playerSeen = true;
            m_st.levelName = CurrentLevelName();
            m_st.levelSpawnPos = pEnt->GetWorldPos();
            m_st.levelSpawnRot = pEnt->GetWorldRotation();
            m_st.levelSpawnValid = true;
            m_st.pendingDeath = false;
            m_st.graceTimer = 0.0f;
            m_st.markerEntity = 0;
            ScanSpawnPoints();
            ScanStations();
            m_st.timeSinceAnySave = 0.0f; // a load or a transition autosave just happened
            m_st.timeSinceDamage = 1e9f;
            m_st.lastHealth = -1.0f;
        }
        else if (m_st.playerSeen && m_spawnPointsLevel != m_st.levelName)
        {
            ScanSpawnPoints();
        }
        if (m_st.captureEntryFrames > 0)
        {
            if (--m_st.captureEntryFrames == 0)
            {
                m_st.levelName = CurrentLevelName();
                if (m_spawnPointsLevel != m_st.levelName)
                    ScanSpawnPoints();
                LevelSpawnRecord& r = CurrentLevelRecord();
                r.entryValid = true;
                r.entryPos = pEnt->GetWorldPos();
                r.entryRot = pEnt->GetWorldRotation();
                // Snap to the arrival spawn point the transition used, if one is right here.
                if (const SpawnPointInfo* sp = NearestSpawnPoint(r.entryPos, 6.0f))
                {
                    r.entryPos = sp->pos;
                    r.entryRot = sp->rot;
                }
                m_st.transitionPending = false;
                SaveSpawns();
                CryLog("Mediumcore: recorded level entry for '{}' at ({:.1f}, {:.1f}, {:.1f})", m_st.levelName, r.entryPos.x, r.entryPos.y, r.entryPos.z);
            }
        }
    }
    else
    {
        m_st.playerSeen = false;
        m_st.pendingDeath = false;
        m_st.graceTimer = 0.0f;
    }

    // Marker on the dropped gear: remove after the timer, or once the entity is gone.
    if (m_st.markerEntity)
    {
        IEntity* pMarked = gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(m_st.markerEntity) : nullptr;
        bool remove = !pMarked || !pPlayer;
        if (m_s.markerTime > 0.0f)
        {
            m_st.markerTimer -= dt;
            if (m_st.markerTimer <= 0.0f)
                remove = true;
        }
        if (pMarked && !remove)
        {
            // picked up = back in an inventory (hidden)
            if (pMarked->IsHidden())
                remove = true;
        }
        if (remove)
            RemoveMarker();
    }

    // Deferred post-respawn save (see DoRespawn).
    if (m_st.respawnSaveTimer > 0.0f)
    {
        m_st.respawnSaveTimer -= dt;
        if (m_st.respawnSaveTimer <= 0.0f)
        {
            ArkPlayerHealthComponent* pHealth = GetHealth(pPlayer);
            if (pPlayer && pHealth && !pHealth->IsDead() && !m_st.pendingDeath)
            {
                CryLog("Mediumcore: post-respawn save ({})", m_s.autosave == 2 ? "quicksave" : "autosave");
                if (m_s.autosave == 2)
                {
                    if (ArkGame* pGame = ArkGame::GetArkGame())
                    {
                        m_modSaving = true;
                        pGame->QuickSave();
                        m_modSaving = false;
                    }
                }
                else
                    ModAutoSave(false);
                m_st.timeSinceAnySave = 0.0f;
            }
            else
                CryLog("Mediumcore: post-respawn save skipped (player not alive)");
        }
    }

    // Post-respawn invulnerability: keep the health where it was.
    if (m_st.graceTimer > 0.0f)
    {
        m_st.graceTimer -= dt;
        if (ArkPlayerHealthComponent* pHealth = GetHealth(pPlayer))
        {
            if (pHealth->GetHealth() < m_st.graceHealth && !pHealth->IsDead())
                pHealth->SetHealth(m_st.graceHealth, false);
        }
    }

    if (m_st.messageTimer > 0.0f)
        m_st.messageTimer -= dt;

    UpdateSaveRules(dt, pPlayer);
}

void MediumcoreDeath::DrawHud()
{
    if (m_st.messageTimer <= 0.0f || m_st.message.empty() || !gEnv || !gEnv->pRenderer)
        return;
    IRenderAuxGeom* pAux = gEnv->pRenderer->GetIRenderAuxGeom(nullptr);
    if (!pAux)
        return;
    const float alpha = clamp_tpl(m_st.messageTimer, 0.0f, 1.0f);
    const float w = (float)gEnv->pRenderer->GetWidth();
    const float h = (float)gEnv->pRenderer->GetHeight();
    pAux->Draw2dLabel(w * 0.5f, h * 0.18f, 2.2f, ColorF(1.0f, 0.85f, 0.6f, alpha), true, "%s", m_st.message.c_str());
}

const char* MediumcoreDeath::KeyName(int keyId) const
{
    if (keyId == 0)
        return "none";
    if (gCL && gCL->cl)
    {
        const auto& names = gCL->cl->GetKeyNames();
        auto it = names.left.find((EKeyId)keyId);
        if (it != names.left.end())
            return it->second.c_str();
    }
    static char buf[32];
    snprintf(buf, sizeof(buf), "key 0x%X", keyId);
    return buf;
}

bool MediumcoreDeath::OnInputEvent(const SInputEvent& event)
{
    if (event.deviceType != eIDT_Keyboard && event.deviceType != eIDT_Mouse && event.deviceType != eIDT_Gamepad)
        return false;
    if (m_waitingForKey)
    {
        if (event.state == eIS_Pressed)
        {
            if (event.keyId != eKI_Escape)
                m_s.spawnKey = (int)event.keyId;
            m_waitingForKey = false;
            return true;
        }
        return false;
    }
    if (m_s.spawnKey == 0 || (int)event.keyId != m_s.spawnKey)
        return false;
    if (event.state == eIS_Pressed)
        SetManualSpawnHere();
    return m_s.spawnKeyConsume != 0;
}

void MediumcoreDeath::OnShutdownGame()
{
    m_st = MediumcoreState();
}

void MediumcoreDeath::SetManualSpawnHere()
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;
    if (!pEnt || (GetHealth(pPlayer) && GetHealth(pPlayer)->IsDead()))
        return;
    m_st.levelName = CurrentLevelName();
    LevelSpawnRecord& r = CurrentLevelRecord();
    r.manualPos = pEnt->GetWorldPos();
    r.manualRot = pEnt->GetWorldRotation();
    r.manualValid = true;
    SaveSpawns();
    ShowMessage("Respawn point set for this level.");
}

void MediumcoreDeath::ClearManualSpawn()
{
    LevelSpawnRecord& r = CurrentLevelRecord();
    r.manualValid = false;
    SaveSpawns();
}

bool MediumcoreDeath::GetRespawnPoint(Vec3& pos, Quat& rot, const char** outSource) const
{
    const LevelSpawnRecord* r = FindLevelRecord();
    auto spawnPoint = [&](const Vec3& near, const char* label) -> bool {
        if (const SpawnPointInfo* sp = NearestSpawnPoint(near, 0.0f))
        {
            pos = sp->pos; rot = sp->rot;
            if (outSource) *outSource = label;
            return true;
        }
        return false;
    };
    auto sessionStart = [&]() -> bool {
        pos = m_st.levelSpawnPos; rot = m_st.levelSpawnRot;
        if (outSource) *outSource = "session start position";
        return m_st.levelSpawnValid;
    };
    auto entry = [&]() -> bool {
        if (r && r->entryValid) { pos = r->entryPos; rot = r->entryRot; if (outSource) *outSource = "level entry"; return true; }
        // Not recorded yet (loaded straight into this level): the level entrance nearest to where the
        // session started is the best guess - never the load position itself.
        if (m_st.levelSpawnValid && spawnPoint(m_st.levelSpawnPos, "level entrance nearest to where the session started (entry not recorded yet)"))
            return true;
        return sessionStart();
    };
    switch (m_s.respawnMode)
    {
    case 4:
        pos = m_st.deathPos; rot = m_st.deathRot;
        if (outSource) *outSource = "death spot";
        return m_st.deathValid;
    case 3:
        return sessionStart();
    case 2:
        if (m_st.deathValid && spawnPoint(m_st.deathPos, "level entrance nearest to the death spot"))
            return true;
        return entry();
    case 1:
        if (r && r->manualValid) { pos = r->manualPos; rot = r->manualRot; if (outSource) *outSource = "your spawn point"; return true; }
        return entry();
    default:
        return entry();
    }
}

//---------------------------------------------------------------------------------
// Extras applied on respawn
//---------------------------------------------------------------------------------
void MediumcoreDeath::DamageWeapons(ArkPlayer* pPlayer)
{
    if (m_s.weaponDamage <= 0.0f || !pPlayer->m_pInventory)
        return;
    ArkGame* pGame = ArkGame::GetArkGame();
    if (!pGame)
        return;
    ArkItemSystem& items = pGame->GetArkItemSystem();
    const std::vector<unsigned> ids = pPlayer->m_pInventory->GetItemIDs();
    for (unsigned id : ids)
    {
        IArkItem* pItem = items.GetItem(id);
        if (!pItem || !pItem->IsWeapon())
            continue;
        CArkWeapon* pWeapon = static_cast<CArkWeapon*>(static_cast<CArkItem*>(pItem));
        if (m_s.weaponDamageRespectDifficulty && !pWeapon->DoesWeaponDegrade())
            continue;
        const float cur = pWeapon->m_condition;
        const float dmg = (m_s.weaponDamageMode == 1) ? cur * clamp_tpl(m_s.weaponDamage, 0.0f, 100.0f) / 100.0f : clamp_tpl(m_s.weaponDamage, 0.0f, 100.0f);
        const float next = max(0.0f, cur - dmg);
        pWeapon->SetWeaponCondition(next);
        if (m_s.logItems)
            CryLog("Mediumcore: weapon {} condition {:.0f} -> {:.0f}", pWeapon->GetEntity() ? pWeapon->GetEntity()->GetName() : "?", cur, next);
    }
}

void MediumcoreDeath::ApplySuitIntegrity(ArkPlayer* pPlayer)
{
    if (m_s.suitIntegrity < 0.0f)
        return;
    ArkTraumaBase* pTrauma = pPlayer->m_playerComponent.GetStatusComponent().GetTraumaForStatus(EArkPlayerStatus::SuitIntegrity);
    if (!pTrauma)
        return;
    // Suit integrity is a trauma that accumulates damage: 0 accumulated = 100 % integrity.
    const float maxAcc = pTrauma->m_maxAccumulation;
    if (maxAcc <= 0.0f)
        return;
    const float want = maxAcc * (1.0f - clamp_tpl(m_s.suitIntegrity, 0.0f, 100.0f) / 100.0f);
    const float cur = pTrauma->m_currentAmount;
    if (want < cur)
        pTrauma->ReduceAccumulation(cur - want, true);
    else if (want > cur)
        pTrauma->Accumulate(want - cur);
}

void MediumcoreDeath::ApplyTrauma(ArkPlayer* pPlayer)
{
    if (m_s.traumaMode == 0)
        return;
    ArkPlayerStatusComponent& status = pPlayer->m_playerComponent.GetStatusComponent();
    struct Pick { EArkPlayerStatus st; int on; const char* name; };
    const Pick picks[] = {
        { EArkPlayerStatus::Bleeding, m_s.traumaBleeding, "bleeding" },
        { EArkPlayerStatus::Burning, m_s.traumaBurning, "burning" },
        { EArkPlayerStatus::Concussion, m_s.traumaConcussion, "concussion" },
        { EArkPlayerStatus::Crippled, m_s.traumaCrippled, "crippled" },
        { EArkPlayerStatus::Disruption, m_s.traumaDisruption, "disruption" },
        { EArkPlayerStatus::Fear, m_s.traumaFear, "fear" },
        { EArkPlayerStatus::PsychoShock, m_s.traumaPsychoShock, "psychoshock" },
        { EArkPlayerStatus::Radiation, m_s.traumaRadiation, "radiation" },
    };
    std::vector<std::pair<ArkTraumaBase*, const char*>> candidates;
    for (const Pick& p : picks)
    {
        if (!p.on)
            continue;
        ArkTraumaBase* pTrauma = status.GetTraumaForStatus(p.st);
        if (pTrauma && pTrauma->IsEnabled() && !pTrauma->IsSuspended())
            candidates.push_back({ pTrauma, p.name });
    }
    if (candidates.empty())
    {
        CryLog("Mediumcore: no enabled trauma to apply (traumas depend on the difficulty options)");
        return;
    }
    const int wantLevel = clamp_tpl(m_s.traumaLevel, 1, 3);
    auto apply = [&](ArkTraumaBase* pTrauma, const char* name)
    {
        int level = wantLevel;
        while (level > 1 && !pTrauma->CanActivate(level))
            level--; // the trauma's config may define fewer levels
        pTrauma->Activate(level);
        CryLog("Mediumcore: applied trauma '{}' level {}", name, level);
    };
    if (m_s.traumaMode == 1)
    {
        const auto& c = candidates[cry_random(0, (int)candidates.size() - 1)];
        apply(c.first, c.second);
    }
    else
    {
        for (const auto& c : candidates)
            apply(c.first, c.second);
    }
}

void MediumcoreDeath::GiveWrench(ArkPlayer* pPlayer)
{
    if (!m_s.giveWrench || !pPlayer->m_pInventory || !pPlayer->GetEntity())
        return;
    ArkGame* pGame = ArkGame::GetArkGame();
    if (!pGame)
        return;
    ArkItemSystem& items = pGame->GetArkItemSystem();
    unsigned wrenchId = 0;
    for (unsigned id : pPlayer->m_pInventory->GetItemIDs())
    {
        IEntity* pEnt = gEnv->pEntitySystem->GetEntity(id);
        if (pEnt && pEnt->GetClass() && StartsWith(pEnt->GetClass()->GetName(), "ArkWeaponWrench"))
        {
            wrenchId = id;
            break;
        }
    }
    if (!wrenchId)
    {
        std::vector<unsigned> given;
        s_fnGiveArchetype(&items, &given, pPlayer->GetEntity()->GetId(), "ArkPickups.Weapons.Wrench", 1);
        if (!given.empty())
            wrenchId = given[0];
        CryLog("Mediumcore: gave a wrench ({} entity)", given.size());
    }
    if (wrenchId && m_s.equipWrench)
        pPlayer->m_weaponComponent.EquipWeapon(wrenchId);
}

static uint64_t MarkerPoiId(int choice)
{
    switch (choice)
    {
    case 1: return 761057047997187194ULL;   // Debris
    case 2: return 761057047997184320ULL;   // Workstation
    case 3: return 13856881013216730525ULL; // OxygenStation
    default: return 13856881013216730636ULL; // Corpse
    }
}

void MediumcoreDeath::PlaceMarker(unsigned entityId)
{
    RemoveMarker();
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !entityId || !pPlayer->m_playerComponent.m_pPOIComponent)
        return;
    IEntity* pEnt = gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr;
    if (!pEnt)
        return;
    CryLog("Mediumcore: placing marker on entity {} '{}'", entityId, pEnt->GetName() ? pEnt->GetName() : "?");
    pPlayer->m_playerComponent.m_pPOIComponent->AddMarker(entityId, MarkerPoiId(m_s.markerPoi), 0.0f, 10000.0f);
    m_st.markerEntity = entityId;
    m_st.markerTimer = m_s.markerTime;
}

void MediumcoreDeath::RemoveMarker()
{
    if (!m_st.markerEntity)
        return;
    if (ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr())
        if (pPlayer->m_playerComponent.m_pPOIComponent)
            pPlayer->m_playerComponent.m_pPOIComponent->RemoveMarker(m_st.markerEntity);
    m_st.markerEntity = 0;
}

//---------------------------------------------------------------------------------
// Death handling
//---------------------------------------------------------------------------------
bool MediumcoreDeath::OnHealthUpdate(ArkPlayerHealthComponent* pHealth, float dt)
{
    if (!m_s.enabled || !pHealth)
        return false;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer || !pPlayer->GetEntity())
        return false;
    if (pPlayer->m_bInTrackview)
        return false; // scripted deaths: leave the game alone

    if (!pHealth->IsDead())
    {
        m_st.pendingDeath = false;
        return false;
    }

    // Dead. DoDeath() already ran (holster, death state, death camera).
    if (pHealth->m_bDeathMenuOpened)
        return false; // the game got there first (e.g. enabled mid-death) - let it run

    if (!m_st.pendingDeath)
    {
        m_st.pendingDeath = true;
        m_st.deathTimer = 0.0f;
        m_st.deathPos = pPlayer->GetEntity()->GetWorldPos();
        m_st.deathRot = pPlayer->GetEntity()->GetWorldRotation();
        m_st.deathValid = true;
        m_st.deaths++;
        CryLog("Mediumcore: player died at ({:.1f}, {:.1f}, {:.1f})", m_st.deathPos.x, m_st.deathPos.y, m_st.deathPos.z);
    }
    m_st.deathTimer += dt;
    if (m_st.deathTimer >= max(m_s.delay, 0.0f))
    {
        m_st.pendingDeath = false;
        DoRespawn(pPlayer, pHealth, true);
    }
    // Swallow the original: it would open the death menu.
    return true;
}

void MediumcoreDeath::RespawnNow(bool dropItems)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    ArkPlayerHealthComponent* pHealth = GetHealth(pPlayer);
    if (!pPlayer || !pHealth || !pPlayer->GetEntity())
        return;
    m_st.deathPos = pPlayer->GetEntity()->GetWorldPos();
    m_st.deathRot = pPlayer->GetEntity()->GetWorldRotation();
    m_st.deathValid = true;
    m_st.pendingDeath = false;
    DoRespawn(pPlayer, pHealth, dropItems);
}

bool MediumcoreDeath::ShouldDrop(IArkItem* pItem, const char* cls, std::string& category) const
{
    const MediumcoreSettings& s = m_s;
    CArkItem* pArkItem = static_cast<CArkItem*>(pItem);
    const bool plotCritical = pArkItem->IsPlotCritical();

    int want = 0;
    if (pItem->IsWeapon() || (StartsWith(cls, "ArkWeapon") && !StartsWith(cls, "ArkWeaponMod")))
    {
        category = "weapon"; want = s.dropWeapons;
    }
    else if (pItem->IsGrenade())
    {
        category = "grenade"; want = s.dropGrenades;
    }
    else if (StartsWith(cls, "ArkAmmo"))
    {
        category = "ammo"; want = s.dropAmmo;
    }
    else if (StartsWith(cls, "ArkNeuroMod"))
    {
        category = "neuromod"; want = s.dropNeuromods;
    }
    else if (StartsWith(cls, "ArkPsychoscopeMod") || StartsWith(cls, "ArkEquipmentMod") || StartsWith(cls, "ArkWeaponMod") || StartsWith(cls, "ArkTrackingChip"))
    {
        category = "chipset"; want = s.dropChipsets;
    }
    else if (StartsWith(cls, "ArkFabricationPlan"))
    {
        category = "plan"; want = s.dropPlans;
    }
    else if (StartsWith(cls, "ArkKeycard") || StartsWith(cls, "ArkPages") || StartsWith(cls, "ArkNote"))
    {
        category = "keycard/note"; want = s.dropKeycardsNotes;
    }
    else if (StartsWith(cls, "ArkRecyclerJunk") || StartsWith(cls, "ArkCraftingIngredient") || pItem->IsTrash() || pArkItem->GetCategory() == CArkItem::Category::junk)
    {
        category = "material"; want = s.dropMaterials;
    }
    else if (StartsWith(cls, "ArkFood") || StartsWith(cls, "ArkAlcohol") || StartsWith(cls, "ArkCure") || StartsWith(cls, "ArkMedKit") ||
             StartsWith(cls, "ArkSuitPatch") || StartsWith(cls, "ArkOxygen") || StartsWith(cls, "ArkConsumable") || StartsWith(cls, "ArkSuperFood") ||
             pArkItem->GetCategory() == CArkItem::Category::consumable)
    {
        category = "consumable"; want = s.dropConsumables;
    }
    else if (pArkItem->GetCategory() == CArkItem::Category::special)
    {
        category = "quest"; want = s.dropQuestItems;
    }
    else
    {
        category = "other"; want = s.dropOther;
    }

    if (plotCritical)
    {
        category += " (plot critical)";
        if (!s.dropPlotCritical)
            return false;
    }
    return want != 0;
}

static CArkItem* pArkItemOf(IArkItem* p) { return static_cast<CArkItem*>(p); }
//! Plot-critical items are never destroyed (dropping them is already opt-in).
static bool plotCriticalSafe(CArkItem* p) { return p && p->IsPlotCritical(); }

static float DestroyPercentFor(const MediumcoreSettings& s, const std::string& category)
{
    if (StartsWith(category.c_str(), "weapon")) return s.destroyWeapons;
    if (StartsWith(category.c_str(), "ammo")) return s.destroyAmmo;
    if (StartsWith(category.c_str(), "grenade")) return s.destroyGrenades;
    if (StartsWith(category.c_str(), "consumable")) return s.destroyConsumables;
    if (StartsWith(category.c_str(), "neuromod")) return s.destroyNeuromods;
    if (StartsWith(category.c_str(), "material")) return s.destroyMaterials;
    if (StartsWith(category.c_str(), "chipset")) return s.destroyChipsets;
    return s.destroyOther;
}

int MediumcoreDeath::DropInventory(ArkPlayer* pPlayer, const Vec3& at)
{
    m_st.lastDropLog.clear();
    m_st.lastDroppedCount = 0;
    m_st.lastKeptCount = 0;
    m_st.lastDestroyedCount = 0;
    if (!pPlayer || !pPlayer->m_pInventory || !gEnv || !gEnv->pEntitySystem)
        return 0;
    ArkGame* pGame = ArkGame::GetArkGame();
    if (!pGame)
        return 0;
    ArkItemSystem& items = pGame->GetArkItemSystem();

    // Copy first: dropping removes entries from the inventory.
    const std::vector<unsigned> ids = pPlayer->m_pInventory->GetItemIDs();

    CArkWeapon* pEquipped = pPlayer->m_weaponComponent.GetEquippedWeapon();
    const unsigned equippedId = (pEquipped && pEquipped->GetEntity()) ? pEquipped->GetEntity()->GetId() : 0;

    m_firstDropped = 0;
    const float radius = clamp_tpl(m_s.scatterRadius, 0.0f, 5.0f);
    const float percent = clamp_tpl(m_s.dropPercent, 0.0f, 100.0f);
    int dropped = 0;
    int index = 0;
    for (unsigned id : ids)
    {
        IArkItem* pItem = items.GetItem(id);
        IEntity* pEnt = gEnv->pEntitySystem->GetEntity(id);
        if (!pItem || !pEnt)
            continue;
        const char* cls = (pEnt->GetClass() && pEnt->GetClass()->GetName()) ? pEnt->GetClass()->GetName() : "";
        std::string category;
        bool drop = ShouldDrop(pItem, cls, category);
        if (drop && m_s.keepEquippedWeapon && id == equippedId)
        {
            drop = false;
            category += " (equipped)";
        }

        const int count = max(pItem->GetCount(), 1);
        int dropCount = count;
        if (pItem->IsStackable() && percent < 100.0f)
            dropCount = clamp_tpl((int)ceilf(count * percent / 100.0f), 0, count);
        if (dropCount <= 0)
            drop = false;

        // Destroyed instead of dropped: a share of the dropped quantity is simply lost.
        int destroyCount = 0;
        if (drop && !plotCriticalSafe(pArkItemOf(pItem)))
        {
            const float dp = clamp_tpl(DestroyPercentFor(m_s, category), 0.0f, 100.0f);
            if (dp > 0.0f)
            {
                if (pItem->IsStackable() && count > 1)
                {
                    // Stochastic rounding, so a 50 % setting is 50 % on average for small stacks too.
                    const float x = dropCount * dp / 100.0f;
                    destroyCount = clamp_tpl((int)x + ((cry_random(0.0f, 1.0f) < x - (int)x) ? 1 : 0), 0, dropCount);
                }
                else
                    destroyCount = (dp >= 100.0f || cry_random(0.0f, 100.0f) < dp) ? dropCount : 0;
            }
            // The weapon in your hands is dropped, never destroyed: the weapon component still refers to it.
            if (id == equippedId)
                destroyCount = 0;
        }
        const int spawnCount = dropCount - destroyCount;

        char line[256];
        snprintf(line, sizeof(line), "%s %s x%d [%s]%s%s", drop ? (spawnCount > 0 ? "DROP" : "DESTROY") : "keep", pEnt->GetName() ? pEnt->GetName() : cls,
                 dropCount, category.c_str(), (drop && dropCount < count) ? " (partial)" : "", (drop && destroyCount > 0 && spawnCount > 0) ? " (part destroyed)" : "");
        if (drop && destroyCount > 0)
        {
            char d[48];
            snprintf(d, sizeof(d), " -%d destroyed", destroyCount);
            strncat(line, d, sizeof(line) - strlen(line) - 1);
        }
        m_st.lastDropLog.push_back(line);
        if (m_s.logItems)
            CryLog("Mediumcore: {}", line);
        if (!drop)
        {
            m_st.lastKeptCount++;
            continue;
        }

        if (destroyCount > 0)
        {
            m_st.lastDestroyedCount += destroyCount;
            if (spawnCount <= 0 && destroyCount >= count)
            {
                // Whole item gone.
                CArkItem* pArk = static_cast<CArkItem*>(pItem);
                pArk->RemoveFromInventory();
                pArk->RemoveEntity();
                continue;
            }
            // Shrink the stack first; the remainder of the dropped share is then dropped normally.
            pItem->ResetCount(count - destroyCount);
            if (spawnCount <= 0)
                continue; // the kept part stays in the inventory
        }

        // Scatter in a ring around the death spot (golden-angle spiral so items do not pile up).
        const float a = index * 2.399963f;
        const float r = radius * sqrtf((index + 1.0f) / (ids.size() + 1.0f));
        const Vec3 pos = at + Vec3(cosf(a) * r, sinf(a) * r, m_s.dropHeight + 0.05f * (index % 3));
        pItem->Drop(spawnCount, &pos);
        if (dropped == 0)
            m_st.markerEntity = 0, m_firstDropped = id;
        dropped++;
        index++;
    }
    m_st.lastDroppedCount = dropped;
    return dropped;
}

void MediumcoreDeath::DoRespawn(ArkPlayer* pPlayer, ArkPlayerHealthComponent* pHealth, bool dropItems)
{
    IEntity* pEnt = pPlayer->GetEntity();
    if (!pEnt)
        return;
    const Vec3 deathPos = m_st.deathPos;
    CryLog("Mediumcore: respawn stage 1 (weapon damage)");

    // Weapon condition first (so dropped weapons carry the damage), then the drop.
    if (m_s.weaponDamageKept || dropItems)
        DamageWeapons(pPlayer);
    CryLog("Mediumcore: respawn stage 2 (drop)");
    int dropped = 0;
    if (dropItems)
        dropped = DropInventory(pPlayer, deathPos);
    CryLog("Mediumcore: respawn stage 3 (teleport), {} dropped", dropped);

    // Where to.
    Vec3 pos = deathPos;
    Quat rot = m_st.deathRot;
    const char* source = "";
    GetRespawnPoint(pos, rot, &source);
    pos.z += m_s.spawnHeightOffset;

    // Stop whatever the ragdoll/physics was doing, then move.
    if (IPhysicalEntity* pPhys = pEnt->GetPhysics())
    {
        pe_action_set_velocity v;
        v.v = Vec3(ZERO);
        pPhys->Action(&v);
    }
    // Keep only the yaw of the recorded rotation.
    Ang3 a(rot);
    Quat yawOnly = Quat::CreateRotationZ(a.z);
    pEnt->SetPosRotScale(pos, yawOnly, Vec3(1.0f), 0);

    // Resurrect: same as the death screen's developer "Revive" button.
    pPlayer->Revive();
    if (pPlayer->m_camera.m_mode == ArkPlayerCamera::Mode::death || pPlayer->m_camera.m_mode == ArkPlayerCamera::Mode::deathByRecycerGrenade)
        pPlayer->m_camera.m_mode = ArkPlayerCamera::Mode::playerControl;
    pPlayer->m_camera.m_rotation = yawOnly;

    CryLog("Mediumcore: respawn stage 4 (health / suit / wrench)");
    // Health after respawn + grace period.
    const float maxHealth = pHealth->GetMaxHealth();
    const float health = max(1.0f, maxHealth * clamp_tpl(m_s.healthPercent, 1.0f, 100.0f) / 100.0f);
    pHealth->SetHealth(health, false);
    m_st.graceHealth = health;
    m_st.graceTimer = max(m_s.gracePeriod, 0.0f);
    ApplySuitIntegrity(pPlayer);
    ApplyTrauma(pPlayer);
    GiveWrench(pPlayer);
    CryLog("Mediumcore: respawn stage 5 (marker / save / message)");
    if (dropped > 0 && m_s.marker && m_firstDropped)
        PlaceMarker(m_firstDropped);

    // The save is deferred: right now the player has just been teleported and revived, the physics /
    // movement state is still settling, and a save taken in that state can load with the player flung
    // away from the level. Update() writes it once the player is alive and steady.
    if (m_s.autosave != 0)
        m_st.respawnSaveTimer = max(m_s.autosaveDelay, 0.25f);

    CryLog("Mediumcore: respawn done");
    if (m_s.message)
    {
        char buf[256];
        const float dist = (pos - deathPos).GetLength();
        if (dropped > 0 && m_st.lastDestroyedCount > 0)
            snprintf(buf, sizeof(buf), "You died. %d item%s dropped where you fell (%.0f m away), %d destroyed. Respawned at %s.", dropped, dropped == 1 ? "" : "s", dist, m_st.lastDestroyedCount, source);
        else if (dropped > 0)
            snprintf(buf, sizeof(buf), "You died. %d item%s dropped where you fell (%.0f m away). Respawned at %s.", dropped, dropped == 1 ? "" : "s", dist, source);
        else if (m_st.lastDestroyedCount > 0)
            snprintf(buf, sizeof(buf), "You died. %d item%s destroyed. Respawned at %s.", m_st.lastDestroyedCount, m_st.lastDestroyedCount == 1 ? "" : "s", source);
        else
            snprintf(buf, sizeof(buf), "You died. Nothing was dropped.");
        ShowMessage(buf);
    }
}

//---------------------------------------------------------------------------------
// Resources
//---------------------------------------------------------------------------------
//! Scales a stack count with stochastic rounding (1.5 x 1 = 1 or 2, 50/50). Never below 1.
static int ScaleCount(int n, float mult)
{
    if (n <= 0 || fabsf(mult - 1.0f) < 0.001f)
        return n;
    const float x = n * clamp_tpl(mult, 0.0f, 10.0f);
    const int r = (int)x + ((cry_random(0.0f, 1.0f) < x - (int)x) ? 1 : 0);
    return max(r, 1);
}

static bool IsAmmoClass(const char* cls) { return StartsWith(cls, "ArkAmmo"); }
static bool IsConsumableClass(const char* cls)
{
    return StartsWith(cls, "ArkFood") || StartsWith(cls, "ArkAlcohol") || StartsWith(cls, "ArkCure") || StartsWith(cls, "ArkMedKit") ||
           StartsWith(cls, "ArkSuitPatch") || StartsWith(cls, "ArkOxygen") || StartsWith(cls, "ArkConsumable") || StartsWith(cls, "ArkSuperFood") ||
           StartsWith(cls, "ArkPsiHypo");
}

float MediumcoreDeath::ScaleHealthChange(float current, float wanted) const
{
    if (wanted <= current || fabsf(m_s.healMult - 1.0f) < 0.001f)
        return wanted; // damage, or nothing to scale
    return current + (wanted - current) * clamp_tpl(m_s.healMult, 0.0f, 10.0f);
}

float MediumcoreDeath::ScaleStatusReduction(uint64_t signalId, float amount) const
{
    if (fabsf(m_s.suitRepairMult - 1.0f) < 0.001f)
        return amount;
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    if (!pPlayer)
        return amount;
    const ArkTraumaBase* pSuit = pPlayer->m_playerComponent.GetStatusComponent().GetTraumaForStatus(EArkPlayerStatus::SuitIntegrity);
    if (pSuit && pSuit->m_id == signalId)
        return amount * clamp_tpl(m_s.suitRepairMult, 0.0f, 10.0f);
    return amount;
}

float MediumcoreDeath::ScalePsi(float points) const
{
    if (points <= 0.0f)
        return points;
    return points * clamp_tpl(m_s.psiMult, 0.0f, 10.0f);
}

void MediumcoreDeath::OnItemCountInitialized(CArkItem* pItem)
{
    IEntity* pEnt = pItem ? pItem->GetEntity() : nullptr;
    if (!pEnt || !pEnt->GetClass())
        return;
    const char* cls = pEnt->GetClass()->GetName();
    float mult = 1.0f;
    if (IsAmmoClass(cls))
        mult = m_s.ammoFoundMult;
    else if (IsConsumableClass(cls))
        mult = m_s.consumablesFoundMult;
    if (fabsf(mult - 1.0f) < 0.001f)
        return;
    const int before = pItem->GetCount();
    const int after = ScaleCount(before, mult);
    if (after != before)
    {
        pItem->ResetCount(after);
        m_scaledPickups++;
        m_lastScaledEntity = pEnt->GetId();
        m_lastScaledBefore = before;
    }
}

void MediumcoreDeath::OnFabricatorSpawnedItem(IEntity* pEnt)
{
    if (fabsf(m_s.ammoFabMult - 1.0f) < 0.001f || !pEnt->GetClass() || !IsAmmoClass(pEnt->GetClass()->GetName()))
        return;
    ArkGame* pGame = ArkGame::GetArkGame();
    if (!pGame)
        return;
    IArkItem* pItem = pGame->GetArkItemSystem().GetItem(pEnt->GetId());
    if (!pItem)
        return;
    // The item was just spawned, so the "ammo found" scaling may have been applied to it a moment ago: undo that.
    int before = pItem->GetCount();
    if (pEnt->GetId() == m_lastScaledEntity && m_lastScaledEntity != 0)
    {
        before = m_lastScaledBefore;
        m_scaledPickups--;
        m_lastScaledEntity = 0;
    }
    const int after = ScaleCount(before, m_s.ammoFabMult);
    if (after != pItem->GetCount())
    {
        pItem->ResetCount(after);
        m_scaledFab++;
        CryLog("Mediumcore: fabricated {} x{} -> x{}", pEnt->GetClass()->GetName(), before, after);
    }
}

int MediumcoreDeath::ScaleLootCount(IEntityArchetype* pArchetype, int count) const
{
    if (!pArchetype || !pArchetype->GetClass() || fabsf(m_s.ammoLootMult - 1.0f) < 0.001f)
        return count;
    if (!IsAmmoClass(pArchetype->GetClass()->GetName()))
        return count;
    const int after = ScaleCount(count, m_s.ammoLootMult);
    if (after != count)
        const_cast<MediumcoreDeath*>(this)->m_scaledLoot++;
    return after;
}

//---------------------------------------------------------------------------------
// Presets: bundles of the individual settings. Key bindings, message and logging options are left alone.
//---------------------------------------------------------------------------------
const char* MediumcoreDeath::PresetName(int preset)
{
    switch (preset)
    {
    case 0: return "Mediumcore classic";
    case 1: return "Gentle";
    case 2: return "Consequences (recommended)";
    case 3: return "Hardcore";
    default: return "?";
    }
}

const char* MediumcoreDeath::PresetDescription(int preset)
{
    switch (preset)
    {
    case 0: return "Terraria rules, nothing else: everything is dropped where you died, you respawn at the level entry with full health.\nSaving and loading are untouched (so reloading is still the easy way out).";
    case 1: return "A taste of consequences: weapons, ammo and grenades are dropped, the game saves right after you respawn and quick load is off,\nbut you can still save anywhere (at most every 5 minutes) and load from the pause menu. Timed autosave every 5 minutes.";
    case 2: return "Death is final but fair: gear is dropped, half of your consumables and a quarter of your ammo are destroyed, you come back\nat 40% health with a suit at 50%, a random mild trauma, a wrench in hand and 15 points of wear on your weapons. Healing and suit repairs are 1.5x, ammo 1.25x. Manual saves only at recyclers, fabricators and operator dispensers;\nno quick load, no loading from the pause menu; the game saves after every respawn and every 10 quiet minutes.";
    case 3: return "No manual saves at all - only level transitions, respawns and a timed autosave every 15 minutes. All consumables and half\nof your ammo, grenades and materials are destroyed on death; you respawn at 25% health, suit at 25%, with every enabled trauma.\nTo compensate: healing and suit repairs 2x, psi hypos 1.5x, ammo 1.5x, consumables found 1.25x.";
    default: return "";
    }
}

void MediumcoreDeath::ApplyPreset(int preset)
{
    MediumcoreSettings& s = m_s;
    const MediumcoreSettings def;
    // Common base: mediumcore on, default drop set, no destruction, no trauma, vanilla saving.
    s.enabled = 1;
    s.delay = def.delay; s.respawnMode = 0; s.spawnHeightOffset = def.spawnHeightOffset;
    s.healthPercent = 100.0f; s.gracePeriod = 3.0f; s.keepEquippedWeapon = 0; s.suitIntegrity = -1.0f;
    s.giveWrench = 0; s.equipWrench = 1;
    s.weaponDamage = 0.0f; s.weaponDamageMode = 0; s.weaponDamageRespectDifficulty = 1; s.weaponDamageKept = 1;
    s.dropWeapons = 1; s.dropAmmo = 1; s.dropGrenades = 1; s.dropConsumables = 1; s.dropNeuromods = 1; s.dropMaterials = 1; s.dropChipsets = 1;
    s.dropPlans = 0; s.dropKeycardsNotes = 0; s.dropQuestItems = 0; s.dropOther = 1; s.dropPlotCritical = 0;
    s.dropPercent = 100.0f; s.scatterRadius = def.scatterRadius; s.dropHeight = def.dropHeight;
    s.destroyWeapons = s.destroyAmmo = s.destroyGrenades = s.destroyConsumables = s.destroyNeuromods = s.destroyMaterials = s.destroyChipsets = s.destroyOther = 0.0f;
    s.traumaMode = 0; s.traumaLevel = 1;
    s.traumaBleeding = 1; s.traumaConcussion = 1; s.traumaCrippled = 1; s.traumaBurning = s.traumaDisruption = s.traumaFear = s.traumaPsychoShock = s.traumaRadiation = 0;
    s.autosave = 0;
    s.saveMode = 0; s.stationRecycler = 1; s.stationFabricator = 1; s.stationDispenser = 1; s.stationOxygen = 0; s.stationOperators = 0; s.stationRadius = 4.0f;
    s.saveCooldown = 0.0f; s.blockQuickload = 0; s.blockLoadMenu = 0; s.timedAutosave = 0.0f; s.timedMinHealth = 50.0f; s.timedCalmSeconds = 20.0f;
    s.healMult = s.suitRepairMult = s.psiMult = s.ammoFoundMult = s.ammoLootMult = s.ammoFabMult = s.consumablesFoundMult = 1.0f;

    switch (preset)
    {
    case 0: // classic
        break;
    case 1: // gentle
        s.dropConsumables = 0; s.dropNeuromods = 0; s.dropMaterials = 0; s.dropChipsets = 0; s.dropOther = 0;
        s.giveWrench = 1;
        s.autosave = 1;
        s.blockQuickload = 1;
        s.saveCooldown = 5.0f;
        s.timedAutosave = 5.0f;
        break;
    case 2: // consequences
        s.healthPercent = 40.0f; s.suitIntegrity = 50.0f; s.gracePeriod = 4.0f;
        s.giveWrench = 1;
        s.weaponDamage = 15.0f;
        s.destroyConsumables = 50.0f; s.destroyAmmo = 25.0f;
        s.traumaMode = 1;
        s.autosave = 1;
        s.saveMode = 1; s.blockQuickload = 1; s.blockLoadMenu = 1;
        s.timedAutosave = 10.0f;
        s.healMult = 1.5f; s.suitRepairMult = 1.5f; s.ammoFoundMult = 1.25f; s.ammoLootMult = 1.25f; s.ammoFabMult = 1.25f;
        break;
    case 3: // hardcore
        s.healthPercent = 25.0f; s.suitIntegrity = 25.0f; s.gracePeriod = 3.0f;
        s.giveWrench = 1;
        s.weaponDamage = 30.0f;
        s.destroyConsumables = 100.0f; s.destroyAmmo = 50.0f; s.destroyGrenades = 50.0f; s.destroyMaterials = 50.0f;
        s.traumaMode = 2; s.traumaBurning = 1; s.traumaDisruption = 1; s.traumaFear = 1;
        s.autosave = 1;
        s.saveMode = 2; s.blockQuickload = 1; s.blockLoadMenu = 1;
        s.timedAutosave = 15.0f; s.timedMinHealth = 30.0f;
        s.healMult = 2.0f; s.suitRepairMult = 2.0f; s.psiMult = 1.5f; s.ammoFoundMult = 1.5f; s.ammoLootMult = 1.5f; s.ammoFabMult = 1.5f; s.consumablesFoundMult = 1.25f;
        break;
    default:
        break;
    }
    CryLog("Mediumcore: applied preset '{}'", PresetName(preset));
}

//---------------------------------------------------------------------------------
// ImGui
//---------------------------------------------------------------------------------
static bool McCheckbox(const char* label, int& value, const char* tooltip = nullptr)
{
    bool b = value != 0;
    bool changed = ImGui::Checkbox(label, &b);
    if (changed)
        value = b ? 1 : 0;
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

void MediumcoreDeath::DrawSettings()
{
    MediumcoreSettings& s = m_s;
    if (!m_hookInstalled)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Warning: the health-update hook is not installed, mediumcore cannot work.");

    if (ImGui::CollapsingHeader("Presets"))
    {
        ImGui::TextWrapped("A preset sets every option below at once (except key bindings, messages and logging). Tweak afterwards as you like.");
        for (int i = 0; i < kPresetCount; ++i)
        {
            if (ImGui::Button(PresetName(i)))
                ApplyPreset(i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", PresetDescription(i));
            if (i + 1 < kPresetCount)
                ImGui::SameLine();
        }
    }

    if (ImGui::CollapsingHeader("Saving & loading", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!m_saveHooksInstalled)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Warning: the save hooks are not installed; these rules have no effect.");
        ImGui::TextWrapped("These rules work on their own, with or without mediumcore death. The game's own autosaves (level transitions) are never blocked, and the main menu is never touched.");
        const char* modes[] = { "Game default (save anywhere)", "Manual saves only near a save station", "No manual saves (autosaves only)" };
        ImGui::Combo("Manual saves", &s.saveMode, modes, 3);
        if (s.saveMode == 1)
        {
            ImGui::Indent();
            ImGui::Text("Save stations:");
            ImGui::SameLine(); McCheckbox("Recyclers", s.stationRecycler);
            ImGui::SameLine(); McCheckbox("Fabricators", s.stationFabricator);
            ImGui::SameLine(); McCheckbox("Operator dispensers", s.stationDispenser);
            ImGui::SameLine(); McCheckbox("Oxygen stations", s.stationOxygen);
            ImGui::SameLine(); McCheckbox("Roaming operators", s.stationOperators, "Medical, engineering, science and military operators floating around.");
            ImGui::SliderFloat("Station radius", &s.stationRadius, 1.0f, 15.0f, "%.1f m");
            if (m_st.stationDistance < 1e8f)
                ImGui::TextDisabled("%d station(s) in this level, nearest %.0f m away%s", (int)m_stations.size(), m_st.stationDistance, NearSaveStation() ? " - you can save here" : "");
            ImGui::Unindent();
        }
        if (s.saveMode != 2)
            ImGui::SliderFloat("Minimum time between manual saves", &s.saveCooldown, 0.0f, 30.0f, s.saveCooldown <= 0.0f ? "none" : "%.0f min");
        McCheckbox("Block quick load (F9)", s.blockQuickload, "Also the pause menu's quick load entry.");
        McCheckbox("Block Load Game in the pause menu", s.blockLoadMenu, "Loading then means quitting to the main menu first - deliberate, not a reflex.");
        ImGui::SliderFloat("Timed autosave", &s.timedAutosave, 0.0f, 30.0f, s.timedAutosave <= 0.0f ? "off" : "every %.0f min");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The mod saves on its own at this interval (counted from the last save of any kind), when things are calm.");
        if (s.timedAutosave > 0.0f)
        {
            ImGui::Indent();
            ImGui::SliderFloat("... only above", &s.timedMinHealth, 0.0f, 100.0f, "%.0f %% health");
            ImGui::SliderFloat("... and no damage for", &s.timedCalmSeconds, 0.0f, 120.0f, "%.0f s");
            ImGui::Unindent();
        }
        McCheckbox("HUD message when a save or load is blocked", s.saveMessages);
        ImGui::TextDisabled("Blocked this session: %d save(s), %d load(s). Timed autosaves: %d. Last save %.0f s ago.", m_st.blockedSaves, m_st.blockedLoads, m_st.timedSaves, m_st.timeSinceAnySave);
    }

    McCheckbox("Enable mediumcore death", s.enabled,
        "When you die: after a short delay your inventory is dropped where you fell and you respawn.\n"
        "The death menu never opens. Works with the existing save system (dropped items are normal world items).");
    ImGui::BeginDisabled(!s.enabled);

    if (ImGui::CollapsingHeader("Respawn", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Delay before respawn", &s.delay, 0.0f, 10.0f, "%.1f s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The death camera plays during this time.");
        const char* modes[] = { "Level entry (the entrance you came through)", "Your spawn point for this level (falls back to level entry)", "Level entrance nearest to where you died", "Where this session started (load / new game position)", "Where you died" };
        ImGui::Combo("Respawn at", &s.respawnMode, modes, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Level entry and your spawn points are remembered per level in Mods/config/Vee.Mediumcore.spawns.xml,\n"
                              "so loading a save does not move them. The entry is recorded when you arrive through a door, airlock or\n"
                              "elevator (snapped to the level's arrival point). Until that has happened once, the level entrance nearest\n"
                              "to where the session started is used - never the load position itself.\n"
                              "'Level entrance' = one of the level's transition arrival points (SpawnPoint entities).");
        ImGui::SliderFloat("Height offset", &s.spawnHeightOffset, 0.0f, 1.0f, "%.2f m");
        {
            const LevelSpawnRecord* r = FindLevelRecord();
            ImGui::Text("Level '%s': entry %s | your spawn point %s | %d level entrances found", m_st.levelName.c_str(),
                r && r->entryValid ? "recorded" : "not recorded yet",
                r && r->manualValid ? "set" : "not set", (int)m_spawnPoints.size());
            Vec3 p; Quat q; const char* src = "";
            if (GetRespawnPoint(p, q, &src))
            {
                ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
                const float d = (pPlayer && pPlayer->GetEntity()) ? (pPlayer->GetEntity()->GetWorldPos() - p).GetLength() : 0.0f;
                ImGui::TextDisabled("Would respawn at: %s (%.1f, %.1f, %.1f), %.0f m from you", src, p.x, p.y, p.z, d);
            }
        }
        if (ImGui::Button("Set spawn point here"))
            SetManualSpawnHere();
        ImGui::SameLine();
        if (ImGui::Button("Clear spawn point"))
            ClearManualSpawn();
        ImGui::SameLine();
        if (ImGui::Button("Use current position as level entry"))
        {
            ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
            if (pPlayer && pPlayer->GetEntity())
            {
                m_st.levelName = CurrentLevelName();
                LevelSpawnRecord& r = CurrentLevelRecord();
                r.entryValid = true; r.entryPos = pPlayer->GetEntity()->GetWorldPos(); r.entryRot = pPlayer->GetEntity()->GetWorldRotation();
                if (const SpawnPointInfo* sp = NearestSpawnPoint(r.entryPos, 6.0f)) { r.entryPos = sp->pos; r.entryRot = sp->rot; }
                SaveSpawns();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stand where you want this level's entry to be (e.g. at the airlock / elevator you use) and click.");
        ImGui::Text("Spawn key: %s", KeyName(s.spawnKey));
        ImGui::SameLine();
        if (m_waitingForKey)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a key... (Esc cancels)");
        else if (ImGui::Button("Bind##mcspawn"))
            m_waitingForKey = true;
        ImGui::SameLine();
        if (ImGui::Button("None##mcspawn"))
            s.spawnKey = 0;
        McCheckbox("Swallow the spawn key", s.spawnKeyConsume, "The game does not see the key while it is bound here.");

        ImGui::SliderFloat("Health after respawn", &s.healthPercent, 1.0f, 100.0f, "%.0f %%");
        ImGui::SliderFloat("Suit integrity after respawn", &s.suitIntegrity, -1.0f, 100.0f, s.suitIntegrity < 0.0f ? "leave as it is" : "%.0f %%");
        ImGui::SliderFloat("Invulnerability after respawn", &s.gracePeriod, 0.0f, 15.0f, "%.1f s");
        McCheckbox("Always respawn with a wrench", s.giveWrench, "Gives you a wrench if you have none left.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.giveWrench);
        McCheckbox("and hold it", s.equipWrench);
        ImGui::EndDisabled();

        const char* tm[] = { "None", "One random trauma from the list", "Every trauma in the list" };
        ImGui::Combo("Trauma after respawn", &s.traumaMode, tm, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Treat it with the matching consumable. Only traumas that exist at your difficulty settings can be applied\n(the 'All traumas' difficulty option, or Nightmare, enables all of them).");
        if (s.traumaMode != 0)
        {
            ImGui::Indent();
            ImGui::SliderInt("Severity", &s.traumaLevel, 1, 3);
            ImGui::Columns(4, nullptr, false);
            McCheckbox("Bleeding", s.traumaBleeding); ImGui::NextColumn();
            McCheckbox("Burning", s.traumaBurning); ImGui::NextColumn();
            McCheckbox("Concussion", s.traumaConcussion); ImGui::NextColumn();
            McCheckbox("Crippled", s.traumaCrippled); ImGui::NextColumn();
            McCheckbox("Disruption", s.traumaDisruption); ImGui::NextColumn();
            McCheckbox("Fear", s.traumaFear); ImGui::NextColumn();
            McCheckbox("Psychoshock", s.traumaPsychoShock); ImGui::NextColumn();
            McCheckbox("Radiation", s.traumaRadiation);
            ImGui::Columns(1);
            ImGui::Unindent();
        }
    }

    if (ImGui::CollapsingHeader("Weapon damage on death", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* dm[] = { "Condition points (0-100)", "Percent of current condition" };
        ImGui::Combo("Damage mode", &s.weaponDamageMode, dm, 2);
        ImGui::SliderFloat("Damage per death", &s.weaponDamage, 0.0f, 100.0f, s.weaponDamageMode == 1 ? "%.0f %%" : "%.0f points");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Taken from every weapon you carried when you died. Weapons break below their malfunction threshold\n(repairable with spare parts as usual). 0 = off.");
        McCheckbox("Only weapons that degrade at this difficulty", s.weaponDamageRespectDifficulty,
            "Weapon degradation is a difficulty option; with it off the game never degrades weapons and has no repair prompt.");
        McCheckbox("Also damage the weapons you keep", s.weaponDamageKept);
    }

    if (ImGui::CollapsingHeader("What is dropped", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Columns(2, nullptr, false);
        McCheckbox("Weapons", s.dropWeapons);
        McCheckbox("Ammo", s.dropAmmo);
        McCheckbox("Grenades", s.dropGrenades);
        McCheckbox("Food / medical / suit / oxygen", s.dropConsumables);
        McCheckbox("Neuromods", s.dropNeuromods);
        McCheckbox("Materials / junk", s.dropMaterials);
        ImGui::NextColumn();
        McCheckbox("Chipsets (in inventory)", s.dropChipsets);
        McCheckbox("Fabrication plans", s.dropPlans);
        McCheckbox("Keycards / notes", s.dropKeycardsNotes, "Dropping keycards can lock you out of areas until you fetch them back.");
        McCheckbox("Quest items", s.dropQuestItems, "Items the game files as 'special'.");
        McCheckbox("Everything else", s.dropOther);
        McCheckbox("Plot-critical items too", s.dropPlotCritical, "Overrides the safety that keeps items the game marks plot critical. Can break progression.");
        ImGui::Columns(1);
        McCheckbox("Keep the weapon in your hands", s.keepEquippedWeapon);
        ImGui::SliderFloat("Portion of each stack", &s.dropPercent, 0.0f, 100.0f, "%.0f %%");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("For stackable items (ammo, materials...): drop only this share, rounded up. Non-stackables are all or nothing.");
        ImGui::SliderFloat("Scatter radius", &s.scatterRadius, 0.0f, 3.0f, "%.2f m");
        ImGui::SliderFloat("Drop height", &s.dropHeight, 0.0f, 1.5f, "%.2f m");
    }

    if (ImGui::CollapsingHeader("Destroyed instead of dropped", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Share of each dropped category that is simply lost. Holding on to things becomes a risk - use them before something eats you. Stacks are reduced; single items are rolled. Plot-critical items are never destroyed.");
        auto pct = [](const char* label, float& v) { ImGui::SliderFloat(label, &v, 0.0f, 100.0f, v <= 0.0f ? "none" : "%.0f %%"); };
        ImGui::Columns(2, nullptr, false);
        pct("Consumables##d", s.destroyConsumables);
        pct("Ammo##d", s.destroyAmmo);
        pct("Grenades##d", s.destroyGrenades);
        pct("Materials / junk##d", s.destroyMaterials);
        ImGui::NextColumn();
        pct("Weapons##d", s.destroyWeapons);
        pct("Chipsets##d", s.destroyChipsets);
        pct("Neuromods##d", s.destroyNeuromods);
        pct("Everything else##d", s.destroyOther);
        ImGui::Columns(1);
    }

    ImGui::EndDisabled(); // resources work with or without mediumcore death
    if (ImGui::CollapsingHeader("Resources & consumables", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Dying instead of reloading burns through more supplies over a playthrough; these put some back. 1.00 = the game's values. Apply to items found from now on (not to what you already carry).");
        auto mult = [](const char* label, float& v, const char* tip) {
            ImGui::SliderFloat(label, &v, 0.25f, 4.0f, "x%.2f");
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        mult("Healing", s.healMult, "Medkits, food, drinks and medical operators.");
        mult("Suit repair", s.suitRepairMult, "Suit repair kits.");
        mult("Psi hypos", s.psiMult, nullptr);
        mult("Ammo found", s.ammoFoundMult, "Ammo lying around and in containers.");
        mult("Ammo from enemies", s.ammoLootMult, "Ammo dropped by killed enemies.");
        mult("Ammo per fabrication", s.ammoFabMult, "How much ammo a fabricator makes per plan use (same materials).");
        mult("Consumables found", s.consumablesFoundMult, "Medkits, food, suit patches, psi hypos lying around and in containers. Single items: x1.5 means a 50 %% chance of finding two.");
        ImGui::TextDisabled("This session: %d pickups, %d enemy drops, %d fabrications scaled.", m_scaledPickups, m_scaledLoot, m_scaledFab);
    }
    ImGui::BeginDisabled(!s.enabled);

    if (ImGui::CollapsingHeader("After respawn", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* saves[] = { "Do not save", "Autosave", "Quicksave" };
        ImGui::Combo("Save", &s.autosave, saves, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saving right after the respawn is what makes death stick: loading the last save cannot undo it.\nThis save is never blocked by the saving rules above.");
        if (s.autosave != 0)
            ImGui::SliderFloat("... after", &s.autosaveDelay, 0.25f, 10.0f, "%.1f s");
        if (s.autosave != 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Gives the game a moment to settle the revived player before the save is written.");
        McCheckbox("Show HUD message", s.message);
        ImGui::BeginDisabled(!s.message);
        ImGui::SliderFloat("Message duration", &s.messageTime, 1.0f, 20.0f, "%.0f s");
        ImGui::EndDisabled();
        McCheckbox("Log dropped items to the game log", s.logItems);
        McCheckbox("Mark the dropped gear (experimental)", s.marker, "Attaches one of the game's point-of-interest markers (HUD / map) to the first dropped item.\nUses a game system in a way it was not built for; turn off if respawning crashes.");
        ImGui::BeginDisabled(!s.marker);
        const char* pois[] = { "Corpse", "Debris", "Workstation", "Oxygen station" };
        ImGui::Combo("Marker icon", &s.markerPoi, pois, 4);
        ImGui::SliderFloat("Marker lifetime", &s.markerTime, 0.0f, 1800.0f, s.markerTime <= 0.0f ? "until picked up" : "%.0f s");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Status / testing"))
    {
        ImGui::Text("Deaths this session: %d | last respawn: %d dropped, %d kept, %d destroyed", m_st.deaths, m_st.lastDroppedCount, m_st.lastKeptCount, m_st.lastDestroyedCount);
        ImGui::Text("Level entry point: %s | pending death: %s (%.1f s) | grace: %.1f s", m_st.levelSpawnValid ? "recorded" : "-",
            m_st.pendingDeath ? "yes" : "no", m_st.deathTimer, max(m_st.graceTimer, 0.0f));
        if (m_st.deathValid)
        {
            ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
            if (pPlayer && pPlayer->GetEntity())
                ImGui::Text("Last death spot: (%.1f, %.1f, %.1f), %.1f m from you", m_st.deathPos.x, m_st.deathPos.y, m_st.deathPos.z,
                    (pPlayer->GetEntity()->GetWorldPos() - m_st.deathPos).GetLength());
        }
        if (ImGui::Button("Test: drop inventory + respawn now"))
            RespawnNow(true);
        ImGui::SameLine();
        if (ImGui::Button("Test: respawn (no drop)"))
            RespawnNow(false);
        ImGui::SameLine();
        if (ImGui::Button("Test: kill me"))
        {
            if (ArkPlayerHealthComponent* pHealth = GetHealth(ArkPlayer::GetInstancePtr()))
                pHealth->ForceKill();
        }
        if (!m_spawnPoints.empty() && ImGui::TreeNode("Level entrances"))
        {
            ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
            const Vec3 me = (pPlayer && pPlayer->GetEntity()) ? pPlayer->GetEntity()->GetWorldPos() : Vec3(ZERO);
            for (const auto& sp : m_spawnPoints)
                ImGui::Text("%s (%s)%s  %.0f m away", sp.name.c_str(), sp.destination.c_str(), sp.initial ? " [initial]" : "", (sp.pos - me).GetLength());
            ImGui::TreePop();
        }
        if (!m_st.lastDropLog.empty() && ImGui::TreeNode("Last drop list"))
        {
            for (const auto& l : m_st.lastDropLog)
                ImGui::TextUnformatted(l.c_str());
            ImGui::TreePop();
        }
    }
    ImGui::EndDisabled();
}
