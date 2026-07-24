// graphics_settings mod - overlay (F6) exposing two Settings-screen values:
// the screen-stretch viewport (preset buttons + custom width/height inputs)
// and the Original/Enhanced graphics style toggle.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/mod_storage.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr uint32_t kOffsetXOffset = 0;
constexpr uint32_t kOffsetYOffset = 4;
constexpr uint32_t kWidthOffset = 8;
constexpr uint32_t kHeightOffset = 12;
constexpr uint32_t kSpanSize = 16;

// Presets (baseline 720p)
constexpr uint32_t kPsxDefaultWidth = 1052;
constexpr uint32_t kPsxDefaultHeight = 720;
constexpr uint32_t kPsxBigWidth = 1097;
constexpr uint32_t kPsxBigHeight = 765;

constexpr uint32_t k1610DefaultWidth = 1052;
constexpr uint32_t k1610DefaultHeight = 667;
constexpr uint32_t k1610BigWidth = 1136;
constexpr uint32_t k1610BigHeight = 720;
constexpr uint32_t k1610HugeWidth = 1226;
constexpr uint32_t k1610HugeHeight = 765;
constexpr uint32_t k1610ExtremeWidth = 1282;
constexpr uint32_t k1610ExtremeHeight = 793;

constexpr uint32_t kOtherStretchedWidth = 1280;
constexpr uint32_t kOtherStretchedHeight = 766;

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(guest_address);
    return rex::memory::load_and_swap<uint32_t>(host);
}

void WriteGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address, uint32_t value) {
    uint8_t* host = memory->TranslateVirtual<uint8_t*>(guest_address);
    rex::memory::store_and_swap<uint32_t>(host, value);
}

void WriteGuestU8(rex::memory::Memory* memory, uint32_t guest_address, uint8_t value) {
    uint8_t* host = memory->TranslateVirtual<uint8_t*>(guest_address);
    *host = value;
}

bool TryReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address, uint32_t* out) {
    auto* heap = memory->LookupHeap(guest_address);
    if (!heap || heap->QueryRangeAccess(guest_address, guest_address + 3) ==
                 rex::memory::PageAccess::kNoAccess) {
        return false;
    }
    *out = ReadGuestU32BE(memory, guest_address);
    return true;
}

bool IsGuestRangeReadable(rex::memory::Memory* memory, uint32_t guest_address, uint32_t size) {
    auto* heap = memory->LookupHeap(guest_address);
    return heap && heap->QueryRangeAccess(guest_address, guest_address + size - 1) !=
                   rex::memory::PageAccess::kNoAccess;
}

std::filesystem::path ConfigFilePath(rex::Runtime* runtime) {
    return runtime->user_data_root() / "mods" / "graphics_settings.cfg";
}

uint32_t GetConfiguredVideoModeWidth() {
    int32_t configured_width = REXCVAR_QUERY(int32_t, video_mode_width);
    if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
        if (rex::cvar::HasNonDefaultValue("window_width") &&
            REXCVAR_QUERY(int32_t, window_width) > 0) {
            configured_width = REXCVAR_QUERY(int32_t, window_width);
        } else {
            int32_t preset_w = 0, preset_h = 0;
            if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_w, preset_h)) {
                configured_width = preset_w;
            }
        }
    }
    return static_cast<uint32_t>(std::clamp(configured_width, 640, 0x0FFF));
}

uint32_t GetConfiguredVideoModeHeight() {
    int32_t configured_height = REXCVAR_QUERY(int32_t, video_mode_height);
    if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
        if (rex::cvar::HasNonDefaultValue("window_height") &&
            REXCVAR_QUERY(int32_t, window_height) > 0) {
            configured_height = REXCVAR_QUERY(int32_t, window_height);
        } else {
            int32_t preset_w = 0, preset_h = 0;
            if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_w, preset_h)) {
                configured_height = preset_h;
            }
        }
    }
    return static_cast<uint32_t>(std::clamp(configured_height, 480, 0x0FFF));
}

