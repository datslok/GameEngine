#pragma once

#include "core/settings_visitor.h"
#include "platform/window.h"
#include "render/vulkan/vulkan_renderer.h"
#include "render/gpu/render_settings.h"

#include <string>

struct ImDrawData;

// What the player asked for in this frame's debug UI; the engine, which owns the settings file, carries it out.
struct DebugUiRequests {
    bool save = false;
    bool resetToDefaults = false;
};

/*
* The debug UI (F5): one window of sliders, checkboxes and lists, drawn with Dear ImGui from the settings' visit()
* functions, so every field of every settings struct gets a control without any UI code of its own. Fields read only
* at start-up are shown greyed out. Above them: the quality preset (render/gpu/quality_preset.h), and buttons to save
* the settings or return them to their defaults.
*
* Owns the ImGui context and both of its backends (SDL events, and the renderer's drawing), so it must be destroyed before the
* window and renderer it was given: declare it after them.
*/
class DebugUi {
public:
    DebugUi(Window& window, VulkanRenderer& renderer);
    ~DebugUi();

    // One ImGui context per program.
    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;

    bool isOpen() const;
    void setOpen(bool open);

    /*
    * Build this frame's UI, changing the walked settings as the player moves controls. Returns what to draw
    * (VulkanRenderer::render's ui), valid until the next build, or null while closed, when nothing is built at all.
    */
    ImDrawData* build(const SettingsWalk& settings, RenderSettings& render, DebugUiRequests& requests);

    // A line shown under the buttons: what the last save or reset did.
    void setStatus(std::string status);

private:
    void cleanup() noexcept;

    Window& window;
    VulkanRenderer& renderer;
    bool open = false;
    bool platformStarted = false;
    std::string status;
};
