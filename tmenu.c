#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define eprintf(...) fprintf(stderr, __VA_ARGS__)

enum {
	BWD = -1,
	FWD = 1
};

enum {
	MODE_BROWSE,
	MODE_SEARCH,
};

enum {
	KEY_UP    = 0x100,
	KEY_DOWN  = 0x101,
	KEY_LEFT  = 0x102,
	KEY_RIGHT = 0x103,
};

struct mode {
	void (*prompt)();
	void (*cleanup)();
	void (*handlekey)();
};

void* checkp(void *p);
void die(const char *fmtstr, ...);
char* aprintf(const char *fmtstr, ...); /* yet unused */

int readkey(FILE *f);
ssize_t freadln(char *buf, size_t size, FILE *f);

size_t entlen(size_t i);
char* readent(char *buf, size_t linei);
void setent(size_t linei, size_t start);

void browse_prompt();
void browse_cleanup();

void search_prompt();
void search_handlekey(int c);
ssize_t search_match(int start, int dir, int new, int cnt, int fallback);
void search_cleanup();

int run();
int handleopt(const char *flag, const char **args);
int main(int argc, const char **argv);

static const char *CSI_CLEAR_LINE = "\x1b[K";
static const char *CSI_CUR_HIDE = "\x1b[?25l";
static const char *CSI_CUR_SHOW = "\x1b[?25h";
static const char *CSI_CUR_UP = "\x1b[A";
static const char *CSI_CUR_DOWN = "\x1b[B";
static const char *CSI_CUR_RIGHT = "\x1b[C";
static const char *CSI_CUR_LEFT = "\x1b[D";
static const char *CSI_STYLE_BOLD = "\x1b[1m";
static const char *CSI_STYLE_RESET = "\x1b[0m";

static const char *userfile = NULL;

static FILE *f;

ssize_t entsel, entcap, entcnt, *entries = NULL;

static char *entry = NULL;

static char searchbuf[1024] = { 0 };
static ssize_t searchc = 0;
static int fwdctx = 1;
static int bwdctx = 1;
static int termw = 80;

struct mode modes[] = {
	[MODE_BROWSE] = {
		.prompt = browse_prompt,
		.handlekey = NULL,
		.cleanup = browse_cleanup
	},
	[MODE_SEARCH] = {
		.prompt = search_prompt,
		.handlekey = search_handlekey,
		.cleanup = search_cleanup
	}
};

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
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	case 'C':
		return KEY_RIGHT;
	case 'D':
		return KEY_LEFT;
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
browse_prompt()
{
	ssize_t i;
	const char *prompt = "(browse) ";

	for (i = entsel - bwdctx; i <= entsel + fwdctx; i++) {
		if (i < 0 || i >= entcnt) {
			eprintf("%s\r\n", CSI_CLEAR_LINE);
			continue;
		}
		entry = readent(entry, i);
		eprintf("%s", CSI_CLEAR_LINE);
		if (i == entsel) eprintf("%s%s: ", CSI_STYLE_BOLD, prompt);
		else eprintf("%*.s", (int) strlen(prompt) + 2, " ");
		eprintf("%.*s\r\n", (int) (termw - strlen(prompt)), entry);
		if (i == entsel) eprintf("%s", CSI_STYLE_RESET);
	}
	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf("%s", CSI_CUR_UP);
}

void
browse_handlekey(int c)
{
}

void
browse_cleanup()
{
	ssize_t i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf("%s\n\r", CSI_CLEAR_LINE);
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf("%s", CSI_CUR_UP);
}

void
search_prompt()
{
	ssize_t i, enti;
	const char *prompt = "(search) ";

	if ((enti = search_match(entsel, FWD, 0, 1, -1)) == -1)
		entsel = search_match(entsel, BWD, 0, 1, 0);
	else
		entsel = enti;

	for (i = - bwdctx; i <= fwdctx; i++) {
		if (i < 0) {
			enti = search_match(entsel, BWD, 1, -i, -1);
		} else if (i == 0) {
			enti = entsel;
		} else if (i > 0) {
			enti = search_match(entsel, FWD, 1, i, -1);
		}

		eprintf("%s", CSI_CLEAR_LINE);
		if (i == 0) eprintf("%s%s%.*s: ", CSI_STYLE_BOLD, prompt,
				    (int) searchc, searchbuf);
		else eprintf("%*.s", (int) strlen(prompt) + 2, " ");

		if (enti == -1) {
			eprintf("%s\r\n", CSI_CLEAR_LINE);
			continue;
		} else {
			entry = readent(entry, enti);
			eprintf("%.*s\r\n", (int) (termw - strlen(prompt)),
				entry);
		}
		if (i == 0) eprintf("%s", CSI_STYLE_RESET);
	}
	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf("%s", CSI_CUR_UP);
}

