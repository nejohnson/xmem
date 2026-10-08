CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11 -lm
PREFIX ?= $(HOME)/.local

all: xmem

xmem: xmem.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xmem

# Install the binary, a desktop entry and an icon. GNOME shows the icon from
# the desktop entry, matched by WM_CLASS, and ignores the window's own icon.
install: xmem
	install -Dm755 xmem $(DESTDIR)$(PREFIX)/bin/xmem
	install -Dm644 xmem.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/xmem.svg
	sed 's|^Exec=.*|Exec=$(PREFIX)/bin/xmem|' xmem.desktop > xmem.desktop.tmp
	install -Dm644 xmem.desktop.tmp $(DESTDIR)$(PREFIX)/share/applications/xmem.desktop
	rm -f xmem.desktop.tmp
	-gtk-update-icon-cache -q -t $(DESTDIR)$(PREFIX)/share/icons/hicolor
	-update-desktop-database -q $(DESTDIR)$(PREFIX)/share/applications

.PHONY: all clean install
