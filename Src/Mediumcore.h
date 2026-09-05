#pragma once
// Mediumcore death (Terraria style): dying drops your inventory where you fell and respawns you
// instead of showing the death menu. Self-contained so it can be extracted into its own mod later.
#include <Prey/CryInput/IInput.h>
#include <string>
#include <vector>
#include <map>

class ArkPlayer;
class ArkPlayerHealthComponent;

struct MediumcoreSettings
{
    int   enabled = 0;

    // Timing
    float delay = 2.5f;             //!< seconds between dying and respawning (death camera plays meanwhile)
    float fadeTime = 0.0f;          //!< reserved

    // Respawn location:
    //  0 = level entry: where you came into this level through a door/airlock/elevator (remembered per level
    //      in a config file, so a save load does not move it; falls back to 2 until you have entered the level once)
    //  1 = spawn point you set yourself (remembered per level in the same file; falls back to 0)
    //  2 = level entrance (SpawnPoint) nearest to the death spot
    //  3 = where the current game session put you in this level (load / new game position)
    //  4 = where you died (no teleport)
    int   respawnMode = 0;
    int   spawnKey = 0;             //!< EKeyId that sets the manual spawn point (0 = none)
    int   spawnKeyConsume = 1;
    float spawnHeightOffset = 0.1f; //!< meters above the recorded point (avoid clipping into the floor)

    // State after respawn
    float healthPercent = 100.0f;   //!< % of max health after respawn
    float gracePeriod = 3.0f;       //!< seconds of invulnerability after respawn
    int   keepEquippedWeapon = 0;   //!< don't drop the weapon in your hands
    float suitIntegrity = -1.0f;    //!< suit integrity % after respawn (-1 = leave as it is)
    int   giveWrench = 0;           //!< make sure you respawn with a wrench
    int   equipWrench = 1;          //!< ... and hold it
    float weaponDamage = 0.0f;      //!< condition points (0..100) taken from every weapon you had when dying
    int   weaponDamageMode = 0;     //!< 0 = flat points, 1 = percent of the weapon's current condition
    int   weaponDamageRespectDifficulty = 1; //!< only if the difficulty option "weapon degradation" is on for that weapon
    int   weaponDamageKept = 1;     //!< also damage weapons that are not dropped

    // What gets dropped (each 0/1)
    int   dropWeapons = 1;
    int   dropAmmo = 1;
    int   dropGrenades = 1;
    int   dropConsumables = 1;      //!< food, drink, medkits, suit patches, oxygen
    int   dropNeuromods = 1;
    int   dropMaterials = 1;        //!< recycler junk / crafting materials
    int   dropChipsets = 1;         //!< suit / psychoscope / weapon mods
    int   dropPlans = 0;            //!< fabrication plans
    int   dropKeycardsNotes = 0;    //!< keycards, notes, pages
    int   dropQuestItems = 0;       //!< "special" category items
    int   dropOther = 1;
    int   dropPlotCritical = 0;     //!< safety override: also drop items the game marks plot critical
    float dropPercent = 100.0f;     //!< % of each stack that is dropped (rounded up, min 1)
    float scatterRadius = 0.7f;     //!< meters around the death spot
    float dropHeight = 0.4f;        //!< meters above the death spot

    // After respawn
    int   autosave = 0;             //!< 0 = none, 1 = autosave, 2 = quicksave
    int   message = 1;              //!< HUD message
    float messageTime = 8.0f;
    int   logItems = 1;             //!< write the dropped items to the log
    int   marker = 0;               //!< put a HUD/map marker on the dropped gear (experimental)
    int   markerPoi = 0;            //!< 0 corpse, 1 debris, 2 workstation, 3 oxygen station
    float markerTime = 0.0f;        //!< seconds until the marker is removed (0 = until the item is gone / picked up)
};

//! Per-level remembered positions (saved to Mods/config/Vee.Mediumcore.spawns.xml).
struct LevelSpawnRecord
{
    bool entryValid = false;        //!< set by a real level-to-level transition
    Vec3 entryPos = Vec3(ZERO);
    Quat entryRot = Quat(IDENTITY);
    bool manualValid = false;
    Vec3 manualPos = Vec3(ZERO);
    Quat manualRot = Quat(IDENTITY);
};

