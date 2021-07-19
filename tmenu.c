#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum {
	MODE_BROWSE,
	MODE_SEARCH,
};

enum {
	CODE_UP    = 0x100,
	CODE_DOWN  = 0x101,
	CODE_LEFT  = 0x102,
	CODE_RIGHT = 0x103,
};

void* checkp(void *p);
void die(const char *fmtstr, ...);
char* aprintf(const char *fmtstr, ...);
ssize_t fgetln(char *buf, size_t size, FILE *f);

int handleopt(const char *flag, const char **args);

void search_handlekey(int c);
void search_selnext();

const char *CSI_CLEARLINE = "\x1b[K";
const char *CSI_HIDECUR = "\x1b[25l";
const char *CSI_SHOWCUR = "\x1b[25h";

const char *prompts[] = {
	[MODE_BROWSE] = "(browse) ",
	[MODE_SEARCH] = "(search) ",
};

const char *userfile = NULL;

FILE *f;

size_t linesel, linecap, linec, *lines = NULL;

char searchbuf[1024] = { 0 };
size_t searchc = 0;

int mode = MODE_BROWSE;

void*
checkp(void *p)
{
	if (!p) die("Allocation failed.\n");
	return p;
}

void
die(const char *fmtstr, ...)
{
	va_list ap;

	va_start(ap, fmtstr);
	vfprintf(stderr, fmtstr, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

char*
aprintf(const char *fmtstr, ...)
{
	va_list ap, cpy;
	size_t size;
	char *astr;

	va_copy(cpy, ap);

	va_start(cpy, fmtstr);
	size = vsnprintf(NULL, 0, fmtstr, cpy);
	va_end(cpy);

	if (size < 0) die("Invalid fmtstr: %s\n", fmtstr);
	if (!(astr = malloc(size + 1))) die("OOM: fmtstr alloc failed\n");

	va_start(ap, fmtstr);
	vsnprintf(astr, size, fmtstr, ap);
	va_end(ap);

	return astr;
}

int
readcode(FILE *f)
{
	int c;

	if ((c = fgetc(f)) != '[') return c;
	switch ((c = fgetc(f))) {
	case 'A':
		return CODE_UP;
	case 'B':
		return CODE_DOWN;
	case 'C':
		return CODE_RIGHT;
	case 'D':
		return CODE_LEFT;
	}

	return c;
}

void
setent(size_t linei, size_t pos)
{
	if (linei >= linecap) {
		linecap *= 2;
		lines = checkp(realloc(lines, linecap));
	}
	lines[linei] = pos;
}

void
search_handlekey(int c)
{
	switch (c) {
	case 127: /* DEL */
		if (searchc) searchc--;
		return;
	default:
		if (searchc < sizeof(searchbuf))
			searchbuf[searchc++] = c & 0xff;
	}
	search_selnext();
}

size_t
linesize(size_t linei)
{
	return lines[linei + 1] - lines[linei];
}

char*
readline(char *buf, int linei)
{
	size_t nleft, nread;
	char *bp, *tok;

	fseek(f, lines[linei], SEEK_SET);
	nleft = linesize(linei);
	buf = bp = checkp(realloc(buf, nleft));
	while (nleft && (nread = fread(bp, 1, nleft, f)) > 0) {
		nleft -= nread;
		bp += nread;
	}
	if (nread < 0) die("Failed to read line %i from input\n", linei);
	tok = memchr(buf, '\n', linesize(linei));
	if (tok) *tok = '\0';

	return buf;
}

void
search_selnext()
{
	size_t i, linei, nread, nleft;
	char *line = NULL, *end, *bp;

	if (!searchc) return;

	for (i = 0; i < linec; i++) {
		linei = (linesel + 1 + i) % linec;
		line = bp = readline(line, linei);
		end = line + linesize(linei);
		while (end - bp && (bp = memchr(bp, searchbuf[0], end - bp))) {
			if (!memcmp(bp, searchbuf, MIN(end - bp, searchc))) {
				linesel = linei;
				goto exit;
			}
			bp += 1;
		}
	}

exit:
	free(line);
}

void
run()
{
	struct termios prevterm, newterm;
	ssize_t pos, start, nread;
	const char *prompt;
	struct winsize ws = { 0 };
	char *tok, iobuf[1024], *line = NULL;
	int c, termw;

	linecap = 100;
	lines = checkp(calloc(linecap, sizeof(size_t)));

	if (!userfile) {
		if (!(f = tmpfile()))
			die("Failed to create temporary file\n");

		linec = start = pos = 0;
		while ((nread = fgetln(iobuf, sizeof(iobuf), stdin)) > 0) {
			pos += nread;
			if (fwrite(iobuf, 1, nread, f) != nread)
				die("Writing to tmp file failed\n");
			if (iobuf[nread-1] == '\n') {
				setent(linec++, start);
				start = pos;
			}
		}
		setent(linec, pos);

		fseek(f, 0, SEEK_SET);

		if (!freopen("/dev/tty", "r", stdin))
			die("Failed to reattach to pseudo tty\n");
	} else {
		if (!(f = fopen(userfile, "r")))
			die("Failed to open file for reading: %s\n", userfile);

		linec = start = pos = 0;
		while ((nread = fgetln(iobuf, sizeof(iobuf), f)) > 0) {
			pos += nread;
			if (iobuf[nread-1] == '\n') {
				setent(linec++, start);
				start = pos;
			}
		}
		setent(linec, pos);

		fseek(f, 0, SEEK_SET);
	}

	if (!linec) return;

	if (tcgetattr(fileno(stdin), &prevterm))
		die("Failed to get terminal properies\n");

	cfmakeraw(&newterm);
	if (tcsetattr(fileno(stdin), TCSANOW, &newterm))
		die("Failed to set new terminal properties\n");

	termw = (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != -1) ? ws.ws_col : 80;

	fprintf(stderr, "%s", CSI_HIDECUR);

	linesel = 0;
	do {
		if (mode == MODE_BROWSE) {
			line = readline(line, linesel);
			fprintf(stderr, "%s%s%.*s\r", CSI_CLEARLINE,
				prompts[mode], termw - (int) strlen(prompts[mode]),
				line);
		} else if (mode == MODE_SEARCH) {
			line = readline(line, linesel);
			fprintf(stderr, "%s%s%.*s: %.*s\r", CSI_CLEARLINE,
				prompts[mode], (int) searchc, searchbuf,
				(int) (termw - strlen(prompts[mode]) - searchc),
				line);
		}

		c = getc(stdin);
		if (c == 0x1b) c = readcode(stdin);
		switch (c) {
		case 0x03: /* CTRL+C */
			goto exit;
		case 0x13: /* CTRL+S */
			if (mode != MODE_SEARCH) {
				mode = MODE_SEARCH;
				search_selnext();
			}
			break;
		case 0x02: /* CTRL+B */
			mode = MODE_BROWSE;
			break;
		case '\r':
			line = readline(line, linesel);
			printf("%.*s", (int) linesize(linesel), line);
			goto exit;
		case CODE_UP:
			if (linesel != 0) linesel--;
			break;
		case CODE_DOWN:
			if (linesel != linec - 1) linesel++;
			break;
		default:
			if (mode == MODE_SEARCH) {
				search_handlekey(c);
			}
		}
	} while (c >= 0);

exit:
	fprintf(stderr, "\r%s", CSI_CLEARLINE);
	fprintf(stderr, "\r%s", CSI_SHOWCUR);

	tcsetattr(fileno(stdin), TCSANOW, &prevterm);

	fclose(f);
}

ssize_t
fgetln(char *buf, size_t size, FILE *f)
{
	size_t i = 0;

	for (i = 0; i < size && (buf[i] = fgetc(f)) >= 0; i++)
		if (buf[i] == '\n') return i + 1;

	return (buf[i] < 0) ? -1 : i;
}

int
handleopt(const char *flag, const char **args)
{
	if (flag[0] && flag[1]) die("Unsupported flag: -%s\n", flag);

	return 0;
}

int
main(int argc, const char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (*argv[i] == '-') {
			i += handleopt(argv[i] + 1, &argv[i+1]);
		} else if (!userfile) {
			userfile = argv[i];
		} else {
			printf("Unexpected argument: %s\n", argv[i]);
		}
	}

	run();
}

