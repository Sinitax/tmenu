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

#define KEY_CTRL(c) (((int) (c)) & 0b11111)

#define CSI_CLEAR_LINE   "\x1b[K\r"
#define CSI_CUR_HIDE     "\x1b[?25l"
#define CSI_CUR_SHOW     "\x1b[?25h"
#define CSI_CUR_UP       "\x1b[A"
#define CSI_CUR_DOWN     "\x1b[B"
#define CSI_CUR_RIGHT    "\x1b[C"
#define CSI_CUR_LEFT     "\x1b[D"
#define CSI_STYLE_BOLD   "\x1b[1m"
#define CSI_STYLE_RESET  "\x1b[0m"
#define CSI_CLEAR_SCREEN "\x1b[2J"
#define CSI_CUR_GOTO     "\x1b[%i%iH"

enum {
	BWD = -1,
	FWD = 1,
};

enum {
	MODE_BROWSE,
	MODE_SEARCH,
};

enum {
	SEARCH_SUBSTR,
	SEARCH_FUZZY,
};

enum {
	CASE_SENSITIVE,
	CASE_INSENSITIVE,
};

enum {
	KEY_DEL = 0x7f,
	KEY_UP = 0x100,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_PGUP,
	KEY_PGDN,
};

struct mode {
	void (*prompt)();
	void (*cleanup)();
	void (*handlekey)();
};

struct searchmode {
	const char *sh;
	int (*matchfunc)(int, int, int, int, int);
};

void* checkp(void *p);
void die(const char *fmtstr, ...);
char* aprintf(const char *fmtstr, ...); /* yet unused */
char chrlower(char c);
char* strlower(const char *str);

int searchcmp(const char *a, const char *b, size_t size);
const char* searchfind(const char *a, char c, size_t size);

int readkey(FILE *f);
int freadln(char *buf, int size, FILE *f);

int entlen(int i);
char* readent(char *buf, int linei);
void setent(int linei, int start);

void browse_prompt();
void browse_handlekey(int c);
void browse_cleanup();

void search_prompt();
void search_handlekey(int c);
int search_match(int start, int dir, int new, int cnt, int fallback);
int search_match_substr(int start, int dir, int new, int cnt, int fallback);
int search_match_fuzzy(int start, int dir, int new, int cnt, int fallback);
void search_cleanup();

int run();
int handleopt(const char *flag, const char **args);
int main(int argc, const char **argv);

static const char *usage = \
	"Usage: tmenu [-h] [-m] [-c LINES] [-a AFTER] [-b BEFORE]";

static const char *userfile = NULL;


static FILE *f;
int exit_status = 0;

int entsel, entcap, entcnt, *entries = NULL;

static char *entry = NULL;

static char searchbuf[1024] = { 0 };
static int searchc = 0;

static int searchcase = CASE_SENSITIVE;
static int searchmode = SEARCH_SUBSTR;
struct searchmode searchmodes[] = {
	[SEARCH_SUBSTR] = {
		.sh = "SUB",
		.matchfunc = search_match_substr,
	},
	[SEARCH_FUZZY] = {
		.sh = "FUZ",
		.matchfunc = search_match_fuzzy,
	}
};

static int fwdctx = 1;
static int bwdctx = 1;
static int termw = 80;

struct mode modes[] = {
	[MODE_BROWSE] = {
		.prompt = browse_prompt,
		.handlekey = browse_handlekey,
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

void __attribute__((noreturn))
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
	int size;
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

char
chrlower(char c)
{
	if (c >= 'A' && c <= 'Z')
		c += 'a' - 'A';
	return c;
}

char*
strlower(const char *str)
{
	static char buf[1024], *bp;
	int i;

	strncpy(buf, str, sizeof(buf));
	for (bp = buf; *bp; bp++)
		*bp = chrlower(*bp);

	return buf;
}

int
searchcmp(const char *a, const char *b, size_t size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (searchcase == CASE_SENSITIVE) {
			if (a[i] != b[i])
				return a[i] - b[i];
		} else {
			if (chrlower(a[i]) != chrlower(b[i]))
				return chrlower(a[i]) - chrlower(b[i]);
		}
	}

	return 0;
}

const char*
searchfind(const char *a, char c, size_t size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (searchcase == CASE_SENSITIVE) {
			if (a[i] == c) return a + i;
		} else if (searchcase == CASE_INSENSITIVE) {
			if (chrlower(a[i]) == chrlower(c))
				return a + i;
		}
	}