uint32_t ScalePresetWidth(uint32_t base_width) {
    double ratio = static_cast<double>(GetConfiguredVideoModeWidth()) / 1280.0;
    return static_cast<uint32_t>(std::lround(base_width * ratio));
}

uint32_t ScalePresetHeight(uint32_t base_height) {
    double ratio = static_cast<double>(GetConfiguredVideoModeHeight()) / 720.0;
    return static_cast<uint32_t>(std::lround(base_height * ratio));
}

class GraphicsSettingsDialog : public rex::ui::ImGuiDialog {
public:
    GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
        : ImGuiDialog(drawer),
          runtime_(runtime),
          storage_(runtime ? ConfigFilePath(runtime) : std::filesystem::path()) {

        storage_.Load();

        rex::ui::RegisterBind(
            "bind_graphics_settings",
            "F6",
            "Toggle graphics settings overlay",
            [this] { visible_ = !visible_; });
    }

    ~GraphicsSettingsDialog() override {
        rex::ui::UnregisterBind("bind_graphics_settings");
    }

    void ResolveAddress() {
        if (runtime_ && runtime_->mod_registry()) {
            if (auto addr = runtime_->mod_registry()->FindAddress("graphics.stretch_rect")) {
                addr_ = *addr;
                addr_resolved_ = true;
            }
            if (auto addr = runtime_->mod_registry()->FindAddress("graphics.style")) {
                style_addr_ = *addr;
                style_addr_resolved_ = true;
            }
            if (auto addr = runtime_->mod_registry()->FindAddress("app.singleton_ptr")) {
                app_singleton_addr_ = *addr;
                app_singleton_resolved_ = true;
            }
        }

        auto width_ratio = storage_.GetDouble("width_ratio");
        auto height_ratio = storage_.GetDouble("height_ratio");
        if (width_ratio && height_ratio && *width_ratio > 0.0 && *height_ratio > 0.0) {
            custom_width_ = static_cast<int>(std::lround(*width_ratio * GetConfiguredVideoModeWidth()));
            custom_height_ = static_cast<int>(std::lround(*height_ratio * GetConfiguredVideoModeHeight()));
            custom_seeded_ = true;
        }

        if (auto style = storage_.GetInt("graphics_style")) {
            pending_style_ = static_cast<uint8_t>(*style != 0 ? 1 : 0);
            style_restore_pending_ = true;
        }
    }

    void TryRestoreStyle() {
        if (!style_restore_pending_ || !app_singleton_resolved_ || !runtime_) {
            return;
        }

        auto* memory = runtime_->memory();
        if (!memory) return;

        uint32_t singleton = 0, settings_base = 0, data_ptr = 0, base_index = 0;

        if (!TryReadGuestU32BE(memory, app_singleton_addr_, &singleton) ||
            !TryReadGuestU32BE(memory, singleton + 2296, &settings_base) ||
            !TryReadGuestU32BE(memory, settings_base + 4, &data_ptr) ||
            !TryReadGuestU32BE(memory, data_ptr + 4348, &base_index)) {
            return;
        }

        uint32_t entry_addr = 180 * (base_index + 21) + data_ptr + 28;

        if (!IsGuestRangeReadable(memory, entry_addr + 2, 1) ||
            !IsGuestRangeReadable(memory, data_ptr + 4548, 1)) {
            return;
        }

        SetGraphicsStyle(pending_style_, false);
    }

    void ApplyRestoredStretch() {
        if (custom_seeded_) {
            SetOverride(
                static_cast<uint32_t>(std::max(custom_width_, 0)),
                static_cast<uint32_t>(std::max(custom_height_, 0))
            );
        }
    }

