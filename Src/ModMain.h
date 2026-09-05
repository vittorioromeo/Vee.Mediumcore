#pragma once
// Mediumcore Death for Prey (2017) - Chairloader mod entry point.
// The death/respawn logic lives in Mediumcore.cpp; this class only wires it into the Chairloader mod
// lifecycle (hooks, cvars, per-frame update, HUD text, F1 settings window, input).
#include <Chairloader/ModSDK/ChairloaderModBase.h>
#include <Prey/CryInput/IInput.h>

class ModMain final : public ChairloaderModBase, public IInputEventListener
{
public:
    using BaseClass = ChairloaderModBase;

    // Mod Initialization
    virtual void FillModInfo(ModDllInfoEx& info) override;
    virtual void InitHooks() override;
    virtual void InitSystem(const ModInitInfo& initInfo, ModDllInfo& dllInfo) override;
    virtual void InitGame(bool isHotReloading) override;

    // Mod Shutdown
    virtual void ShutdownGame(bool isHotUnloading) override;
    virtual void ShutdownSystem(bool isHotUnloading) override;

    // GUI
    virtual void Draw() override;

    // Main Update Loop
    virtual void UpdateBeforeSystem(unsigned updateFlags) override {}
    virtual void UpdateBeforePhysics(unsigned updateFlags) override {}
    virtual void MainUpdate(unsigned updateFlags) override;
    virtual void LateUpdate(unsigned updateFlags) override;

    // IInputEventListener
    virtual bool OnInputEvent(const SInputEvent& event) override;
    virtual bool OnInputEventUI(const SInputEvent& event) override { return false; }
    virtual int GetPriority() override { return 100; }

private:
    int m_showWindow = 1;               //!< persisted (mc_show_window)
    bool m_inputListenerRegistered = false;

    void DrawWindow();
};

extern ModMain* gMod;
