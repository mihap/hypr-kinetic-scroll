#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <string>

// Plain extern globals, defined once in main.cpp. Deliberately NOT `inline`:
// inline variables become STB_GNU_UNIQUE symbols, which makes glibc mark the
// .so NODELETE (dlclose never unmaps it) and binds every later build of the
// plugin to the first build's object. That crashed Hyprland when the layout of
// such an object changed between builds.
extern HANDLE PHANDLE;

class KineticState;
extern KineticState* g_pKineticState;

inline int getKineticConfigInt(const std::string& name, int fallback) {
    const auto OPTION = CConfigValue<Config::INTEGER>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}

inline double getKineticConfigFloat(const std::string& name, double fallback) {
    const auto OPTION = CConfigValue<Config::FLOAT>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}

inline std::string getKineticConfigString(const std::string& name, const std::string& fallback) {
    const auto OPTION = CConfigValue<Config::STRING>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}
