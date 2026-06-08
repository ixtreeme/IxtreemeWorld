#pragma once

#include "RmlUiLayer.h"
#include "RuntimeSession.h"

#include <functional>
#include <memory>

class RuntimeUiAdapter
{
public:
    virtual ~RuntimeUiAdapter() = default;

    virtual void InstallSceneRouting(SceneManager& scenes) = 0;
    virtual void BindRuntime(RuntimeSession& runtime) = 0;
    virtual void SetQuitCallback(std::function<void()> callback) = 0;
    virtual void HideAll() = 0;
    virtual bool OnInput(const InputEvent& event) = 0;
    virtual bool IsSettingsVisible() const = 0;
    virtual void HideSettings() = 0;
    virtual void ToggleInGameMenu() = 0;
    virtual void ToggleInventory() = 0;
    virtual void UpdateHud(const RmlHudData& data) = 0;
};

std::unique_ptr<RuntimeUiAdapter> CreateNullRuntimeUiAdapter(RmlUiLayer& rmlUi);
std::unique_ptr<RuntimeUiAdapter> CreateAurigaRuntimeUiAdapter(RmlUiLayer& rmlUi,
                                                               client::asset::IAssetReader& assets,
                                                               std::function<bool()> isRuntimeFlowActive);
