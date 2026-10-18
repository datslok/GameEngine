#include "engine/debug_ui.h"
#include "render/gpu/quality_preset.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl3.h"
#include "imgui/imgui_impl_sdlgpu3.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
    // A slider's number format followed by the field's unit, with any % in the unit doubled so ImGui does not read it
    // as part of the format.
    std::string numberFormat(const char* number, const char* unit) {
        std::string format = number;

        if (unit[0] != '\0') {
            format += ' ';

            for (const char* character = unit; *character != '\0'; ++character) {
                format += *character;

                if (*character == '%') {
                    format += '%';
                }
            }
        }

        return format;
    }

    // Start-up fields are greyed out for as long as this lives, with a tooltip saying why.
    class StartupFieldScope {
    public:
        explicit StartupFieldScope(FieldApplies applies): active(applies == FieldApplies::AtStartup) {
            if (active) {
                ImGui::BeginDisabled();
            }
        }

        ~StartupFieldScope() {
            if (!active) {
                return;
            }

            ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Read once at start-up: change it in settings.cfg and restart");
            }
        }

        StartupFieldScope(const StartupFieldScope&) = delete;
        StartupFieldScope& operator=(const StartupFieldScope&) = delete;

    private:
        bool active;
    };

    /*
    * Draws each field as a control. Groups at the top level are collapsing headers, deeper ones tree nodes, each with
    * its own ID scope, so two fields may share a label in different groups (bloom's and a particle's Radius). Typing
    * a value (Ctrl+click on a slider) can go past the slider's range but never past the hard limits.
    */
    class ImGuiSettingsVisitor final : public SettingsVisitor {
    protected:
        void visitFloat(const char* label, float& value, const FloatLimits& limits) override {
            const StartupFieldScope scope{limits.applies};
            const std::string format = numberFormat("%.4g", limits.unit);

            if (ImGui::SliderFloat(label, &value, limits.lowestSliderValue(), limits.highestSliderValue(), format.c_str())) {
                value = std::clamp(value, limits.min, limits.max);
            }
        }

        void visitInt(const char* label, int& value, const IntLimits& limits) override {
            const StartupFieldScope scope{limits.applies};
            const std::string format = numberFormat("%d", limits.unit);

            if (ImGui::SliderInt(label, &value, limits.lowestSliderValue(), limits.highestSliderValue(), format.c_str())) {
                value = std::clamp(value, limits.min, limits.max);
            }
        }

        void visitBool(const char* label, bool& value, FieldApplies applies) override {
            const StartupFieldScope scope{applies};
            ImGui::Checkbox(label, &value);
        }

        void visitChoice(const char* label, int& index, const char* const* names, int count, FieldApplies applies) override {
            const StartupFieldScope scope{applies};
            ImGui::Combo(label, &index, names, count);
        }

        void visitGroup(const char* label, const std::function<void()>& contents) override {
            const bool topLevel = depth == 0;
            const bool shown = topLevel
                ? ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)
                : ImGui::TreeNode(label);

            if (!shown) {
                return;
            }

            ImGui::PushID(label);
            ++depth;
            contents();
            --depth;
            ImGui::PopID();

            if (!topLevel) {
                ImGui::TreePop();
            }
        }

    private:
        int depth = 0;
    };

    // The preset list, showing "Custom" when the settings match none of the presets.
    void drawQualityPreset(RenderSettings& render) {
        const std::optional<QualityPreset> current = matchingQualityPreset(render);
        const char* preview = current ? qualityPresetNames[static_cast<std::size_t>(*current)] : "Custom";

        if (!ImGui::BeginCombo("Quality preset", preview)) {
            return;
        }

        for (std::size_t index = 0; index < qualityPresetNames.size(); ++index) {
            const QualityPreset preset = static_cast<QualityPreset>(index);

            if (ImGui::Selectable(qualityPresetNames[index], current == preset)) {
                applyQualityPreset(render, preset);
            }
        }

        ImGui::EndCombo();
    }
}

/*
* No imgui.ini: window positions are not worth a file of their own, and the settings file keeps what matters. Keyboard
* navigation stays off, so with the UI open the game keeps its keys (fly around while tuning). Sizes and the font
* follow the display's scale, so the UI is readable on a high-DPI screen.
*/
DebugUi::DebugUi(Window& window, GpuRenderer& renderer):
    window(window),
    renderer(renderer)
{
    try {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;

        ImGui::StyleColorsDark();
        const float scale = SDL_GetWindowDisplayScale(window.getSdlWindow());

        if (scale > 0.0f) {
            ImGuiStyle& style = ImGui::GetStyle();
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
        }

        if (!ImGui_ImplSDL3_InitForSDLGPU(window.getSdlWindow())) {
            throw std::runtime_error("Debug UI input start-up failed");
        }

        platformStarted = true;
        renderer.startDebugUi();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

DebugUi::~DebugUi() {
    cleanup();
}

// The renderer's half first: it waits for the GPU to finish with the UI's buffers before releasing them.
void DebugUi::cleanup() noexcept {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    renderer.stopDebugUi();

    if (platformStarted) {
        ImGui_ImplSDL3_Shutdown();
        platformStarted = false;
    }

    window.setDebugUiOpen(false);
    ImGui::DestroyContext();
}

bool DebugUi::isOpen() const {
    return open;
}

void DebugUi::setOpen(bool isOpen) {
    open = isOpen;
    window.setDebugUiOpen(isOpen);
}

void DebugUi::setStatus(std::string text) {
    status = std::move(text);
}

ImDrawData* DebugUi::build(const SettingsWalk& settings, RenderSettings& render, DebugUiRequests& requests) {
    if (!open) {
        return nullptr;
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos(ImVec2(16.0f * scale, 120.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f * scale, 640.0f * scale), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Settings (F5)", &open)) {
        drawQualityPreset(render);

        requests.save = ImGui::Button("Save");
        ImGui::SameLine();
        requests.resetToDefaults = ImGui::Button("Reset to defaults");

        if (!status.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::Separator();

        ImGuiSettingsVisitor visitor;
        settings(visitor);
    }

    ImGui::End();
    ImGui::Render();

    // Closed with the window's own button: this frame is still drawn, the next is not.
    if (!open) {
        window.setDebugUiOpen(false);
    }

    return ImGui::GetDrawData();
}
