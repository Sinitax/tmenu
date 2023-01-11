#include <stddef.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <err.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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
	KEY_NONE = 0,
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
	bool (*handlekey)();
};

struct searchmode {
	const char *sh;
	ssize_t (*match)(size_t, int, bool, size_t, ssize_t);
};

void browse_prompt(void);
bool browse_handlekey(int c);
void browse_cleanup(void);

void search_prompt(void);
bool search_handlekey(int c);
void search_cleanup(void);

ssize_t search_match(size_t start, int dir,
	bool new, size_t cnt, ssize_t fallback);
ssize_t search_match_substr(size_t start, int dir,
	bool new, size_t cnt, ssize_t fallback);
ssize_t search_match_fuzzy(size_t start, int dir,
	bool new, size_t cnt, ssize_t fallback);

static const char *usage = \
	"Usage: tmenu [-h] [-m] [-a LINES] [-b LINES]";

static const struct searchmode searchmodes[] = {
	[SEARCH_SUBSTR] = {
		.sh = "SUB",
		.match = search_match_substr,
	},
	[SEARCH_FUZZY] = {
		.sh = "FUZ",
		.match = search_match_fuzzy,
	}
};

static const struct mode modes[] = {
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

static FILE *infile;

static bool verbose;

static size_t *entries;
static size_t entries_cap, entries_cnt;

static ssize_t selected;
static char *entry;

static char searchbuf[1024];
static size_t searchlen;

static int mode = MODE_BROWSE;
static int searchcase = CASE_SENSITIVE;
static int searchmode = SEARCH_SUBSTR;

static int fwdctx = 1;
static int bwdctx = 1;
static int termw = 80;

static bool multiout = false;

char
lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		c += 'a' - 'A';
	return c;
}

int
search_cmp(const char *a, const char *b, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (searchcase == CASE_SENSITIVE) {
			if (a[i] != b[i])
				return a[i] - b[i];
		} else {
			if (lower(a[i]) != lower(b[i]))
				return lower(a[i]) - lower(b[i]);
		}
	}

	return 0;
}

const char*
search_find(const char *a, char c, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (searchcase == CASE_SENSITIVE) {
			if (a[i] == c) return a + i;
		} else if (searchcase == CASE_INSENSITIVE) {
			if (lower(a[i]) == lower(c))
				return a + i;
		}
	}

	return NULL;
}

int
readkey(FILE *f)
{
	int c;

	c = fgetc(f);
	if (c != '\x1b')
		return c;

	if (fgetc(f) != '[')
		return KEY_NONE;

	switch (fgetc(f)) {
	case 'A':
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	case 'C':
		return KEY_RIGHT;
	case 'D':
		return KEY_LEFT;
	case '5':
		return fgetc(f) == '~' ? KEY_PGUP : KEY_NONE;
	case '6':
		return fgetc(f) == '~' ? KEY_PGDN : KEY_NONE;
	}

	return KEY_NONE;
}

size_t
freadln(char *buf, size_t size, FILE *f)
{
	size_t i;
	int c;

	for (i = 0; i < size; i++) {
		c = fgetc(f);
		if (c < 0) return -1;
		buf[i] = c;
		if (c == '\n')
			return i + 1;
	}

	return i;
}

size_t
entry_len(size_t index)
{
	return entries[index + 1] - entries[index];
}

char *
read_entry(char *buf, size_t index)
{
	ssize_t nleft, nread;
	char *pos, *tok;

	fseek(infile, entries[index], SEEK_SET);

	nleft = entry_len(index);
	buf = realloc(buf, nleft);
	if (!buf) err(1, "realloc");

	pos = buf;
	while (nleft > 0) {
		nread = fread(pos, 1, nleft, infile);
		if (!nread) break;
		if (nread < 0) err(1, "fread");
		nleft -= nread;
		pos += nread;
	}

	tok = memchr(buf, '\n', entry_len(index));
	if (tok) *tok = '\0';

	return buf;
}

