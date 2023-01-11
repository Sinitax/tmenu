CFLAGS = -g -Wunused-variable

PREFIX ?= /usr
BINDIR ?= /bin

all: tmenu

clean:
	rm -f tmenu

tmenu: tmenu.c

install: tmenu
	install -m755 tmenu -t "$(DESTDIR)$(PREFIX)$(BINDIR)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)$(BINDIR)/tmenu"

.PHONY: all clean install uninstall