    void TryApplyStretchIfReady() {
        if (!addr_resolved_ || !runtime_) return;

        auto* memory = runtime_->memory();
        if (!memory) return;

        auto* heap = memory->LookupHeap(addr_);
        bool readable = heap &&
            heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) != rex::memory::PageAccess::kNoAccess;

        if (!readable) return;

        ApplyRestoredStretch();
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        TryApplyStretchIfReady();

        if (!visible_) return;

        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Graphics Settings", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }

        auto* memory = runtime_ ? runtime_->memory() : nullptr;
        if (!memory) {
            ImGui::End();
            return;
        }

        if (style_addr_resolved_) {
            auto* heap = memory->LookupHeap(style_addr_);
            bool readable = heap && heap->QueryRangeAccess(style_addr_, style_addr_ + 3) !=
                                      rex::memory::PageAccess::kNoAccess;
            if (readable) {
                uint32_t style = ReadGuestU32BE(memory, style_addr_);
                bool enhanced = style != 0;

                ImGui::TextUnformatted("Graphics style:");
                if (ImGui::RadioButton("Original", !enhanced)) {
                    style_restore_pending_ = false;
                    SetGraphicsStyle(0);
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Enhanced", enhanced)) {
                    style_restore_pending_ = false;
                    SetGraphicsStyle(1);
                }
                ImGui::Separator();
            }
        }

        auto* heap = memory->LookupHeap(addr_);
        bool readable = heap && heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;

        if (!readable) {
            ImGui::TextDisabled("Start or load a game to edit the screen stretch.");
            ImGui::End();
            return;
        }

        uint32_t width = ReadGuestU32BE(memory, addr_ + kWidthOffset);
        uint32_t height = ReadGuestU32BE(memory, addr_ + kHeightOffset);

        ImGui::TextUnformatted("PSX:");
        if (ImGui::Button("Default##psx")) {
            SetOverride(ScalePresetWidth(kPsxDefaultWidth), ScalePresetHeight(kPsxDefaultHeight));
        }
        ImGui::SameLine();
        if (ImGui::Button("Big##psx")) {
            SetOverride(ScalePresetWidth(kPsxBigWidth), ScalePresetHeight(kPsxBigHeight));
        }

        ImGui::TextUnformatted("16:10:");
        if (ImGui::Button("Default##1610")) {
            SetOverride(ScalePresetWidth(k1610DefaultWidth), ScalePresetHeight(k1610DefaultHeight));
        }
        ImGui::SameLine();
        if (ImGui::Button("Big##1610")) {
            SetOverride(ScalePresetWidth(k1610BigWidth), ScalePresetHeight(k1610BigHeight));
        }
        ImGui::SameLine();
        if (ImGui::Button("Huge##1610")) {
            SetOverride(ScalePresetWidth(k1610HugeWidth), ScalePresetHeight(k1610HugeHeight));
        }

        ImGui::TextUnformatted("Other:");
        if (ImGui::Button("Extreme##1610")) {
            SetOverride(ScalePresetWidth(k1610ExtremeWidth), ScalePresetHeight(k1610ExtremeHeight));
        }
        ImGui::SameLine();
        if (ImGui::Button("Stretched")) {
            SetOverride(ScalePresetWidth(kOtherStretchedWidth), ScalePresetHeight(kOtherStretchedHeight));
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Custom:");

        if (!custom_seeded_) {
            custom_width_ = static_cast<int>(width);
            custom_height_ = static_cast<int>(height);
            custom_seeded_ = true;
        }

        float label_column = ImGui::CalcTextSize("Height").x + ImGui::GetStyle().ItemSpacing.x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Width");
        ImGui::SameLine(label_column);
        ImGui::SetNextItemWidth(100);
        bool width_changed = ImGui::InputInt("##width", &custom_width_);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Height");
        ImGui::SameLine(label_column);
        ImGui::SetNextItemWidth(100);
        bool height_changed = ImGui::InputInt("##height", &custom_height_);

        if (width_changed || height_changed) {
            SetOverride(static_cast<uint32_t>(std::max(custom_width_, 0)),
                        static_cast<uint32_t>(std::max(custom_height_, 0)));
        }

        ImGui::End();
    }

private:
    bool SetGraphicsStyle(uint8_t style_value, bool persist = true) {
        if (!app_singleton_resolved_ || !runtime_) return false;

        auto* memory = runtime_->memory();
        if (!memory) return false;

        uint32_t singleton = ReadGuestU32BE(memory, app_singleton_addr_);
        uint32_t settings_base = ReadGuestU32BE(memory, singleton + 2296);
        uint32_t data_ptr = ReadGuestU32BE(memory, settings_base + 4);
        uint32_t base_index = ReadGuestU32BE(memory, data_ptr + 4348);

        uint32_t entry_addr = 180 * (base_index + 21) + data_ptr + 28;

        WriteGuestU8(memory, entry_addr + 2, style_value);
        WriteGuestU8(memory, data_ptr + 4548, style_value);

        if (persist) {
            storage_.SetInt("graphics_style", style_value != 0 ? 1 : 0);
            storage_.Save();
        }

        return true;
    }

