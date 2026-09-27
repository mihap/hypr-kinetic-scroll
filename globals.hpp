#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>

// Plain extern globals, defined once in main.cpp. Deliberately NOT `inline`:
// inline variables become STB_GNU_UNIQUE symbols, which makes glibc mark the
// .so NODELETE (dlclose never unmaps it) and binds every later build of the
// plugin to the first build's object. That crashed Hyprland when the layout of
// such an object changed between builds.
extern HANDLE PHANDLE;

class KineticState;
extern KineticState* g_pKineticState;

// Config values are read through CConfigValue handles bound once (see
// KineticState::SConfig). Do not add per-call lookups here: constructing a
// CConfigValue registers it in Hyprland's global registry and resolves the
// name through the config manager, which is far too expensive per event.
