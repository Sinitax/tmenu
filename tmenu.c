#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <unistd.h>
#include <termios.h>

void* checkp(void *p);
void die(const char *fmtstr, ...);
char* aprintf(const char *fmtstr, ...);
void run();
int handleopt(const char *flag, const char **args);

const char *userfile = NULL;

enum {
	CODE_UP    = 0x100,
	CODE_DOWN  = 0x101,
	CODE_LEFT  = 0x102,
	CODE_RIGHT = 0x103,
};

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
addent(size_t linec, size_t pos, size_t **lines, size_t *linecap)
{
	(*lines)[linec] = pos;
	if (linec == *linecap - 1) {
		*linecap *= 2;
		*lines = checkp(realloc(*lines, *linecap));
	}
}

void
run()
{
	char *tok, buf[1024];
	struct termios prevterm, newterm;
	size_t pos, start, linesel, nread,
	       linec, linecap, *lines = NULL;
	FILE *f;
	int c, termw;

	if (tcgetattr(STDIN_FILENO, &prevterm))
		die("Failed to get terminal properies\n");

	linecap = 100;
	lines = checkp(calloc(linecap, sizeof(size_t)));

	if (!userfile) {
		if (!(f = tmpfile()))
			die("Failed to create temporary file\n");

		linec = start = pos = 0;
		while ((fgets(buf, sizeof(buf), stdin))) {
			if ((tok = memchr(buf, '\n', sizeof(buf))))
				addent(linec++, start, &lines, &linecap);
			nread = tok ? tok - buf + 1 : sizeof(buf);
			if (fwrite(buf, 1, nread, f) != nread)
				die("Writing to tmp file failed\n");
			pos += nread;
			if (tok) start = pos;
		}

		fseek(f, 0, SEEK_SET);
	} else {
		if (!(f = fopen(userfile, "r")))
			die("Failed to open file for reading: %s\n", userfile);

		linec = start = pos = 0;
		while ((fgets(buf, sizeof(buf), f))) {
			if ((tok = memchr(buf, '\n', sizeof(buf))))
				addent(linec++, start, &lines, &linecap);
			pos += tok ? tok - buf + 1 : sizeof(buf) - 1;
			if (tok) start = pos;
		}

		fseek(f, 0, SEEK_SET);
	}

	if (!linec) return;
	printf("LINES: %li\n", linec);

	cfmakeraw(&newterm);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &newterm))
		die("Failed to set new terminal properties\n");

	linesel = 0;
	do {
		/* TODO: render lines */
		termw = 80;
		fseek(f, lines[linesel], SEEK_SET);
		printf("%li %li\n\r", linesel, lines[linesel]);
		fgets(buf, termw, f);
		if (buf[termw-1] == '\n') buf[termw-1] = '\0';
		printf("%s\n\r", buf);

		c = getc(stdin);
		if (c == 0x1b) c = readcode(stdin);
		printf("CODE: %i\n\r", c);
		switch (c) {
		case 0x03: /* CTRL+C */
			goto exit;
		case '\r':
			fseek(f, lines[linesel], SEEK_SET);
			while ((fgets(buf, sizeof(buf), f))
					&& buf[sizeof(buf)-1] != '\n')
				printf("%s", buf);
			goto exit;
		case CODE_UP:
			if (linesel != 0) linesel--;
			break;
		case CODE_DOWN:
			if (linesel != linec - 1) linesel++;
			break;
		}
	} while (c >= 0);

exit:
	tcsetattr(STDIN_FILENO, TCSANOW, &prevterm);

	fclose(f);
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
