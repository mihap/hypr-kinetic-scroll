#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <sstream>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

HANDLE        PHANDLE         = nullptr;
KineticState* g_pKineticState = nullptr;

static Hyprutils::Signal::CHyprSignalListener g_pAxisCallback;
static Hyprutils::Signal::CHyprSignalListener g_pButtonCallback;
static Hyprutils::Signal::CHyprSignalListener g_pWindowCallback;
static Hyprutils::Signal::CHyprSignalListener g_pConfigReloadCallback;
static Hyprutils::Signal::CHyprSignalListener g_pRenderPreCallback;

static void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    if (!g_pKineticState)
        return;

    auto event = e;
    // Real scroll deltas always pass through. Only the finger-lift stop is
    // swallowed while momentum is live; the plugin sends its own stop later.
    if (g_pKineticState->onAxis(event))
        info.cancelled = true;
}

static void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& /*info*/) {
    if (!g_pKineticState || e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (*g_pKineticState->config().stopOnClick)
        g_pKineticState->stopKinetic(eStop::MOUSE_BUTTON);
}

static void onActiveWindow() {
    if (!g_pKineticState)
        return;
    if (*g_pKineticState->config().stopOnFocus)
        g_pKineticState->stopKinetic(eStop::ACTIVE_WINDOW);
}

static void onConfigPreReload() {
    if (g_pKineticState)
        g_pKineticState->resetAppRules();
}

static Hyprlang::CParseResult parseKineticScrollRule(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;
    std::istringstream     iss(value ? value : "");
    std::string            mode, appClass;

    if (!(iss >> mode)) {
        result.setError("Invalid format: expected 'enable [class]' or 'disable [class]'");
        return result;
    }

    bool enable = true;
    if (mode == "disable")
        enable = false;
    else if (mode != "enable") {
        result.setError("Invalid mode: expected 'enable' or 'disable'");
        return result;
    }

    if (!(iss >> appClass)) {
        g_pKineticState->setDefaultAppRule(enable);
        return result;
    }

    std::string extra;
    if (iss >> extra) {
        result.setError("Invalid format: expected 'enable [class]' or 'disable [class]'");
        return result;
    }

    g_pKineticState->setAppRule(appClass, enable);
    return result;
}

static int luaSetAppRule(lua_State* L, bool enabled) {
    const char* appClass = luaL_checkstring(L, 1);
    if (g_pKineticState)
        g_pKineticState->setAppRule(appClass, enabled);
    return 0;
}

static int luaEnable(lua_State* L) {
    return luaSetAppRule(L, true);
}

static int luaDisable(lua_State* L) {
    return luaSetAppRule(L, false);
}

static int luaEnableDefault(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->setDefaultAppRule(true);
    return 0;
}

static int luaDisableDefault(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->setDefaultAppRule(false);
    return 0;
}

static int luaResetRules(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->resetAppRules();
    return 0;
}

static void registerLuaFunctions() {
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "enable", luaEnable);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "disable", luaDisable);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "enable_default", luaEnableDefault);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "disable_default", luaDisableDefault);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "reset_rules", luaResetRules);
}

static void registerConfigValues() {
    using namespace Config::Values;
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:enabled", "Enable kinetic scrolling", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:decel", "Velocity multiplier per 16 ms (0.967 ~ macOS)", 0.967F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:min_velocity", "Stop below this many scroll units per 16 ms", 0.1F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:interval_ms", "Watchdog interval when no compositor frame arrives", 16));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:delta_multiplier", "Launch velocity multiplier", 1.0F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:velocity_window_ms", "Window before lift used to measure velocity", 64));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:lift_tail_cap_ms", "Max ms of motionless tail before lift counted against launch velocity (0 = auto)", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:disable_in_browser", "Disable kinetic scrolling in browsers", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_target_change", "Stop inertia when scroll target changes", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CStringValue>("plugin:kinetic-scroll:disabled_classes", "Comma or space separated classes with kinetic scrolling disabled", ""));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:debug", "Enable kinetic scroll debug logging", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CStringValue>("plugin:kinetic-scroll:metrics_file", "Append one JSON line per gesture with cost and behaviour metrics (empty = off)", ""));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_click", "Stop inertia on mouse click", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_focus", "Stop inertia on focus change", 0));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // NOTE: version check skipped for local dev (headers v0.53.3, running v0.53.1).
    // Re-enable for distribution:
    // if (__hyprland_api_get_hash() != __hyprland_api_get_client_hash())
    //     throw std::runtime_error("Version mismatch");

    registerConfigValues();

    // Create kinetic state (must be before registering keyword so it's available during config parse)
    g_pKineticState = new KineticState();

    HyprlandAPI::addConfigKeyword(PHANDLE, "kinetic-scroll-rule", parseKineticScrollRule, {});
    registerLuaFunctions();

    // Register event callbacks
    g_pAxisCallback = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_pButtonCallback = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_pWindowCallback = Event::bus()->m_events.window.active.listen(onActiveWindow);
    g_pConfigReloadCallback = Event::bus()->m_events.config.preReload.listen(onConfigPreReload);
    g_pRenderPreCallback    = Event::bus()->m_events.render.pre.listen([](PHLMONITOR mon) {
        if (g_pKineticState)
            g_pKineticState->onRenderPre(mon);
    });

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Release callback refs (Hyprland auto-cleans registered callbacks on plugin unload,
    // but resetting our SPs ensures deterministic ordering)
    g_pAxisCallback.reset();
    g_pButtonCallback.reset();
    g_pWindowCallback.reset();
    g_pConfigReloadCallback.reset();
    g_pRenderPreCallback.reset();

    // Clean up kinetic state (removes wl timers)
    delete g_pKineticState;
    g_pKineticState = nullptr;
}
