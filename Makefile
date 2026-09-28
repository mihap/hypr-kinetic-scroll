PLUGIN_NAME = hypr-kinetic-scroll

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -g
LDFLAGS ?=

PKG_CONFIG = pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon libeis-1.0

# Development only: load a build into a Hyprland it was not built against.
ifdef SKIP_VERSION_CHECK
CXXFLAGS += -DKINETIC_SKIP_VERSION_CHECK
endif

SRC = main.cpp kinetic.cpp gesture.cpp metrics.cpp
OUT = $(PLUGIN_NAME).so
HDR = globals.hpp kinetic.hpp gesture.hpp metrics.hpp physics.hpp

# Compositor-free tests: the physics and the gesture state machine. Only
# libwayland-server is needed (metrics.cpp's idle callback), not Hyprland.
TESTS         = tests/physics_test tests/gesture_test
TEST_CXXFLAGS = -O2 -std=c++2b -g -I.

# hyprpm source for `make reinstall`. Override: make reinstall REPO=... BRANCH=...
REPO   ?= https://github.com/mihap/hypr-kinetic-scroll
BRANCH ?=

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SRC) -o $@ `$(PKG_CONFIG)`

clean:
	rm -f $(OUT) $(TESTS)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

tests/physics_test: tests/physics_test.cpp physics.hpp
	$(CXX) $(TEST_CXXFLAGS) $< -o $@

tests/gesture_test: tests/gesture_test.cpp gesture.cpp gesture.hpp metrics.cpp metrics.hpp physics.hpp
	$(CXX) $(TEST_CXXFLAGS) $< gesture.cpp metrics.cpp -o $@ `pkg-config --cflags --libs wayland-server`

# --- hyprpm -----------------------------------------------------------------
# hyprpm clones the repository's default branch only, so BRANCH must be a
# revision reachable from it (or empty to use the default branch itself).
# hyprpm asks for sudo; run these from a terminal.

reinstall:
	-hyprpm remove $(PLUGIN_NAME)
	hyprpm add $(REPO) $(BRANCH)
	hyprpm enable $(PLUGIN_NAME)
	@echo "Installed. Run 'make reload' to load it into the running Hyprland."

# Rebuild after a Hyprland update (hyprpm refreshes headers and rebuilds).
update:
	hyprpm update

# Unload any development build loaded straight from a .so, then let hyprpm
# load the installed one. Loading both would run two copies of the plugin.
reload: unload-dev
	hyprpm reload -n

# --- development ------------------------------------------------------------
# Build and hot-load into the running Hyprland from a fresh /tmp path,
# replacing any previously loaded copy (hyprpm's or a dev build).
dev-load: $(OUT) unload-all
	@tmp=/tmp/$(PLUGIN_NAME)-dev-$$(date +%s).so; \
	cp $(OUT) $$tmp && chmod 755 $$tmp && hyprctl plugin load $$tmp && echo "loaded $$tmp"

# Unload copies loaded from /tmp (dev builds) but leave hyprpm's alone.
unload-dev:
	@for p in $$(grep -h "/tmp/$(PLUGIN_NAME)-" /proc/$$(pidof Hyprland | cut -d' ' -f1)/maps 2>/dev/null | awk '{print $$6}' | sort -u); do \
		hyprctl plugin unload $$p >/dev/null && echo "unloaded $$p"; done

# Unload every loaded copy, whatever its path.
unload-all:
	@for p in $$(grep -h "$(PLUGIN_NAME)" /proc/$$(pidof Hyprland | cut -d' ' -f1)/maps 2>/dev/null | awk '{print $$6}' | sort -u); do \
		hyprctl plugin unload $$p >/dev/null && echo "unloaded $$p"; done

.PHONY: all clean test reinstall update reload dev-load unload-dev unload-all
