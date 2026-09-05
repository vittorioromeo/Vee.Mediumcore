// Mediumcore Death for Prey (2017) - Chairloader mod entry point.
#include "ModMain.h"
#include "Mediumcore.h"
#include <Prey/CrySystem/IConsole.h>

ModMain* gMod = nullptr;

//---------------------------------------------------------------------------------
// Mod Initialization
//---------------------------------------------------------------------------------
void ModMain::FillModInfo(ModDllInfoEx& info)
{
    info.modName = "Vee.Mediumcore";
    info.logTag = "Mediumcore";
    info.supportsHotReload = true;
}

void ModMain::InitHooks()
{
    if (!gMediumcore)
        gMediumcore = new MediumcoreDeath();
    gMediumcore->InitHooks();
}

void ModMain::InitSystem(const ModInitInfo& initInfo, ModDllInfo& dllInfo)
{
    BaseClass::InitSystem(initInfo, dllInfo);
    if (!gMediumcore)
        gMediumcore = new MediumcoreDeath();
    gMediumcore->RegisterCVars();
    REGISTER_CVAR2("mc_show_window", &m_showWindow, m_showWindow, VF_DUMPTOCHAIR, "Mediumcore: show the settings window in the Chairloader GUI (0/1)");
    CryLog("Mediumcore: initialized (health hook {})", gMediumcore->HookInstalled() ? "installed" : "NOT installed");
}

void ModMain::InitGame(bool isHotReloading)
{
    BaseClass::InitGame(isHotReloading);
    if (gEnv && gEnv->pInput && !m_inputListenerRegistered)
    {
        gEnv->pInput->AddEventListener(this);
        m_inputListenerRegistered = true;
    }
}

//---------------------------------------------------------------------------------
// Mod Shutdown
//---------------------------------------------------------------------------------
void ModMain::ShutdownGame(bool isHotUnloading)
{
    if (gEnv && gEnv->pInput && m_inputListenerRegistered)
    {
        gEnv->pInput->RemoveEventListener(this);
        m_inputListenerRegistered = false;
    }
    if (gMediumcore)
        gMediumcore->OnShutdownGame();
    BaseClass::ShutdownGame(isHotUnloading);
}

void ModMain::ShutdownSystem(bool isHotUnloading)
{
    BaseClass::ShutdownSystem(isHotUnloading);
}

//---------------------------------------------------------------------------------
// Main Update Loop
//---------------------------------------------------------------------------------
void ModMain::MainUpdate(unsigned updateFlags)
{
    const float dt = (gEnv && gEnv->pTimer) ? gEnv->pTimer->GetFrameTime() : 0.0f;
    if (gMediumcore)
        gMediumcore->Update(dt);
}

void ModMain::LateUpdate(unsigned updateFlags)
{
    if (gMediumcore)
        gMediumcore->DrawHud();
}

bool ModMain::OnInputEvent(const SInputEvent& event)
{
    if (event.deviceType != eIDT_Keyboard && event.deviceType != eIDT_Mouse && event.deviceType != eIDT_Gamepad)
        return false;
    if (event.state != eIS_Pressed && event.state != eIS_Released)
        return false;
    return gMediumcore && gMediumcore->OnInputEvent(event);
}

//---------------------------------------------------------------------------------
// GUI
//---------------------------------------------------------------------------------
void ModMain::Draw()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Mediumcore"))
        {
            bool show = m_showWindow != 0;
            if (ImGui::MenuItem("Settings window", nullptr, &show))
                m_showWindow = show ? 1 : 0;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (m_showWindow)
        DrawWindow();
}

void ModMain::DrawWindow()
{
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mediumcore Death", &open))
    {
        ImGui::TextDisabled("Experimental. Dying drops your inventory where you fell and respawns you - no death menu.");
        if (gMediumcore)
            gMediumcore->DrawSettings();
    }
    ImGui::End();

    if (!open)
        m_showWindow = 0;
}

//---------------------------------------------------------------------------------
// Exported Functions
//---------------------------------------------------------------------------------
extern "C" DLL_EXPORT IChairloaderMod* ClMod_Initialize()
{
    CRY_ASSERT(!gMod);
    gMod = new ModMain();
    return gMod;
}

extern "C" DLL_EXPORT void ClMod_Shutdown()
{
    CRY_ASSERT(gMod);
    delete gMod;
    gMod = nullptr;
    delete gMediumcore;
    gMediumcore = nullptr;
}

// Validate that declarations haven't changed
static_assert(std::is_same_v<decltype(ClMod_Initialize), IChairloaderMod::ProcInitialize>);
static_assert(std::is_same_v<decltype(ClMod_Shutdown), IChairloaderMod::ProcShutdown>);
