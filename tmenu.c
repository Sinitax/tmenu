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
char* aprintf(const char *fmtstr, ...); /* yet unused */

int readkey(FILE *f);
ssize_t freadln(char *buf, size_t size, FILE *f);

size_t entlen(size_t i);
char* readent(char *buf, size_t linei);
void setent(size_t linei, size_t start);

void search_handlekey(int c);
void search_selnext();

int run();
int handleopt(const char *flag, const char **args);
int main(int argc, const char **argv);

const char *CSI_CLEARLINE = "\x1b[K";
const char *CSI_HIDECUR = "\x1b[25l";
const char *CSI_SHOWCUR = "\x1b[25h";

const char *prompts[] = {
	[MODE_BROWSE] = "(browse) ",
	[MODE_SEARCH] = "(search) ",
};

const char *userfile = NULL;

FILE *f;

size_t entsel, entcap, entcnt, *entries = NULL;

char searchbuf[1024] = { 0 };
size_t searchc = 0;

int mode = MODE_BROWSE;
int multiout = 0;

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
readkey(FILE *f)
{
	int c;

	if ((c = fgetc(f)) != '\x1b') return c;
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

ssize_t
freadln(char *buf, size_t size, FILE *f)
{
	size_t i = 0;

	for (i = 0; i < size && (buf[i] = fgetc(f)) >= 0; i++)
		if (buf[i] == '\n') return i + 1;

	return (buf[i] < 0) ? -1 : i;
}

size_t
entlen(size_t linei)
{
	return entries[linei + 1] - entries[linei];
}

char*
readent(char *buf, size_t linei)
{
	size_t nleft, nread;
	char *bp, *tok;

	fseek(f, entries[linei], SEEK_SET);
	nleft = entlen(linei);
	buf = bp = checkp(realloc(buf, nleft));
	while (nleft && (nread = fread(bp, 1, nleft, f)) > 0) {
		nleft -= nread;
		bp += nread;
	}
	if (nread < 0) die("Failed to read line %i from input\n", linei);
	tok = memchr(buf, '\n', entlen(linei));
	if (tok) *tok = '\0';

	return buf;
}

void
setent(size_t linei, size_t pos)
{
	if (linei >= entcap) {
		entcap *= 2;
		entries = checkp(realloc(entries, entcap));
	}
	entries[linei] = pos;
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

void
search_selnext()
{
	size_t i, linei, nread, nleft;
	char *line = NULL, *end, *bp;

	if (!searchc) {
		entsel = (entsel + 1) % entcnt;
		return;
	}

	for (i = 0; i < entcnt; i++) {
		linei = (entsel + 1 + i) % entcnt;
		line = bp = readent(line, linei);
		end = line + entlen(linei);
		while (end - bp && (bp = memchr(bp, searchbuf[0], end - bp))) {
			if (!memcmp(bp, searchbuf, MIN(end - bp, searchc))) {
				entsel = linei;
				goto exit;
			}
			bp += 1;
		}
	}

exit:
	free(line);
}

int
run()
{
	struct termios prevterm, newterm;
	ssize_t pos, start, nread;
	const char *prompt;
	struct winsize ws = { 0 };
	char *tok, iobuf[1024], *line = NULL;
	int c, termw;

	entcap = 100;
	entries = checkp(calloc(entcap, sizeof(size_t)));

	if (!userfile) {
		if (!(f = tmpfile()))
			die("Failed to create temporary file\n");

		entcnt = start = pos = 0;
		while ((nread = freadln(iobuf, sizeof(iobuf), stdin)) > 0) {
			pos += nread;
			if (fwrite(iobuf, 1, nread, f) != nread)
				die("Writing to tmp file failed\n");
			if (iobuf[nread-1] == '\n') {
				setent(entcnt++, start);
				start = pos;
			}
		}
		setent(entcnt, pos);

		fseek(f, 0, SEEK_SET);

		if (!freopen("/dev/tty", "r", stdin))
			die("Failed to reattach to pseudo tty\n");
		if (fread(NULL, 0, 0, f) < 0) return EXIT_FAILURE;
	} else {
		if (!(f = fopen(userfile, "r")))
			die("Failed to open file for reading: %s\n", userfile);

		entcnt = start = pos = 0;
		while ((nread = freadln(iobuf, sizeof(iobuf), f)) > 0) {
			pos += nread;
			if (iobuf[nread-1] == '\n') {
				setent(entcnt++, start);
				start = pos;
			}
		}
		setent(entcnt, pos);

		fseek(f, 0, SEEK_SET);
	}

	if (!entcnt) return EXIT_SUCCESS;

	if (tcgetattr(fileno(stdin), &prevterm))
		die("Failed to get term attrs\n");

	cfmakeraw(&newterm);
	if (tcsetattr(fileno(stdin), TCSANOW, &newterm))
		die("Failed to set term attrs\n");

	termw = (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != -1) ? ws.ws_col : 80;

	fprintf(stderr, "%s", CSI_HIDECUR);

	entsel = 0;
	do {
		if (mode == MODE_BROWSE) {
			line = readent(line, entsel);
			fprintf(stderr, "%s%s%.*s\r", CSI_CLEARLINE,
				prompts[mode], termw - (int) strlen(prompts[mode]),
				line);
		} else if (mode == MODE_SEARCH) {
			line = readent(line, entsel);
			fprintf(stderr, "%s%s%.*s: %.*s\r", CSI_CLEARLINE,
				prompts[mode], (int) searchc, searchbuf,
				(int) (termw - strlen(prompts[mode]) - searchc),
				line);
		}

		switch ((c = readkey(stdin))) {
		case 0x03: /* CTRL+C */
			goto exit;
		case 0x13: /* CTRL+S */
			mode = MODE_SEARCH;
			search_selnext();
			break;
		case 0x02: /* CTRL+B */
			mode = MODE_BROWSE;
			break;
		case CODE_UP:
			mode = MODE_BROWSE;
			if (entsel != 0) entsel--;
			break;
		case CODE_DOWN:
			mode = MODE_BROWSE;
			if (entsel != entcnt - 1) entsel++;
			break;
		case '\r': /* NEWLINE */
			line = readent(line, entsel);
			printf("%.*s\n", (int) entlen(entsel), line);
			if (!multiout) goto exit;
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

	return EXIT_SUCCESS;
}

int
handleopt(const char *flag, const char **args)
{
	if (flag[0] && flag[1]) die("Unsupported flag: -%s\n", flag);

	switch (flag[0]) {
	case 'm':
		multiout = 1;
		break;
	}

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

	return run();
}
