#include "util/theme.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <SDL2/SDL_image.h>
#include "util/error.hpp"
#include "util/json.hpp"

namespace {
    // Plutonium keeps the view offset private and offers no paging API. An explicit
    // template instantiation may name private members ([temp.spec]), which lets us reach
    // them without modifying the bundled library.
    template <typename Tag, typename Tag::type Member>
    struct PrivateAccess {
        friend typename Tag::type Get(Tag) { return Member; }
    };
    struct MenuFirstVisible {
        using type = s32 pu::ui::elm::Menu::*;
        friend type Get(MenuFirstVisible);
    };
    struct MenuSelected {
        using type = s32 pu::ui::elm::Menu::*;
        friend type Get(MenuSelected);
    };
    struct MenuReloadRenders {
        using type = void (pu::ui::elm::Menu::*)();
        friend type Get(MenuReloadRenders);
    };
    template struct PrivateAccess<MenuFirstVisible, &pu::ui::elm::Menu::fisel>;
    template struct PrivateAccess<MenuSelected, &pu::ui::elm::Menu::isel>;
    template struct PrivateAccess<MenuReloadRenders, &pu::ui::elm::Menu::ReloadItemRenders>;
}

namespace inst::theme {
    s32 GetMenuTop(pu::ui::elm::Menu& menu) {
        return menu.*Get(MenuFirstVisible{});
    }

    void SetMenuPosition(pu::ui::elm::Menu& menu, s32 selected, s32 top) {
        const s32 count = static_cast<s32>(menu.GetItems().size());
        if (count == 0)
            return;
        const s32 pageSize = std::max<s32>(1, menu.GetNumberOfItemsToShow());
        selected = std::clamp<s32>(selected, 0, count - 1);
        // Keep the selection on screen, and never leave empty rows below the last item.
        top = std::clamp<s32>(top, std::max<s32>(0, selected - pageSize + 1), selected);
        top = std::clamp<s32>(top, 0, std::max<s32>(0, count - pageSize));
        menu.SetSelectedIndex(selected);
        s32& first = menu.*Get(MenuFirstVisible{});
        if (first != top) {
            first = top;
            (menu.*Get(MenuReloadRenders{}))();
        }
    }

    bool PageMenu(pu::ui::elm::Menu& menu, int direction) {
        const s32 count = static_cast<s32>(menu.GetItems().size());
        if (count == 0 || direction == 0)
            return false;
        const s32 pageSize = std::max<s32>(1, menu.GetNumberOfItemsToShow());
        const s32 maxTop = std::max<s32>(0, count - pageSize);
        s32& top = menu.*Get(MenuFirstVisible{});
        s32& selected = menu.*Get(MenuSelected{});
        const s32 oldTop = std::clamp<s32>(top, 0, maxTop);
        const s32 oldSelected = std::clamp<s32>(selected, 0, count - 1);
        const s32 newTop = std::clamp<s32>(oldTop + (direction > 0 ? pageSize : -pageSize), 0, maxTop);
        s32 newSelected = oldSelected + (newTop - oldTop);
        if (newTop == oldTop)
            newSelected = direction > 0 ? count - 1 : 0; // Last page: go to the edge item.
        newSelected = std::clamp<s32>(newSelected, 0, count - 1);
        if (newTop == oldTop && newSelected == oldSelected)
            return false;
        // SetSelectedIndex resets the focus animation; then pin the view where we want it.
        menu.SetSelectedIndex(newSelected);
        if (top != newTop) {
            top = newTop;
            (menu.*Get(MenuReloadRenders{}))();
        }
        return true;
    }

    namespace {
        constexpr const char* kImagePrefix = "romfs:/images/";
        std::unordered_map<std::string, pu::ui::Color> g_colors;
        std::unordered_map<std::string, std::string> g_images;
        std::string g_name = "Default";
        bool g_noBackgroundImage = false;

        bool ValidManifestPath(const std::string& file) {
            if (file.empty() || file.find(':') != std::string::npos || file.find('\\') != std::string::npos) return false;
            const std::filesystem::path path(file);
            if (path.is_absolute() || path.extension() != ".json") return false;
            for (const auto& part : path) if (part == "..") return false;
            return true;
        }

        bool ReadManifest(const std::filesystem::path& manifest, nlohmann::json& root) {
            std::error_code error;
            const auto size = std::filesystem::file_size(manifest, error);
            if (error || size == 0 || size > 256 * 1024) return false;
            try {
                std::ifstream input(manifest);
                root = nlohmann::json::parse(input);
                return root.is_object() && (!root.contains("version") || root["version"] == 1) &&
                    (!root.contains("enabled") || (root["enabled"].is_boolean() && root["enabled"].get<bool>()));
            } catch (...) { return false; }
        }