void
add_entry(size_t index, size_t pos)
{
	if (index >= entries_cap) {
		entries_cap *= 2;
		entries = realloc(entries,
			entries_cap * sizeof(size_t));
		if (!entries) err(1, "realloc");
	}
	entries[index] = pos;
}

void
browse_prompt(void)
{
	ssize_t i;

	if (selected < 0) selected = 0;

	for (i = selected - bwdctx; i <= selected + fwdctx; i++) {
		eprintf(CSI_CLEAR_LINE);
		if (i == selected) {
			eprintf(CSI_STYLE_BOLD);
			eprintf("(browse): ");
		} else {
			eprintf("%*.s", 10, " ");
		}

		if (selected >= 0 && i >= 0 && i < entries_cnt) {
			entry = read_entry(entry, i);
			eprintf("%.*s\n", termw - 10, entry);
		} else {
			eprintf("\n");
		}

		if (i == selected)
			eprintf(CSI_STYLE_RESET);
	}

	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf(CSI_CUR_UP);
}

bool
browse_handlekey(int c)
{
	int cnt;
	
	switch (c) {
	case 'g':
		selected = 0;
		break;
	case 'G':
		selected = entries_cnt - 1;
		break;
	case 'q':
		return true;
	case KEY_PGUP:
		cnt = fwdctx + bwdctx + 1;
		if (selected > cnt)
			selected -= cnt;
		else
			selected = 0;
		break;
	case KEY_PGDN:
		cnt = fwdctx + bwdctx + 1;
		if (selected < entries_cnt - cnt)
			selected += cnt;
		else
			selected = entries_cnt - 1;
		break;
	case KEY_UP:
		if (selected != 0)
			selected--;
		break;
	case KEY_DOWN:
		if (selected != entries_cnt - 1)
			selected++;
		break;
	}

	return false;
}

void
browse_cleanup(void)
{
	size_t i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CLEAR_LINE "\n");
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CUR_UP);
}

void
search_prompt(void)
{
	char prompt[256];
	ssize_t i, index;
	ssize_t len, entlen;

	if (selected < 0) selected = 0;

	index = search_match(selected, FWD, 0, 1, -1);
	if (index != -1) {
		selected = index;
	} else {
		selected = search_match(selected, BWD, 1, 1, -1);
	}

	len = snprintf(prompt, sizeof(prompt), "(search[%c:%s]) %.*s",
			(searchcase == CASE_SENSITIVE) ? 'I' : 'i',
			searchmodes[searchmode].sh, (int) searchlen, searchbuf);
	if (len < 0) err(1, "snprintf");

	for (i = -bwdctx; i <= fwdctx; i++) {
		if (selected >= 0) {
			if (i < 0) {
				index = search_match(selected, BWD, 1, -i, -1);
			} else if (i == 0) {
				index = selected;
			} else if (i > 0) {
				index = search_match(selected, FWD, 1, i, -1);
			}
		} else {
			index = -1;
		}

		eprintf(CSI_CLEAR_LINE);

		if (i == 0) {
			eprintf(CSI_STYLE_BOLD);
			eprintf("%s : ", prompt);
		} else {
			eprintf("%*.s", (int) (len + 3), " ");
		}

		if (index < 0) {
			eprintf("\n");
		} else {
			entlen = entry_len(index);
			entry = read_entry(entry, index);
			if (entlen > termw - len - 3) {
				eprintf("..%.*s\n", (int) (termw - len - 5),
					entry + MAX(0, entlen - (termw - len - 5)));
			} else {
				eprintf("%.*s\n", (int) (termw - len - 3), entry);
			}
		}

		if (i == 0) eprintf(CSI_STYLE_RESET);
	}

	for (i = 0; i < bwdctx + fwdctx + 1; i++)
		eprintf(CSI_CUR_UP);
}

