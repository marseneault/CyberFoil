#pragma once

#include <string>
#include <vector>
#include <pu/ui/ui_Layout.hpp>
#include <pu/ui/elm/elm_Menu.hpp>

namespace inst::theme {
    // Load on the UI thread before creating or recreating themed layouts.
    struct Choice {
        std::string file; // Relative to appDir/theme; empty selects the built-in appearance.
        std::string name;
    };
    std::vector<Choice> Discover(const std::string& appDir);
    void Load(const std::string& appDir, const std::string& selectedFile = "theme.json");
    std::string Name();
    bool HasColor(const std::string& role);
    pu::ui::Color Color(const std::string& role, pu::ui::Color fallback);
    pu::ui::Color Palette(const std::string& fallbackHex);
    std::string Asset(const std::string& defaultPath);
    void ApplyBackground(pu::ui::Layout& layout, bool oledMode, const std::string& appDir);
    // Scroll a menu by one visible page (direction > 0 = down), keeping the cursor on the
    // same screen row, with a single re-render. Returns false when already at the edge.
    bool PageMenu(pu::ui::elm::Menu& menu, int direction);
    // Index of the first visible row.
    s32 GetMenuTop(pu::ui::elm::Menu& menu);
    // Select an item and show the view starting at `top` (both clamped).
    void SetMenuPosition(pu::ui::elm::Menu& menu, s32 selected, s32 top);

    // Apply focus text colors without modifying the bundled Plutonium library.
    class Menu : public pu::ui::elm::Menu {
    public:
        Menu(s32 x, s32 y, s32 width, pu::ui::Color optionColor, s32 itemSize, s32 itemsToShow, s32 fontSize = 25);
        PU_SMART_CTOR(Menu)
        void OnRender(pu::ui::render::Renderer::Ref& drawer, s32 x, s32 y) override;
    private:
        std::weak_ptr<pu::ui::elm::MenuItem> focusedItem;
        pu::ui::Color originalColor;
    };
}