//! A level transition arrival point ("SpawnPoint" entity) found in the loaded level.
struct SpawnPointInfo
{
    unsigned entityId = 0;
    Vec3 pos = Vec3(ZERO);
    Quat rot = Quat(IDENTITY);
    std::string name;
    std::string destination;    //!< Properties.destinationName, e.g. "From_Psychotronics"
    bool initial = false;
};

struct MediumcoreState
{
    bool pendingDeath = false;
    float deathTimer = 0.0f;
    Vec3 deathPos = Vec3(ZERO);
    Quat deathRot = Quat(IDENTITY);
    bool deathValid = false;        //!< a death spot exists (for the distance readout)

    bool levelSpawnValid = false;   //!< session position (load / new game)
    Vec3 levelSpawnPos = Vec3(ZERO);
    Quat levelSpawnRot = Quat(IDENTITY);
    bool playerSeen = false;
    std::string levelName;
    int captureEntryFrames = 0;     //!< >0: a level transition just finished, record the entry in N frames
    bool transitionPending = false;

    unsigned markerEntity = 0;
    float markerTimer = 0.0f;

    float graceTimer = 0.0f;
    float graceHealth = 0.0f;
    float messageTimer = 0.0f;
    std::string message;
    int deaths = 0;
    int lastDroppedCount = 0;
    int lastKeptCount = 0;
    std::vector<std::string> lastDropLog;
};

class MediumcoreDeath
{
public:
    void InitHooks();
    void RegisterCVars();
    void Update(float dt);               //!< once per frame (MainUpdate)
    void DrawHud();                      //!< once per frame after the game rendered its HUD (LateUpdate)
    void DrawSettings();                 //!< ImGui tab content
    bool OnInputEvent(const SInputEvent& event);
    void OnShutdownGame();

    //! Hook body for ArkPlayerHealthComponent::Update. Returns true when the original must be skipped.
    bool OnHealthUpdate(ArkPlayerHealthComponent* pHealth, float dt);

    void SetManualSpawnHere();
    void ClearManualSpawn();
    void OnLevelTransitionFinished();   //!< hook body (ArkGame::OnLevelTransitionFinished, post)
    void RespawnNow(bool dropItems);     //!< debug: run the respawn sequence immediately

    MediumcoreSettings& Settings() { return m_s; }
    bool HookInstalled() const { return m_hookInstalled; }
    const char* KeyName(int keyId) const;

private:
    MediumcoreSettings m_s;
    MediumcoreState m_st;
    bool m_hookInstalled = false;
    bool m_waitingForKey = false;

    void DoRespawn(ArkPlayer* pPlayer, ArkPlayerHealthComponent* pHealth, bool dropItems);
    int DropInventory(ArkPlayer* pPlayer, const Vec3& at);
    bool ShouldDrop(struct IArkItem* pItem, const char* className, std::string& category) const;
    bool GetRespawnPoint(Vec3& pos, Quat& rot, const char** outSource) const;
    void ShowMessage(const std::string& text);
    void DamageWeapons(ArkPlayer* pPlayer);
    void ApplySuitIntegrity(ArkPlayer* pPlayer);
    void GiveWrench(ArkPlayer* pPlayer);
    void PlaceMarker(unsigned entityId);
    void RemoveMarker();
    LevelSpawnRecord& CurrentLevelRecord();
    const LevelSpawnRecord* FindLevelRecord() const;
    void LoadSpawns();
    void SaveSpawns();
    std::map<std::string, LevelSpawnRecord> m_levels;
    bool m_spawnsLoaded = false;
    unsigned m_firstDropped = 0;
    std::vector<SpawnPointInfo> m_spawnPoints;
    std::string m_spawnPointsLevel;
    void ScanSpawnPoints();
    const SpawnPointInfo* NearestSpawnPoint(const Vec3& to, float maxDist) const;
};

extern MediumcoreDeath* gMediumcore;
