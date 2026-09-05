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

void MediumcoreDeath::InitHooks()
{
    s_hookHealthUpdate.SetHookFunc(&ArkPlayerHealthComponent_Update_Hook);
    s_hookTransitionFinished.SetHookFunc(&ArkGame_OnLevelTransitionFinished_Hook);
}

//---------------------------------------------------------------------------------
// CVars
//---------------------------------------------------------------------------------
void MediumcoreDeath::RegisterCVars()
{
    MediumcoreSettings& s = m_s;
    m_hookInstalled = s_hookHealthUpdate.IsHooked();
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
    REGISTER_CVAR2("mc_message", &s.message, s.message, VF_DUMPTOCHAIR, "Mediumcore: show a HUD message after respawning (0/1)");
    REGISTER_CVAR2("mc_message_time", &s.messageTime, s.messageTime, VF_DUMPTOCHAIR, "Mediumcore: HUD message duration in seconds");
    REGISTER_CVAR2("mc_log_items", &s.logItems, s.logItems, VF_DUMPTOCHAIR, "Mediumcore: log every dropped item (0/1)");
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
// Per-frame
//---------------------------------------------------------------------------------
void MediumcoreDeath::Update(float dt)
{
    ArkPlayer* pPlayer = ArkPlayer::GetInstancePtr();
    IEntity* pEnt = pPlayer ? pPlayer->GetEntity() : nullptr;

    if (!m_spawnsLoaded)
        LoadSpawns();

    // Session position: the first frame the player exists after not existing (load, new game, transition).
    if (pEnt)
    {
        if (!m_st.playerSeen)
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
        }
        else if (m_spawnPointsLevel != m_st.levelName)
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

int MediumcoreDeath::DropInventory(ArkPlayer* pPlayer, const Vec3& at)
{
    m_st.lastDropLog.clear();
    m_st.lastDroppedCount = 0;
    m_st.lastKeptCount = 0;
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

        char line[256];
        snprintf(line, sizeof(line), "%s %s x%d [%s]%s", drop ? "DROP" : "keep", pEnt->GetName() ? pEnt->GetName() : cls, dropCount, category.c_str(),
                 (drop && dropCount < count) ? " (partial)" : "");
        m_st.lastDropLog.push_back(line);
        if (m_s.logItems)
            CryLog("Mediumcore: {}", line);
        if (!drop)
        {
            m_st.lastKeptCount++;
            continue;
        }

        // Scatter in a ring around the death spot (golden-angle spiral so items do not pile up).
        const float a = index * 2.399963f;
        const float r = radius * sqrtf((index + 1.0f) / (ids.size() + 1.0f));
        const Vec3 pos = at + Vec3(cosf(a) * r, sinf(a) * r, m_s.dropHeight + 0.05f * (index % 3));
        pItem->Drop(dropCount, &pos);
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
    GiveWrench(pPlayer);
    CryLog("Mediumcore: respawn stage 5 (marker / save / message)");
    if (dropped > 0 && m_s.marker && m_firstDropped)
        PlaceMarker(m_firstDropped);

    if (m_s.autosave == 1)
        ArkGame::GetArkGame()->AutoSave(true);
    else if (m_s.autosave == 2)
        ArkGame::GetArkGame()->QuickSave();

    CryLog("Mediumcore: respawn done");
    if (m_s.message)
    {
        char buf[256];
        const float dist = (pos - deathPos).GetLength();
        if (dropped > 0)
            snprintf(buf, sizeof(buf), "You died. %d item%s dropped where you fell (%.0f m away). Respawned at %s.", dropped, dropped == 1 ? "" : "s", dist, source);
        else
            snprintf(buf, sizeof(buf), "You died. Nothing was dropped.");
        ShowMessage(buf);
    }
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

    if (ImGui::CollapsingHeader("After respawn", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* saves[] = { "Do not save", "Autosave", "Quicksave" };
        ImGui::Combo("Save", &s.autosave, saves, 3);
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
        ImGui::Text("Deaths this session: %d | last respawn: %d dropped, %d kept", m_st.deaths, m_st.lastDroppedCount, m_st.lastKeptCount);
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
