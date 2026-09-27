PLUGIN_NAME = hypr-kinetic-scroll

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -g
LDFLAGS ?=

PKG_CONFIG = pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

SRC = main.cpp kinetic.cpp metrics.cpp
OUT = $(PLUGIN_NAME).so

# hyprpm source for `make reinstall`. Override: make reinstall REPO=... BRANCH=...
REPO   ?= https://github.com/mihap/hypr-kinetic-scroll
BRANCH ?=

all: $(OUT)

$(OUT): $(SRC) globals.hpp kinetic.hpp metrics.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SRC) -o $@ `$(PKG_CONFIG)`

clean:
	rm -f $(OUT)

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

.PHONY: all clean reinstall update reload dev-load unload-dev unload-all
