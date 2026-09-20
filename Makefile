# simple-login-gui
#
# One binary. X11 + Cairo + FreeType + libjpeg + PAM, and nothing else -- no GTK, no GLib, no
# toolkit. C++17 for the drawing and windowing layers, plain C11 for the PAM, privilege-drop
# and session-launch code.
#
# THE GFX LAYER DELIBERATELY DOES NOT LINK X11. canvas, fontstack and image need cairo and
# nothing more, which is what lets tools/uirender compose and audit the whole layout with no X
# server running. Keep it that way: an #include of Xlib.h in src/gfx/ costs the headless audit.

CC      ?= gcc
CXX     ?= g++

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
SHAREDIR    ?= $(PREFIX)/share/xlogin
BGDIR       ?= $(SHAREDIR)/backgrounds
SYSCONFDIR  ?= /etc

# --- packages, each verified present with pkg-config before use ---------------------------
GFX_PKGS  = cairo cairo-ft freetype2
X11_PKGS  = cairo-xlib x11 xrandr
JPEG_PKGS = libjpeg

GFX_CFLAGS  := $(shell pkg-config --cflags $(GFX_PKGS) $(JPEG_PKGS))
GFX_LIBS    := $(shell pkg-config --libs   $(GFX_PKGS) $(JPEG_PKGS))
X11_CFLAGS  := $(shell pkg-config --cflags $(X11_PKGS))
X11_LIBS    := $(shell pkg-config --libs   $(X11_PKGS))

# --- warnings and hardening ---------------------------------------------------------------
# -Werror is not negotiable: this tree vendors no upstream source, so every warning is ours.
WARN     = -Wall -Wextra -Werror -Wformat=2 -Wformat-security
HARDEN   = -O2 -fstack-protector-strong -fstack-clash-protection -fcf-protection=full \
           -D_FORTIFY_SOURCE=3 -fPIE
LDHARDEN = -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

DEFS = -DXLOGIN_RESOURCE_DIR_DEFAULT=\"$(SHAREDIR)\" \
       -DXLOGIN_BACKGROUND_DIR=\"$(BGDIR)\"

COMMON   = $(WARN) $(HARDEN) $(DEFS) -Isrc
CFLAGS   += -std=c11   $(COMMON)
CXXFLAGS += -std=c++17 $(COMMON)

# --- objects ------------------------------------------------------------------------------
GFX_OBJS = src/gfx/canvas.o src/gfx/fontstack.o src/gfx/image.o
PLAT_OBJS = src/platform/xerror.o src/platform/respath.o src/platform/x11window.o

.PHONY: all clean install uninstall gfx tools

# Phase 1 builds and audits the drawing layer. The xlogin binary arrives with main.cpp.
all: tools

gfx: $(GFX_OBJS)
tools: tools/uirender

# gfx objects: cairo only, no X11 in the include path at all.
src/gfx/%.o: src/gfx/%.cpp
	$(CXX) $(CXXFLAGS) $(GFX_CFLAGS) -c -o $@ $<

src/platform/%.o: src/platform/%.cpp
	$(CXX) $(CXXFLAGS) $(GFX_CFLAGS) $(X11_CFLAGS) -c -o $@ $<

src/session/%.o: src/session/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

tools/uirender: tools/uirender.o $(GFX_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(GFX_LIBS) $(LDHARDEN)

tools/%.o: tools/%.cpp
	$(CXX) $(CXXFLAGS) $(GFX_CFLAGS) -c -o $@ $<

clean:
	rm -f $(GFX_OBJS) $(PLAT_OBJS) tools/*.o tools/uirender xlogin

install:
	@echo "install: not wired up until the xlogin binary exists"; exit 1

uninstall:
	rm -f $(BINDIR)/xlogin $(BINDIR)/xlogin-launcher
	rm -f $(SYSCONFDIR)/pam.d/xlogin $(SYSCONFDIR)/xlogin.conf
	rm -rf $(SHAREDIR)