bool
search_handlekey(int c)
{
	int cnt;

	switch (c) {
	case KEY_CTRL('I'):
		searchcase ^= 1;
		break;
	case KEY_PGUP:
		cnt = fwdctx + bwdctx + 1;
		selected = search_match(selected, BWD, 1, cnt, selected);
		break;
	case KEY_PGDN:
		cnt = fwdctx + bwdctx + 1;
		selected = search_match(selected, FWD, 1, cnt, selected);
		break;
	case KEY_CTRL('K'):
	case KEY_UP:
		selected = search_match(selected, BWD, 1, 1, selected);
		break;
	case KEY_CTRL('L'):
	case KEY_DOWN:
		selected = search_match(selected, FWD, 1, 1, selected);
		break;
	case 0x20 ... 0x7e:
		if (searchlen < sizeof(searchbuf) - 1)
			searchbuf[searchlen++] = c & 0xff;
		break;
	case KEY_DEL:
		if (searchlen) searchlen--;
		break;
	}

	return false;
}

void
search_cleanup(void)
{
	size_t i;

	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CLEAR_LINE "\n");
	for (i = 0; i < bwdctx + 1 + fwdctx; i++)
		eprintf(CSI_CUR_UP);
}

ssize_t
search_match(size_t start, int dir, bool new, size_t cnt, ssize_t fallback)
{
	return searchmodes[searchmode].match(start, dir, new, cnt, fallback);
}

ssize_t
search_match_substr(size_t start, int dir,
	bool new, size_t cnt, ssize_t fallback)
{
	const char *end, *bp;
	size_t i, found, len;
	ssize_t index;

	if (!searchlen) {
		index = start + dir * (new + cnt - 1);
		if (index < 0 || index >= entries_cnt)
			return fallback;
		return index;
	}

	found = 0;
	for (i = new; i < entries_cnt; i++) {
		index = start + dir * i;
		if (index < 0 || index >= entries_cnt)
			break;

		entry = read_entry(entry, index);
		end = entry + entry_len(index);

		for (bp = entry; *bp; bp++) {
			len = MIN(end - bp, searchlen);
			if (!search_cmp(bp, searchbuf, len)) {
				if (++found == cnt)
					return index;
				break;
			}
		}
	}

	return fallback;
}

ssize_t
search_match_fuzzy(size_t start, int dir,
	bool new, size_t cnt, ssize_t fallback)
{
	const char *end, *pos, *c;
	size_t i, found;
	ssize_t index;

	if (!searchlen) {
		index = start + dir * (new + cnt - 1);
		if (index < 0 || index >= entries_cnt)
			return fallback;
		return index;
	}

	found = 0;
	for (i = new; i < entries_cnt; i++) {
		index = start + dir * i;
		if (index < 0 || index >= entries_cnt)
			break;

		entry = read_entry(entry, index);
		end = entry + entry_len(index);

		pos = entry;
		for (c = searchbuf; c - searchbuf < searchlen; c++) {
			pos = search_find(pos, *c, end - pos);
			if (!pos) break;
			pos++;
		}
		if (c == searchbuf + searchlen) {
			if (++found == cnt)
				return index;
		}
	}

	return fallback;
}

void
load_entries(const char *filepath)
{
	size_t pos, start;
	ssize_t nread;
	char iobuf[1024];

	entries_cnt = 0;
	entries_cap = 100;
	entries = calloc(entries_cap, sizeof(size_t));
	if (!entries) err(1, "alloc");

	if (!filepath) {
		infile = tmpfile();
		if (!infile) err(1, "tmpfile");

		start = pos = 0;
		while (true) {
			nread = freadln(iobuf, sizeof(iobuf), stdin);
			if (nread <= 0) break;

			pos += nread;
			if (fwrite(iobuf, 1, nread, infile) != nread)
				errx(1, "fwrite to tmpfile truncated");
			if (iobuf[nread - 1] == '\n') {
				add_entry(entries_cnt, start);
				entries_cnt++;
				start = pos;
			}
		}
		add_entry(entries_cnt, pos);

		fseek(infile, 0, SEEK_SET);

		if (!freopen("/dev/tty", "r", stdin))
			err(1, "freopen tty");
		if (fread(NULL, 0, 0, infile) < 0)
			err(1, "fread stdin");
	} else {
		infile = fopen(filepath, "r");
		if (!infile) err(1, "fopen %s", filepath);

		start = pos = 0;
		while (true) {
			nread = freadln(iobuf, sizeof(iobuf), infile);
			if (nread <= 0) break;

			pos += nread;
			if (iobuf[nread - 1] == '\n') {
				add_entry(entries_cnt, start);
				entries_cnt++;
				start = pos;
			}
		}
		add_entry(entries_cnt, pos);

		fseek(infile, 0, SEEK_SET);
	}
}