        std::string ManifestName(const nlohmann::json& root, const std::string& fallback) {
            if (root.contains("name") && root["name"].is_string()) {
                const auto name = root["name"].get<std::string>();
                if (!name.empty() && name.size() <= 80) return name;
            }
            return fallback;
        }

        bool SameColor(pu::ui::Color left, pu::ui::Color right) {
            return left.R == right.R && left.G == right.G && left.B == right.B && left.A == right.A;
        }

        bool ParseColor(const nlohmann::json& value, pu::ui::Color& result) {
            if (!value.is_string()) return false;
            auto text = value.get<std::string>();
            if ((text.size() != 7 && text.size() != 9) || text.front() != '#') return false;
            if (!std::all_of(text.begin() + 1, text.end(), [](unsigned char ch) { return std::isxdigit(ch); })) return false;
            if (text.size() == 7) text += "FF";
            result = pu::ui::Color::FromHex(text);
            return true;
        }

        std::string ValidImage(const std::filesystem::path& directory, const std::string& relative) {
            const std::filesystem::path path(relative);
            if (relative.empty() || relative.find(':') != std::string::npos || relative.find('\\') != std::string::npos || path.is_absolute()) return {};
            for (const auto& part : path) if (part == "..") return {};
            const auto full = directory / path;
            std::error_code error;
            const auto size = std::filesystem::file_size(full, error);
            if (error || size == 0 || size > 16ULL * 1024 * 1024) return {};
            // Decode before accepting the override so corrupt assets fall back safely.
            SDL_Surface* image = IMG_Load(full.string().c_str());
            if (!image) return {};
            const bool valid = image->w > 0 && image->h > 0 && image->w <= 4096 && image->h <= 4096;
            SDL_FreeSurface(image);
            return valid ? full.string() : std::string();
        }

        pu::ui::Color Tint(const std::string& role, pu::ui::Color fallback) {
            const auto found = g_colors.find(role);
            if (found == g_colors.end()) return fallback;
            auto result = found->second;
            result.A = static_cast<u8>(static_cast<unsigned>(result.A) * fallback.A / 255);
            return result;
        }
    }

    std::vector<Choice> Discover(const std::string& appDir) {
        std::vector<Choice> choices;
        const auto directory = std::filesystem::path(appDir) / "theme";
        const auto add = [&](const std::filesystem::path& manifest, const std::string& file) {
            nlohmann::json root;
            if (ValidManifestPath(file) && ReadManifest(manifest, root)) {
                choices.push_back({file, ManifestName(root, std::filesystem::path(file).stem().string())});
            }
        };
        std::error_code error;
        std::filesystem::directory_iterator it(directory, error), end;
        while (!error && it != end) {
            const auto entry = *it;
            const auto filename = entry.path().filename().string();
            std::error_code typeError;
            if (entry.is_regular_file(typeError) && entry.path().extension() == ".json") {
                add(entry.path(), filename);
            } else if (!typeError && entry.is_directory(typeError)) {
                add(entry.path() / "theme.json", filename + "/theme.json");
            }
            it.increment(error);
        }
        std::sort(choices.begin(), choices.end(), [](const Choice& left, const Choice& right) {
            return left.name == right.name ? left.file < right.file : left.name < right.name;
        });
        return choices;
    }

    void Load(const std::string& appDir, const std::string& selectedFile) {
        g_colors.clear();
        g_images.clear();
        g_name = "Default";
        g_noBackgroundImage = false;
        if (!ValidManifestPath(selectedFile)) return;
        const auto manifest = std::filesystem::path(appDir) / "theme" / selectedFile;
        const auto directory = manifest.parent_path();
        try {
            nlohmann::json root;
            if (!ReadManifest(manifest, root)) return;
            if (root.contains("colors") && root["colors"].is_object()) {
                for (const auto& entry : root["colors"].items()) {
                    pu::ui::Color color;
                    if (ParseColor(entry.value(), color)) g_colors[entry.key()] = color;
                    else LOG_DEBUG("Theme: ignored invalid color %s\n", entry.key().c_str());
                }
            }
            if (root.contains("images") && root["images"].is_object()) {
                for (const auto& entry : root["images"].items()) {
                    if (!entry.value().is_string()) continue;
                    const auto relative = entry.value().get<std::string>();
                    auto key = entry.key();
                    if (key == "background") key = "background.jpg";
                    if (key == "logo") key = "logo.png";
                    if (key == "background.jpg" && relative.empty()) {
                        g_noBackgroundImage = true;
                        continue;
                    }
                    const auto image = ValidImage(directory, relative);
                    if (!image.empty()) g_images[key] = image;
                    else LOG_DEBUG("Theme: ignored invalid image %s\n", relative.c_str());
                }
            }
            g_name = ManifestName(root, manifest.stem().string());
        } catch (...) {
            g_colors.clear();
            g_images.clear();
            g_noBackgroundImage = false;
            g_name = "Default";
            LOG_DEBUG("Theme: could not load theme.json, using defaults\n");
        }
    }