void
search_handlekey(int c)
{
	switch (c) {
	case 0x0a: /* CTRL+J */
		entsel = search_match(entsel, BWD, 1, 1, entsel);
		break;
	case 0x0b: /* CTRL+K */
		entsel = search_match(entsel, FWD, 1, 1, entsel);
		break;
	case 0x20 ... 0x7e:
		if (searchc < sizeof(searchbuf))
			searchbuf[searchc++] = c & 0xff;
		entsel = search_match(entsel, FWD, 0, 1, entsel);
		break;
	case 127: /* DEL */
		if (searchc) searchc--;
		break;
	}
}

ssize_t
search_match(int start, int dir, int new, int cnt, int fallback)
{
	ssize_t i, enti, res, rescnt;
	char *line = NULL, *end, *bp;

	if (!searchc) {
		res = start + dir * new;
		return (res >= 0 && res < entcnt) ? res : fallback;
	}

	res = fallback;
	rescnt = 0;
	for (i = 0; i < entcnt; i++) {
		enti = start + dir * (new + i);
		if (enti < 0 || enti >= entcnt) break;
		line = bp = readent(line, enti);
		end = line + entlen(enti);
		while (end - bp && (bp = memchr(bp, searchbuf[0], end - bp))) {
			if (!memcmp(bp, searchbuf, MIN(end - bp, searchc))) {
				rescnt++;
				if (rescnt == cnt) {
					res = enti;
					goto exit;
				}
			}
			bp += 1;
		}
	}

exit:
	free(line);
	return res;
}

void
search_cleanup()
{
	ssize_t i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf("%s\n\r", CSI_CLEAR_LINE);
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf("%s", CSI_CUR_UP);
}

int
run()
{
	struct termios prevterm, newterm = { 0 };
	ssize_t pos, start, nread, enti;
	const char *prompt;
	struct winsize ws = { 0 };
	char *tok, iobuf[1024];
	int i, c, termw;

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

	if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != -1)
		termw = ws.ws_col;

	eprintf("%s", CSI_CUR_HIDE);

	entsel = 0;
	do {
		if (modes[mode].prompt)
			modes[mode].prompt();

		switch ((c = readkey(stdin))) {
		case 0x03: /* CTRL+C */
			goto exit;
		case 0x13: /* CTRL+S */
			entsel = search_match(entsel, FWD,
				mode == MODE_SEARCH, 1, entsel);
			mode = MODE_SEARCH;
			break;
		case 0x02: /* CTRL+B */
			mode = MODE_BROWSE;
			break;
		case KEY_UP:
			mode = MODE_BROWSE;
			if (entsel != 0) entsel--;
			break;
		case KEY_DOWN:
			mode = MODE_BROWSE;
			if (entsel != entcnt - 1) entsel++;
			break;
		case '\r': /* NEWLINE */
			entry = readent(entry, entsel);
			printf("%.*s\n", (int) entlen(entsel), entry);
			if (!multiout) goto exit;
			break;
		default:
			if (modes[mode].handlekey)
				modes[mode].handlekey(c);
			break;
		}
	} while (c >= 0);

exit:
	if (modes[mode].cleanup)
		modes[mode].cleanup();

	eprintf("%s", CSI_CUR_SHOW);

	tcsetattr(fileno(stdin), TCSANOW, &prevterm);

	fclose(f);

	return EXIT_SUCCESS;
}

int
handleopt(const char *flag, const char **args)
{
	char *end;
	int tmp;

	if (flag[0] && flag[1]) die("Unsupported flag: -%s\n", flag);

	switch (flag[0]) {
	case 'm':
		multiout = 1;
		return 0;
	case 'b':
		fwdctx = strtol(*args, &end, 10);
		if (end && *end) goto badint;
		return 1;
	case 'a':
		bwdctx = strtol(*args, &end, 10);
		if (end && *end) goto badint;
		return 1;
	case 'c':
		tmp = strtol(*args, &end, 10);
		if (end && *end) goto badint;
		bwdctx = tmp / 2;
		fwdctx = tmp - tmp / 2;
		return 1;
	}

	return 0;

badint:
	die("Invalid integer argument: %s\n", *args);
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