void
run(const char *filepath)
{
	struct termios prevterm, newterm = { 0 };
	struct winsize ws = { 0 };
	int c;

	load_entries(filepath);

	if (verbose)
		eprintf("Loaded %lu entries\n", entries_cnt);

	if (!entries_cnt) return;

	if (tcgetattr(fileno(stdin), &prevterm))
		err(1, "tcgetattr");

	cfmakeraw(&newterm);
	newterm.c_oflag |= ONLCR | OPOST;
	if (tcsetattr(fileno(stdin), TCSANOW, &newterm))
		err(1, "tcsetattr");

	eprintf(CSI_CUR_HIDE);

	selected = 0;
	searchlen = 0;
	do {
		if (ioctl(1, TIOCGWINSZ, &ws) != -1)
			termw = ws.ws_col;

		modes[mode].prompt();

		switch ((c = readkey(stdin))) {
		case KEY_CTRL('C'):
			goto exit;
		case KEY_CTRL('D'):
			if (!multiout) goto exit;
			break;
		case KEY_CTRL('S'):
			searchmode = SEARCH_SUBSTR;
			mode = MODE_SEARCH;
			break;
		case KEY_CTRL('F'):
			searchmode = SEARCH_FUZZY;
			mode = MODE_SEARCH;
			break;
		case KEY_CTRL('Q'):
		case KEY_CTRL('B'):
			mode = MODE_BROWSE;
			break;
		case KEY_CTRL('L'):
			eprintf(CSI_CLEAR_SCREEN CSI_CUR_GOTO, 0, 0);
			break;
		case KEY_CTRL('W'):
			searchlen = 0;
			break;
		case KEY_CTRL('J'):
		case '\r':
			entry = read_entry(entry, selected);
			modes[mode].cleanup();
			printf("%.*s\n", (int) entry_len(selected), entry);
			if (!multiout) goto exit;
			break;
		default:
			if (modes[mode].handlekey(c))
				goto exit;
			break;
		}
	} while (c >= 0);

exit:
	modes[mode].cleanup();

	eprintf(CSI_CUR_SHOW);

	tcsetattr(fileno(stdin), TCSANOW, &prevterm);

	fclose(infile);
}

int
parseopt(const char *flag, const char **args)
{
	char *end;

	if (flag[0] && flag[1]) {
		eprintf("Invalid flag: -%s\n", flag);
		exit(1);
	}

	switch (flag[0]) {
	case 'm':
		multiout = true;
		return 0;
	case 'v':
		verbose = true;
		return 0;
	case 'b':
		fwdctx = strtol(*args, &end, 10);
		if (end && *end) goto badint;
		return 1;
	case 'a':
		bwdctx = strtol(*args, &end, 10);
		if (end && *end) goto badint;
		return 1;
	case 'h':
		printf("%s\n", usage);
		exit(0);
	}

	return 0;

badint:
	eprintf("Invalid int: %s\n", *args);
	exit(1);
}

int
main(int argc, const char **argv)
{
	const char *filepath;
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	entry = NULL;
	entries = NULL;

	verbose = false;
	filepath = NULL;
	for (i = 1; i < argc; i++) {
		if (*argv[i] == '-') {
			i += parseopt(argv[i] + 1, &argv[i+1]);
		} else if (!filepath) {
			filepath = argv[i];
		} else {
			eprintf("Unexpected argument: %s\n", argv[i]);
			return 0;
		}
	}

	run(filepath);

	free(entries);
	free(entry);
}