	return NULL;
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

int
freadln(char *buf, int size, FILE *f)
{
	int c, i;

	for (c = i = 0; i < size && (c = fgetc(f)) >= 0; i++)
		if ((buf[i] = c) == '\n') return i + 1;

	return (c < 0) ? -1 : i;
}

int
entlen(int linei)
{
	return entries[linei + 1] - entries[linei];
}

char*
readent(char *buf, int linei)
{
	int nleft, nread;
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
setent(int linei, int pos)
{
	if (linei >= entcap) {
		entcap *= 2;
		entries = checkp(realloc(entries, entcap * sizeof(int)));
	}
	entries[linei] = pos;
}

void
browse_prompt()
{
	int i, promptlen;
	const char *prompt = "(browse) ";

	for (i = entsel - bwdctx; i <= entsel + fwdctx; i++) {
		if (i < 0 || i >= entcnt) {
			eprintf(CSI_CLEAR_LINE "\n");
			continue;
		}

		eprintf(CSI_CLEAR_LINE);

		promptlen = snprintf(NULL, 0, "(browse): ");

		entry = readent(entry, i);
		if (i == entsel) eprintf(CSI_STYLE_BOLD "(browse): ");
		else eprintf("%*.s", promptlen, " ");
		eprintf("%.*s\n", termw - promptlen, entry);
		if (i == entsel) eprintf(CSI_STYLE_RESET);
	}
	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf(CSI_CUR_UP);
}

void
browse_handlekey(int c)
{
	switch (c) {
	case KEY_UP:
		if (entsel != 0) entsel--;
		break;
	case KEY_DOWN:
		if (entsel != entcnt - 1) entsel++;
		break;
	}
}

void
browse_cleanup()
{
	int i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CLEAR_LINE "\n");
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CUR_UP);
}

void
search_prompt()
{
	int i, enti, promptlen;

	if (entsel == -1) entsel = 0;
	if ((enti = search_match(entsel, FWD, 0, 1, -1)) == -1) {
		entsel = search_match(entsel, BWD, 1, 1, -1);
	} else {
		entsel = enti;
	}

	for (i = -bwdctx; i <= fwdctx; i++) {
		if (entsel >= 0) {
			if (i < 0) {
				enti = search_match(entsel, BWD, 1, -i, -1);
			} else if (i == 0) {
				enti = entsel;
			} else if (i > 0) {
				enti = search_match(entsel, FWD, 1, i, -1);
			}
		} else {
			enti = -1;
		}

		promptlen = snprintf(NULL, 0, "(search[%c:%s]) %.*s: ",
				(searchcase == CASE_SENSITIVE) ? 'I' : 'i',
				searchmodes[searchmode].sh, searchc,
				searchbuf);

		eprintf(CSI_CLEAR_LINE);
		if (i == 0) {
			eprintf(CSI_STYLE_BOLD "(search[%c:%s]) %.*s: ",
				(searchcase == CASE_SENSITIVE) ? 'I' : 'i',
				searchmodes[searchmode].sh, searchc,
				searchbuf);
		} else {
			eprintf("%*.s", promptlen, " ");
		}

		if (enti < 0) {
			eprintf("\n");
			continue;
		} else {
			entry = readent(entry, enti);
			eprintf("%.*s\n", termw - promptlen, entry);
		}
		if (i == 0) eprintf(CSI_STYLE_RESET);
	}

	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf(CSI_CUR_UP);
}

void
search_handlekey(int c)
{
	switch (c) {
	case KEY_CTRL('E'):
		searchmode = SEARCH_SUBSTR;
		break;
	case KEY_CTRL('F'):
		searchmode = SEARCH_FUZZY;
		break;
	case KEY_CTRL('I'):
		searchcase ^= 1;
		break;
	case KEY_CTRL('K'):
	case KEY_UP:
		entsel = search_match(entsel, BWD, 1, 1, entsel);
		break;
	case KEY_CTRL('L'):
	case KEY_DOWN:
		entsel = search_match(entsel, FWD, 1, 1, entsel);
		break;
	case 0x20 ... 0x7e:
		if (searchc < sizeof(searchbuf) - 1)
			searchbuf[searchc++] = c & 0xff;
		break;
	case KEY_DEL:
		if (searchc) searchc--;
		break;
	}
	searchbuf[searchc] = '\0';
}

