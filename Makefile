CFLAGS = -g -Wunused-variable

all: tmenu

clean:
	rm -f tmenu

tmenu: tmenu.c

.PHONY: all clean