    std::string Name() { return g_name; }
    bool HasColor(const std::string& role) { return g_colors.find(role) != g_colors.end(); }

    pu::ui::Color Color(const std::string& role, pu::ui::Color fallback) {
        const auto found = g_colors.find(role);
        if (found != g_colors.end()) return found->second;
        if (role == "highlight" || role == "scrollbar") return Tint("accent", fallback);
        return fallback;
    }

    pu::ui::Color Palette(const std::string& fallbackHex) {
        const auto fallback = pu::ui::Color::FromHex(fallbackHex);
        const auto rgb = fallbackHex.substr(0, 7);
        if (rgb == "#FFFFFF" && fallback.A == 255) return Color("text", fallback);
        if (rgb == "#A8A8B8") return Tint("muted_text", fallback);
        if (rgb == "#FF4D4D" || rgb == "#FF7777" || rgb == "#FF9999") return Tint("accent", fallback);
        if (rgb == "#170909" || rgb == "#17171C" || rgb == "#222228" || rgb == "#1A1A1A" ||
            (rgb == "#000000" && (fallback.A == 204 || fallback.A == 238))) return Tint("panel", fallback);
        if (rgb == "#222222") return Color("progress_track", fallback);
        return fallback;
    }

    std::string Asset(const std::string& defaultPath) {
        if (defaultPath.rfind(kImagePrefix, 0) != 0) return defaultPath;
        const auto found = g_images.find(defaultPath.substr(std::char_traits<char>::length(kImagePrefix)));
        return found == g_images.end() ? defaultPath : found->second;
    }

    void ApplyBackground(pu::ui::Layout& layout, bool oledMode, const std::string& appDir) {
        layout.SetBackgroundColor(Color("background", pu::ui::Color::FromHex(oledMode ? "#000000FF" : "#670000FF")));
        const auto background = g_images.find("background.jpg");
        if (background != g_images.end() && !g_noBackgroundImage) {
            layout.SetBackgroundImage(background->second);
        } else if (!HasColor("background") && !g_noBackgroundImage && !oledMode) {
            std::error_code error;
            const auto legacy = appDir + "/background.png";
            layout.SetBackgroundImage(std::filesystem::is_regular_file(legacy, error) ? legacy : "romfs:/images/background.jpg");
        }
    }

    Menu::Menu(s32 x, s32 y, s32 width, pu::ui::Color optionColor, s32 itemSize, s32 itemsToShow, s32 fontSize)
        : pu::ui::elm::Menu(x, y, width, optionColor, itemSize, itemsToShow, fontSize) {
        SetOnFocusColor(inst::theme::Color("highlight", GetOnFocusColor()));
        SetScrollbarColor(inst::theme::Color("scrollbar", GetScrollbarColor()));
    }

    void Menu::OnRender(pu::ui::render::Renderer::Ref& drawer, s32 x, s32 y) {
        if (HasColor("selected_text")) {
            const auto index = GetSelectedIndex();
            if (index >= 0 && index < static_cast<s32>(GetItems().size())) {
                const auto item = GetItems()[index];
                const auto previous = focusedItem.lock();
                const auto selectedColor = Color("selected_text", item->GetColor());
                if (previous != item || !SameColor(item->GetColor(), selectedColor)) {
                    if (previous && previous != item) previous->SetColor(originalColor);
                    originalColor = item->GetColor();
                    focusedItem = item;
                    item->SetColor(selectedColor);
                    // Rebuild text textures once per focus change, not on every frame.
                    // SetSelectedIndex also moves the view; keep it where scrolling put it.
                    s32& top = this->*Get(MenuFirstVisible{});
                    const s32 keepTop = top;
                    pu::ui::elm::Menu::SetSelectedIndex(index);
                    if (top != keepTop) {
                        top = keepTop;
                        (this->*Get(MenuReloadRenders{}))();
                    }
                }
            }
        }
        pu::ui::elm::Menu::OnRender(drawer, x, y);
    }
}