int
search_match(int start, int dir, int new, int cnt, int fallback)
{
	return searchmodes[searchmode].matchfunc(start, dir, new, cnt, fallback);
}

int
search_match_substr(int start, int dir, int new, int cnt, int fallback)
{
	int i, enti, res, rescnt;
	const char *end, *bp;

	if (!searchc) {
		res = start + dir * (new + cnt - 1);
		return (res >= 0 && res < entcnt) ? res : fallback;
	}

	rescnt = 0;
	for (i = new; i < entcnt; i++) {
		enti = start + dir * i;
		if (enti < 0 || enti >= entcnt) break;
		entry = readent(entry, enti);
		end = entry + entlen(enti);
		for (bp = entry; *bp; bp++) {
			if (!searchcmp(bp, searchbuf, MIN(end - bp, searchc))) {
				if (++rescnt == cnt) return enti;
				break;
			}
		}
	}

	return fallback;
}

int
search_match_fuzzy(int start, int dir, int new, int cnt, int fallback)
{
	int i, enti, res, rescnt;
	const char *end, *bp, *sbp;

	if (!searchc) {
		res = start + dir * (new + cnt - 1);
		return (res >= 0 && res < entcnt) ? res : fallback;
	}

	rescnt = 0;
	for (i = new; i < entcnt; i++) {
		enti = start + dir * i;
		if (enti < 0 || enti >= entcnt) break;
		bp = entry = readent(entry, enti);
		end = entry + entlen(enti);
		sbp = searchbuf;
		while (end - bp && *sbp && (bp = searchfind(bp, *sbp, end - bp))) {
			sbp++;
			bp++;
		}
		if (!*sbp) {
			if (++rescnt == cnt) return enti;
		}
	}

	return fallback;
}

void
search_cleanup()
{
	int i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CLEAR_LINE "\n");
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CUR_UP);
}

int
run()
{
	struct termios prevterm, newterm = { 0 };
	int pos, start, nread, enti;
	const char *prompt;
	struct winsize ws = { 0 };
	char *tok, iobuf[1024];
	int i, c;

	entcap = 100;
	entries = checkp(calloc(entcap, sizeof(int)));

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
	eprintf("Loaded %i entries\n", entcnt);

	if (tcgetattr(fileno(stdin), &prevterm))
		die("Failed to get term attrs\n");

	cfmakeraw(&newterm);
	newterm.c_oflag |= ONLCR | OPOST;
	if (tcsetattr(fileno(stdin), TCSANOW, &newterm))
		die("Failed to set term attrs\n");

	eprintf(CSI_CUR_HIDE);

	entsel = 0;
	do {
		if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != -1)
			termw = ws.ws_col;

		if (modes[mode].prompt) modes[mode].prompt();

		switch ((c = readkey(stdin))) {
		case KEY_CTRL('C'):
		case KEY_CTRL('D'):
			goto exit;
		case KEY_CTRL('Q'):
			exit_status = EXIT_FAILURE;
			goto exit;
		case KEY_CTRL('S'):
			mode = MODE_SEARCH;
			break;
		case KEY_CTRL('B'):
			mode = MODE_BROWSE;
			break;
		case KEY_CTRL('L'):
			eprintf(CSI_CLEAR_SCREEN CSI_CUR_GOTO, 0, 0);
			break;
		case KEY_CTRL('J'):
		case '\r':
			entry = readent(entry, entsel);
			if (modes[mode].cleanup) modes[mode].cleanup();
			printf("%.*s\n", entlen(entsel), entry);
			if (!multiout) goto exit;
			break;
		default:
			if (modes[mode].handlekey) modes[mode].handlekey(c);
			break;
		}
	} while (c >= 0);

exit:
	if (modes[mode].cleanup) modes[mode].cleanup();

	eprintf("%s", CSI_CUR_SHOW);

	tcsetattr(fileno(stdin), TCSANOW, &prevterm);

	fclose(f);

	return exit_status;
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
	case 'h':
		printf("%s\n", usage);
		return 0;
	}

	return 0;

badint:
	die("Invalid integer argument: %s\n", *args);
}

int
main(int argc, const char **argv)
{
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);

	for (i = 1; i < argc; i++) {
		if (*argv[i] == '-') {
			i += handleopt(argv[i] + 1, &argv[i+1]);
		} else if (!userfile) {
			userfile = argv[i];
		} else {
			die("Unexpected argument: %s\n", argv[i]);
		}
	}

	return run();
}