    void SetOverride(uint32_t width, uint32_t height) {
        Apply(runtime_->memory(), width, height);

        custom_width_ = static_cast<int>(width);
        custom_height_ = static_cast<int>(height);
        custom_seeded_ = true;

        uint32_t configured_width = GetConfiguredVideoModeWidth();
        uint32_t configured_height = GetConfiguredVideoModeHeight();

        if (configured_width != 0 && configured_height != 0) {
            storage_.SetDouble("width_ratio", static_cast<double>(width) / configured_width);
            storage_.SetDouble("height_ratio", static_cast<double>(height) / configured_height);
            storage_.Save();
        }
    }

    void Apply(rex::memory::Memory* memory, uint32_t width, uint32_t height) {
        uint32_t offset_x = GetConfiguredVideoModeWidth() - width;
        uint32_t offset_y = GetConfiguredVideoModeHeight() - height;

        WriteGuestU32BE(memory, addr_ + kOffsetXOffset, offset_x);
        WriteGuestU32BE(memory, addr_ + kOffsetYOffset, offset_y);
        WriteGuestU32BE(memory, addr_ + kWidthOffset, width);
        WriteGuestU32BE(memory, addr_ + kHeightOffset, height);
    }

    rex::Runtime* runtime_ = nullptr;
    bool visible_ = false;

    bool addr_resolved_ = false;
    uint32_t addr_ = 0;

    bool style_addr_resolved_ = false;
    uint32_t style_addr_ = 0;

    bool app_singleton_resolved_ = false;
    uint32_t app_singleton_addr_ = 0;

    bool custom_seeded_ = false;
    int custom_width_ = 0;
    int custom_height_ = 0;

    bool aspect_locked_ = false;
    double locked_aspect_ratio_ = 0.0;

    rex::system::ModStorage storage_;

    bool style_restore_pending_ = false;
    uint8_t pending_style_ = 0;
};

}  // namespace

class GraphicsSettingsPlugin : public rex::system::IModPlugin {
public:
    GraphicsSettingsPlugin(const rex::system::ModHostContext& ctx)
        : ctx_(ctx) {}

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
        dialog_ = new GraphicsSettingsDialog(drawer, ctx_.runtime);
        drawer->AddDialog(dialog_);
    }

    void OnModuleLaunched() override {
        if (dialog_) {
            dialog_->ResolveAddress();
            dialog_->TryRestoreStyle();
            dialog_->TryApplyStretchIfReady();
        }
    }

private:
    rex::system::ModHostContext ctx_;
    GraphicsSettingsDialog* dialog_ = nullptr;
};

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version() {
    return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
    if (!ctx || abi_version != rex::system::kModPluginAbiVersion) {
        return nullptr;
    }
    return new GraphicsSettingsPlugin(*ctx);
}