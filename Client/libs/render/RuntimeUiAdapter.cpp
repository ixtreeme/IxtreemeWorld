#include "RuntimeUiAdapter.h"

namespace
{
class NullRuntimeUiAdapter final : public RuntimeUiAdapter
{
public:
    explicit NullRuntimeUiAdapter(RmlUiLayer& rmlUi) : m_rmlUi(rmlUi) {}

    void BindRuntime(RuntimeSession&) override {}
    void SetQuitCallback(std::function<void()>) override {}
    void HideAll() override { m_rmlUi.HideAll(); }
    bool OnInput(const InputEvent&) override { return false; }
    bool IsSettingsVisible() const override { return false; }
    void HideSettings() override {}
    void ToggleInGameMenu() override {}
    void ToggleInventory() override {}
    void UpdateHud(const RmlHudData&) override {}

private:
    RmlUiLayer& m_rmlUi;
};
}

std::unique_ptr<RuntimeUiAdapter> CreateNullRuntimeUiAdapter(RmlUiLayer& rmlUi)
{
    return std::make_unique<NullRuntimeUiAdapter>(rmlUi);
}
