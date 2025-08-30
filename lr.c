/* lr - a better recursive ls/find */

/*
 * Copyright (C) 2015-2025 Leah Neukirchen <purl.org/net/chneukirchen>
 * Parts of code derived from musl libc, which is
 * Copyright (C) 2005-2014 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
##% gcc -Os -Wall -g -o $STEM $FILE -Wno-switch -Wextra -Wwrite-strings
*/

#define _GNU_SOURCE

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#include <sys/xattr.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <paths.h>
#include <pwd.h>
#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __has_include
  #if __has_include(<stdnoreturn.h>)
    #include <stdnoreturn.h>
  #else
    #define noreturn /**/
  #endif
#else
  #define noreturn /**/
#endif

/* For Solaris. */
#if !defined(FNM_CASEFOLD) && defined(FNM_IGNORECASE)
#define FNM_CASEFOLD FNM_IGNORECASE
#endif

/* For Hurd. */
#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

struct fitree;
struct idtree;

static int Bflag;
static int Cflag;
static char *Cflags[64];
static int Gflag;
static int Dflag;
static int Hflag;
static int Lflag;
static int Qflag;
static int Pflag;
static int Uflag;
static int Wflag;
static int Xflag;
static int hflag;
static int lflag;
static int sflag;
static int qflag;
static int xflag;
static char Tflag = 'T';

#define COLOR_DEFAULT -1
#define COLOR_HIDDEN -2

static char *argv0;
static char *format;
static char *ordering;
static struct expr *expr;
static int prune;
static size_t prefixl;
static char input_delim = '\n';
static int current_color;
static int status;
static char *basepath;
static char host[1024];

static char default_ordering[] = "n";
static char default_format[] = "%p\\n";
static char type_format[] = "%p%F\\n";
static char long_format[] = "%M%x %n %u %g %s %\324F %\324R %p%F%l\n";
static char zero_format[] = "%p\\0";
static char stat_format[] = "%D %i %M %n %u %g %R %s \"%Ab %Ad %AT %AY\" \"%Tb %Td %TT %TY\" \"%Cb %Cd %CT %CY\" %b %p\n";

static struct fitree *root;
static struct fitree *new_root;

static struct idtree *users;
static struct idtree *groups;
static struct idtree *filesystems;
static int scanned_filesystems;

static int need_stat;
static int need_group;
static int need_user;
static int need_fstype;
static int need_xattr;

static dev_t maxdev;
static ino_t maxino;
static nlink_t maxnlink;
static uid_t maxuid;
static gid_t maxgid;
static dev_t maxrdev;
static off_t maxsize;
static blkcnt_t maxblocks;
static unsigned int maxxattr;

static int bflag_depth;
static int maxdepth;
static int uwid, gwid, fwid;

static time_t now;
static mode_t default_mask;

/* [^a-zA-Z0-9~._/-] */
static char rfc3986[128] = {
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,
	1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,
	1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,1
};

struct fileinfo {
	char *fpath;
	size_t prefixl;
	int depth;
	ino_t entries;
	struct stat sb;
	off_t total;
	char xattr[4];
	int color;
};

enum op {
	EXPR_OR = 1,
	EXPR_AND,
	EXPR_COND,
	EXPR_NOT,
	EXPR_LT,
	EXPR_LE,
	EXPR_EQ,
	EXPR_NEQ,
	EXPR_GE,
	EXPR_GT,
	EXPR_STREQ,
	EXPR_STREQI,
	EXPR_GLOB,
	EXPR_GLOBI,
	EXPR_REGEX,
	EXPR_REGEXI,
	EXPR_PRUNE,
	EXPR_PRINT,
	EXPR_COLOR,
	EXPR_TYPE,
	EXPR_ALLSET,
	EXPR_ANYSET,
	EXPR_CHMOD,
};

enum prop {
	PROP_ATIME = 1,
	PROP_CTIME,
	PROP_DEPTH,
	PROP_DEV,
	PROP_ENTRIES,
	PROP_FSTYPE,
	PROP_GID,
	PROP_GROUP,
	PROP_INODE,
	PROP_LINKS,
	PROP_MODE,
	PROP_MTIME,
	PROP_NAME,
	PROP_PATH,
	PROP_RDEV,
	PROP_SIZE,
	PROP_TARGET,
	PROP_TOTAL,
	PROP_UID,
	PROP_USER,
	PROP_XATTR,
};

enum filetype {
	TYPE_BLOCK = 'b',
	TYPE_CHAR = 'c',
	TYPE_DIR = 'd',
	TYPE_FIFO = 'p',
	TYPE_REGULAR = 'f',
	TYPE_SOCKET = 's',
	TYPE_SYMLINK = 'l',
};

struct expr {
	enum op op;
	union {
		enum prop prop;
		enum filetype filetype;
		struct expr *expr;
		char *string;
		int64_t num;
		regex_t *regex;
	} a, b, c;
};

static char *pos;

noreturn static void
parse_error(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s: parse error: ", argv0);
	vfprintf(stderr, msg, ap);
	fprintf(stderr, "\n");
	exit(2);
}

int
test_chmod(char *c, mode_t oldmode)
{
	mode_t newmode = oldmode;
	mode_t whom, what;
	char op;

	do {
		whom = 0;
		what = 0;

		while (1) {
			switch (*c) {
			case 'u': whom |= 04700; break;
			case 'g': whom |= 02070; break;
			case 'o': whom |= 01007; break;
			case 'a': whom |= 07777; break;
			default: goto op;
			}
			c++;
		}
op:
		if (whom == 0)
			whom = default_mask;
		op = *c++;
		if (!(op == '-' || op == '+' || op == '='))
			parse_error("invalid mode operator");

		switch (*c) {
		case 'u': what = 00111 * ((newmode >> 6) & 0007); c++; break;
		case 'g': what = 00111 * ((newmode >> 3) & 0007); c++; break;
		case 'o': what = 00111 * ((newmode     ) & 0007); c++; break;
		default:
			while (1) {
				switch (*c) {
				case 'r': what |= 00444; break;
				case 'w': what |= 00222; break;
				case 'x': what |= 00111; break;
				case 'X': if (oldmode & 00111) what |= 00111; break;
				case 's': what |= 06000; break;
				case 't': what |= 01000; break;
				case ',': case 0: goto doit;
				default: parse_error("invalid permission");
				}
				c++;
			}
		}
doit:
		switch (op) {
		case '-': newmode &= ~(whom & what); break;
		case '+': newmode |= (whom & what); break;
		case '=': newmode = (newmode & ~whom) | (whom & what); break;
		}
	} while (*c == ',' && c++);

	if (*c)
		parse_error("trailing garbage in mode string '%s'", c);

	return newmode == oldmode;
}

static struct expr *
mkexpr(enum op op)
{
	struct expr *e = malloc(sizeof (struct expr));
	if (!e)
		parse_error("out of memory");
	e->op = op;
	return e;
}

static void
ws()
{
	while (isspace((unsigned char)*pos))
		pos++;
}

static int
token(const char *token)
{
	if (strncmp(pos, token, strlen(token)) == 0) {
		pos += strlen(token);
		ws();
		return 1;
	} else {
		return 0;
	}
}

static int64_t
parse_num(int64_t *r)
{
	char *s = pos;
	if (isdigit((unsigned char)*pos)) {
		int64_t n;

		for (n = 0; isdigit((unsigned char)*pos) && n <= INT64_MAX / 10 - 10; pos++)
			n = 10 * n + (*pos - '0');
		if (isdigit((unsigned char)*pos))
			parse_error("number too big: %s", s);
		if (token("c"))      ;
		else if (token("b")) n *= 512LL;
		else if (token("k")) n *= 1024LL;
		else if (token("M")) n *= 1024LL * 1024;
		else if (token("G")) n *= 1024LL * 1024 * 1024;
		else if (token("T")) n *= 1024LL * 1024 * 1024 * 1024;
		ws();
		*r = n;
		return 1;
	} else {
		return 0;
	}
}

int
parse_octal(long *r)
{
	char *s = pos;
	if (*pos >= '0' && *pos <= '7') {
		long n = 0;
		while (*pos >= '0' && *pos <= '7') {
			n *= 8;
			n += *pos - '0';
			pos++;
			if (n > 07777)
				parse_error("number to big: %s", s);
		}
		ws();
		*r = n;
		return 1;
	} else {
		return 0;
	}
}

static enum op
parse_op()
{
	if (token("<="))
		return EXPR_LE;
	else if (token("<"))
		return EXPR_LT;
	else if (token(">="))
		return EXPR_GE;
	else if (token(">"))
		return EXPR_GT;
	else if (token("==") || token("="))
		return EXPR_EQ;
	else if (token("!="))
		return EXPR_NEQ;

	return 0;
}

static struct expr *parse_cmp();
static struct expr *parse_cond();

static struct expr *
parse_inner()
{
	if (token("prune")) {
		struct expr *e = mkexpr(EXPR_PRUNE);
		return e;
	} else if (token("print")) {
		struct expr *e = mkexpr(EXPR_PRINT);
		return e;
	} else if (token("skip")) {
		struct expr *e = mkexpr(EXPR_PRINT);
		struct expr *not = mkexpr(EXPR_NOT);
		not->a.expr = e;
		return not;
	} else if (token("color")) {
		struct expr *e = mkexpr(EXPR_COLOR);
		int64_t n;
		if (parse_num(&n) && n >= 0 && n <= 255) {
			e->a.num = n;
			return e;
		} else {
			parse_error("invalid 256-color at '%.15s'", pos);
			return 0;
		}
	} else if (token("!")) {
		struct expr *e = parse_cmp();
		struct expr *not = mkexpr(EXPR_NOT);
		not->a.expr = e;
		return not;
	} else if (token("(")) {
		struct expr *e = parse_cond();
		if (token(")"))
			return e;
		parse_error("missing ) at '%.15s'", pos);
		return 0;
	} else {
		parse_error("unknown expression at '%.15s'", pos);
		return 0;
	}
}

static struct expr *
parse_type()
{
	int negate = 0;

	if (token("type")) {
		if (token("==") || token("=")
		    || (token("!=") && ++negate)) {
			struct expr *e = mkexpr(EXPR_TYPE);
			if (token("b"))
				e->a.filetype = TYPE_BLOCK;
			else if (token("c"))
				e->a.filetype = TYPE_CHAR;
			else if (token("d"))
				e->a.filetype = TYPE_DIR;
			else if (token("p"))
				e->a.filetype = TYPE_FIFO;
			else if (token("f"))
				e->a.filetype = TYPE_REGULAR;
			else if (token("l"))
				e->a.filetype = TYPE_SYMLINK;
			else if (token("s"))
				e->a.filetype = TYPE_SOCKET;
			else if (*pos)
				parse_error("invalid file type '%c'", *pos);
			else
				parse_error("no file type given");
			if (negate) {
				struct expr *not = mkexpr(EXPR_NOT);
				not->a.expr = e;
				return not;
			} else {
				return e;
			}
		} else {
			parse_error("invalid file type comparison at '%.15s'",
			    pos);
		}
	}

	return parse_inner();
}

static int
parse_string(char **s)
{
	char *buf = 0;
	size_t bufsiz = 0;
	size_t len = 0;

	if (*pos == '"') {
		pos++;
		while (*pos &&
		    (*pos != '"' || (*pos == '"' && *(pos+1) == '"'))) {
			if (len + 1 >= bufsiz) {
				bufsiz = 2*bufsiz + 16;
				buf = realloc(buf, bufsiz);
				if (!buf)
					parse_error("string too long");
			}
			if (*pos == '"')
				pos++;
			buf[len++] = *pos++;
		}
		if (!*pos)
			parse_error("unterminated string");
		if (buf)
			buf[len] = 0;
		pos++;
		ws();
		*s = buf ? buf : (char *)"";
		return 1;
	} else if (*pos == '$') {
		char t;
		char *e = ++pos;

		while (isalnum((unsigned char)*pos) || *pos == '_')
			pos++;
		if (e == pos)
			parse_error("invalid environment variable name");

		t = *pos;
		*pos = 0;
		*s = getenv(e);
		if (!*s)
			*s = (char *)"";
		*pos = t;
		ws();
		return 1;
	}

	return 0;
}

static int
parse_dur(int64_t *n)
{
	char *s, *r;
	if (!parse_string(&s))
		return 0;

	if (*s == '/' || *s == '.') {
		struct stat st;
		if (stat(s, &st) < 0)
			parse_error("can't stat file '%s': %s",
			    s, strerror(errno));
		*n = st.st_mtime;
		return 1;
	}

	struct tm tm = { 0 };
	r = strptime(s, "%Y-%m-%d %H:%M:%S", &tm);
	if (r && !*r) {
		*n = mktime(&tm);
		return 1;
	}
	r = strptime(s, "%Y-%m-%d", &tm);
	if (r && !*r) {
		tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
		*n = mktime(&tm);
		return 1;
	}
	r = strptime(s, "%H:%M:%S", &tm);
	if (r && !*r) {
		struct tm *tmnow = localtime(&now);
		tm.tm_year = tmnow->tm_year;
		tm.tm_mon = tmnow->tm_mon;
		tm.tm_mday = tmnow->tm_mday;
		*n = mktime(&tm);
		return 1;
	}
	r = strptime(s, "%H:%M", &tm);
	if (r && !*r) {
		struct tm *tmnow = localtime(&now);
		tm.tm_year = tmnow->tm_year;
		tm.tm_mon = tmnow->tm_mon;
		tm.tm_mday = tmnow->tm_mday;
		tm.tm_sec = 0;
		*n = mktime(&tm);
		return 1;
	}

	if (*s == '-') {
		s++;

		errno = 0;
		int64_t d;
		d = strtol(s, &r, 10);
		if (errno == 0 && r[0] == 'd' && !r[1]) {
			struct tm *tmnow = localtime(&now);
			tmnow->tm_mday -= d;
			tmnow->tm_hour = tmnow->tm_min = tmnow->tm_sec = 0;
			*n = mktime(tmnow);
			return 1;
		}
		if (errno == 0 && r[0] == 'h' && !r[1]) {
			*n = now - (d*60*60);
			return 1;
		}
		if (errno == 0 && r[0] == 'm' && !r[1]) {
			*n = now - (d*60);
			return 1;
		}
		if (errno == 0 && r[0] == 's' && !r[1]) {
			*n = now - d;
			return 1;
		}
		parse_error("invalid relative time format '%s'", s-1);
	}

	parse_error("invalid time format '%s'", s);
	return 0;
}

static struct expr *
mkstrexpr(enum prop prop, enum op op, char *s, int negate)
{
	int r = 0;
	struct expr *e = mkexpr(op);
	e->a.prop = prop;
	if (op == EXPR_REGEX) {
		e->b.regex = malloc(sizeof (regex_t));
		r = regcomp(e->b.regex, s, REG_EXTENDED | REG_NOSUB);
	} else if (op == EXPR_REGEXI) {
		e->b.regex = malloc(sizeof (regex_t));
		r = regcomp(e->b.regex, s, REG_EXTENDED | REG_NOSUB | REG_ICASE);
	} else {
		e->b.string = s;
	}

	if (r != 0) {
		char msg[256];
		regerror(r, e->b.regex, msg, sizeof msg);
		parse_error("invalid regex '%s': %s", s, msg);
		exit(2);
	}

	if (negate) {
		struct expr *not = mkexpr(EXPR_NOT);
		not->a.expr = e;
		return not;
	}

	return e;
}

static struct expr *
parse_strcmp()
{
	enum prop prop;
	enum op op;
	int negate = 0;

	if (token("fstype"))
		prop = PROP_FSTYPE;
	else if (token("group"))
		prop = PROP_GROUP;
	else if (token("name"))
		prop = PROP_NAME;
	else if (token("path"))
		prop = PROP_PATH;
	else if (token("target"))
		prop = PROP_TARGET;
	else if (token("user"))
		prop = PROP_USER;
	else if (token("xattr"))
		prop = PROP_XATTR;
	else
		return parse_type();

	if (token("~~~"))
		op = EXPR_GLOBI;
	else if (token("~~"))
		op = EXPR_GLOB;
	else if (token("=~~"))
		op = EXPR_REGEXI;
	else if (token("=~"))
		op = EXPR_REGEX;
	else if (token("==="))
		op = EXPR_STREQI;
	else if (token("=="))
		op = EXPR_STREQ;
	else if (token("="))
		op = EXPR_STREQ;
	else if (token("!~~~"))
		negate = 1, op = EXPR_GLOBI;
	else if (token("!~~"))
		negate = 1, op = EXPR_GLOB;
	else if (token("!=~~"))
		negate = 1, op = EXPR_REGEXI;
	else if (token("!=~") || token("!~"))
		negate = 1, op = EXPR_REGEX;
	else if (token("!==="))
		negate = 1, op = EXPR_STREQI;
	else if (token("!==") || token("!="))
		negate = 1, op = EXPR_STREQ;
	else
		parse_error("invalid string operator at '%.15s'", pos);

	char *s;
	if (parse_string(&s))
		return mkstrexpr(prop, op, s, negate);

	parse_error("invalid string at '%.15s'", pos);
	return 0;
}

static struct expr *
parse_mode()
{
	struct expr *e = mkexpr(0);
	long n;
	char *s;

	e->a.prop = PROP_MODE;

	if (token("==") || token("=")) {
		e->op = EXPR_EQ;
	} else if (token("&")) {
		e->op = EXPR_ALLSET;
	} else if (token("|")) {
		e->op = EXPR_ANYSET;
	} else {
		parse_error("invalid mode comparison at '%.15s'", pos);
	}

	if (parse_octal(&n)) {
		e->b.num = n;
	} else if (e->op == EXPR_EQ && parse_string(&s)) {
		e->op = EXPR_CHMOD;
		e->b.string = s;
		umask(default_mask = 07777 & ~umask(0));  /* for future usage */
		test_chmod(s, 0);  /* run once to check for syntax */
	} else {
		parse_error("invalid mode at '%.15s'", pos);
	}

	return e;
}

static struct expr *
parse_cmp()
{
	enum prop prop;
	enum op op;

	if (token("depth"))
		prop = PROP_DEPTH;
	else if (token("dev"))
		prop = PROP_DEV;
	else if (token("entries"))
		prop = PROP_ENTRIES;
	else if (token("gid"))
		prop = PROP_GID;
	else if (token("inode"))
		prop = PROP_INODE;
	else if (token("links"))
		prop = PROP_LINKS;
	else if (token("mode"))
		return parse_mode();
	else if (token("rdev"))
		prop = PROP_RDEV;
	else if (token("size"))
		prop = PROP_SIZE;
	else if (token("total"))
		prop = PROP_TOTAL;
	else if (token("uid"))
		prop = PROP_UID;
	else
		return parse_strcmp();

	op = parse_op();
	if (!op)
		parse_error("invalid comparison at '%.15s'", pos);

	int64_t n;
	if (parse_num(&n)) {
		struct expr *e = mkexpr(op);
		e->a.prop = prop;
		e->b.num = n;
		return e;
	}

	return 0;
}

static struct expr *
parse_timecmp()
{
	enum prop prop;
	enum op op;

	if (token("atime"))
		prop = PROP_ATIME;
	else if (token("ctime"))
		prop = PROP_CTIME;
	else if (token("mtime"))
		prop = PROP_MTIME;
	else
		return parse_cmp();

	op = parse_op();
	if (!op)
		parse_error("invalid comparison at '%.15s'", pos);

	int64_t n;
	if (parse_num(&n) || parse_dur(&n)) {
		struct expr *e = mkexpr(op);
		e->a.prop = prop;
		e->b.num = n;
		return e;
	}

	return 0;
}

static struct expr *
chain(struct expr *e1, enum op op, struct expr *e2)
{
	struct expr *i, *j, *e;
	if (!e1)
		return e2;
	if (!e2)
		return e1;
	for (j = 0, i = e1; i->op == op; j = i, i = i->b.expr)
		;
	if (!j) {
		e = mkexpr(op);
		e->a.expr = e1;
		e->b.expr = e2;
		return e;
	} else {
		e = mkexpr(op);
		e->a.expr = i;
		e->b.expr = e2;
		j->b.expr = e;
		return e1;
	}
}

static struct expr *
parse_and()
{
	struct expr *e1 = parse_timecmp();
	struct expr *r = e1;

	while (token("&&")) {
		struct expr *e2 = parse_timecmp();
		r = chain(r, EXPR_AND, e2);
	}

	return r;
}


static struct expr *
parse_or()
{
	struct expr *e1 = parse_and();
	struct expr *r = e1;

	while (token("||")) {
		struct expr *e2 = parse_and();
		r = chain(r, EXPR_OR, e2);
	}

	return r;
}

static struct expr *
parse_cond()
{
	struct expr *e1 = parse_or();

	if (token("?")) {
		struct expr *e2 = parse_or();
		if (token(":")) {
			struct expr *e3 = parse_cond();
			struct expr *r = mkexpr(EXPR_COND);
			r->a.expr = e1;
			r->b.expr = e2;
			r->c.expr = e3;

			return r;
		} else {
			parse_error(": expected at '%.15s'", pos);
		}
	}

	return e1;
}

static struct expr *
parse_expr(const char *s)
{
	pos = (char *)s;
	ws();
	struct expr *e = parse_cond();
	if (*pos)
		parse_error("trailing garbage at '%.15s'", pos);
	return e;
}

static const char *
basenam(const char *s)
{
	char *r = strrchr(s, '/');
	return r ? r + 1 : s;
}

static const char *
extnam(const char *s)
{
	char *r = strrchr(s, '/');
	char *e = strrchr(s, '.');
	if (!r || r < e)
		return e ? e + 1 : "";
	return "";
}

static const char *
readlin(const char *p, const char *alt)
{
	static char b[PATH_MAX];
	ssize_t r = readlink(p, b, sizeof b - 1);
	if (r < 0 || (size_t)r >= sizeof b - 1)
		return alt;
	b[r] = 0;
	return b;
}

/* AA-tree implementation, adapted from https://github.com/ccxvii/minilibs */
struct idtree {
	long id;
	char *name;
	struct idtree *left, *right;
	int level;
};

static struct idtree idtree_sentinel = { 0, 0, &idtree_sentinel, &idtree_sentinel, 0 };

static struct idtree *
idtree_make(long id, char *name)
{
	struct idtree *node = malloc(sizeof (struct idtree));
	node->id = id;
	node->name = name;
	node->left = node->right = &idtree_sentinel;
	node->level = 1;
	return node;
}

char *
idtree_lookup(struct idtree *node, long id)
{
	if (node) {
		while (node != &idtree_sentinel) {
			if (id == node->id)
				return node->name;
			else if (id < node->id)
				node = node->left;
			else
				node = node->right;
		}
	}

	return 0;
}

static struct idtree *
idtree_skew(struct idtree *node)
{
	if (node->left->level == node->level) {
		struct idtree *save = node;
		node = node->left;
		save->left = node->right;
		node->right = save;
	}
	return node;
}

static struct idtree *
idtree_split(struct idtree *node)
{
	if (node->right->right->level == node->level) {
		struct idtree *save = node;
		node = node->right;
		save->right = node->left;
		node->left = save;
		node->level++;
	}
	return node;
}

struct idtree *
idtree_insert(struct idtree *node, long id, char *name)
{
	if (node && node != &idtree_sentinel) {
		if (id == node->id)
			return node;
		else if (id < node->id)
			node->left = idtree_insert(node->left, id, name);
		else
			node->right = idtree_insert(node->right, id, name);
		node = idtree_skew(node);
		node = idtree_split(node);
		return node;
	}
	return idtree_make(id, name);
}
/**/

static char *
strid(long id)
{
	static char buf[32];
	snprintf(buf, sizeof buf, "%ld", id);
	return buf;
}

static char *
groupname(gid_t gid)
{
	char *name = idtree_lookup(groups, gid);

	if (name)
		return name;

	struct group *g = getgrgid(gid);
	if (g) {
		if ((int)strlen(g->gr_name) > gwid)
			gwid = strlen(g->gr_name);
		char *name = strdup(g->gr_name);
		groups = idtree_insert(groups, gid, name);
		return name;
	}

	return strid(gid);
}

static char *
username(uid_t uid)
{
	char *name = idtree_lookup(users, uid);

	if (name)
		return name;

	struct passwd *p = getpwuid(uid);
	if (p) {
		if ((int)strlen(p->pw_name) > uwid)
			uwid = strlen(p->pw_name);
		char *name = strdup(p->pw_name);
		users = idtree_insert(users, uid, name);
		return name;
	}

	return strid(uid);
}

#if defined(__linux__) || defined(__CYGWIN__)
#include <mntent.h>
void
scan_filesystems()
{
	FILE *mtab;
	struct mntent *mnt;
	struct stat st;

	/* Approach: iterate over mtab and memorize st_dev for each mountpoint.
	 * this will fail if we are not allowed to read the mountpoint, but then
	 * we should not have to look up this st_dev value... */
	mtab = setmntent(_PATH_MOUNTED, "r");
	if (!mtab && errno == ENOENT)
		mtab = setmntent("/proc/mounts", "r");
	if (!mtab)
		return;

	while ((mnt = getmntent(mtab))) {
		if (stat(mnt->mnt_dir, &st) < 0)
			continue;
		filesystems = idtree_insert(filesystems,
		    st.st_dev, strdup(mnt->mnt_type));
	};

	endmntent(mtab);

	scanned_filesystems = 1;
}
#elif defined(__SVR4)
#include <sys/mnttab.h>
void
scan_filesystems()
{
	FILE *mtab;
	struct mnttab mnt;
	struct stat st;

	mtab = fopen(MNTTAB, "r");
	if (!mtab)
		return;

	while (getmntent(mtab, &mnt) == 0) {
		if (stat(mnt.mnt_mountp, &st) < 0)
			continue;
		filesystems = idtree_insert(filesystems,
		    st.st_dev, strdup(mnt->mnt_fstype));
	};

	fclose(mtab);

	scanned_filesystems = 1;
}
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
void
scan_filesystems()
{
	struct statfs *mnt;
	struct stat st;
	int i = getmntinfo(&mnt, MNT_NOWAIT);

	while (i-- > 0) {
		if (stat(mnt->f_mntonname, &st) < 0)
			continue;
		filesystems = idtree_insert(filesystems,
		    st.st_dev, strdup(mnt->f_fstypename));
		mnt++;
	};

	scanned_filesystems = 1;
}
#elif defined(__NetBSD__)
#include <sys/statvfs.h>
#include <sys/types.h>
void
scan_filesystems()
{
	struct statvfs *mnt;
	struct stat st;
	int i = getmntinfo(&mnt, MNT_NOWAIT);

	while (i-- > 0) {
		if (stat(mnt->f_mntonname, &st) < 0)
			continue;
		filesystems = idtree_insert(filesystems,
		    st.st_dev, strdup(mnt->f_fstypename));
		mnt++;
	};

	scanned_filesystems = 1;
}
#else
#warning fstype lookup not implemented on this platform, keeping st_dev as number
void
scan_filesystems()
{
}
#endif

static char *
fstype(dev_t devid)
{
	if (!scanned_filesystems)
		scan_filesystems();

	char *name = idtree_lookup(filesystems, devid);
	if (name) {
		if ((int)strlen(name) > fwid)
			fwid = strlen(name);
		return name;
	}

	return strid(devid);
}

static char *
xattr_string(const char *f)
{
#ifdef __linux__
	char xattr[1024];
	int i, r;
	int have_xattr = 0, have_cap = 0, have_acl = 0;

	if (Lflag)
		r = listxattr(f, xattr, sizeof xattr);
	else
		r = llistxattr(f, xattr, sizeof xattr);
	if (r < 0 && errno == ERANGE) {
		/* just look at prefix */
		r = sizeof xattr;
		xattr[r-1] = 0;
	}
	/* ignoring ENOTSUP or r == 0 */

	for (i = 0; i < r; i += strlen(xattr+i) + 1)
		if (strcmp(xattr+i, "security.capability") == 0)
			have_cap = 1;
		else if (strcmp(xattr+i, "system.posix_acl_access") == 0 ||
		    strcmp(xattr+i, "system.posix_acl_default") == 0)
			have_acl = 1;
		else
			have_xattr = 1;

	static char buf[4];
	char *c = buf;
	if (have_cap)
		*c++ = '#';
	if (have_acl)
		*c++ = '+';
	if (have_xattr)
		*c++ = '@';
	*c = 0;

	return buf;
#else
	static char empty[] = "";
	(void)f;
	return empty;           /* No support for xattrs on this platform. */
#endif
}

static ino_t
count_entries(struct fileinfo *fi)
{
	ino_t c = 0;
	struct dirent *de;
	DIR *d;

	if (Dflag)
		return fi->entries;

	if (!S_ISDIR(fi->sb.st_mode))
		return 0;

	d = opendir(fi->fpath[0] ? fi->fpath : ".");
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' &&
		    (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
			continue;
		c++;
	}
	closedir(d);

	return c;
}

int
eval(struct expr *e, struct fileinfo *fi)
{
	long v = 0;
	const char *s = "";

	switch (e->op) {
	case EXPR_OR:
		return eval(e->a.expr, fi) || eval(e->b.expr, fi);
	case EXPR_AND:
		return eval(e->a.expr, fi) && eval(e->b.expr, fi);
	case EXPR_COND:
		return eval(e->a.expr, fi)
		    ? eval(e->b.expr, fi)
		    : eval(e->c.expr, fi);
	case EXPR_NOT:
		return !eval(e->a.expr, fi);
	case EXPR_PRUNE:
		prune = 1;
		return 1;
	case EXPR_PRINT:
		return 1;
	case EXPR_COLOR:
		fi->color = e->a.num;
		return 1;
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_EQ:
	case EXPR_NEQ:
	case EXPR_GE:
	case EXPR_GT:
	case EXPR_ALLSET:
	case EXPR_ANYSET:
		switch (e->a.prop) {
		case PROP_ATIME: v = fi->sb.st_atime; break;
		case PROP_CTIME: v = fi->sb.st_ctime; break;
		case PROP_DEPTH: v = fi->depth; break;
		case PROP_DEV: v = fi->sb.st_dev; break;
		case PROP_ENTRIES: v = count_entries(fi); break;
		case PROP_GID: v = fi->sb.st_gid; break;
		case PROP_INODE: v = fi->sb.st_ino; break;
		case PROP_LINKS: v = fi->sb.st_nlink; break;
		case PROP_MODE: v = fi->sb.st_mode & 07777; break;
		case PROP_MTIME: v = fi->sb.st_mtime; break;
		case PROP_RDEV: v = fi->sb.st_rdev; break;
		case PROP_SIZE: v = fi->sb.st_size; break;
		case PROP_TOTAL: v = fi->total; break;
		case PROP_UID: v = fi->sb.st_uid; break;
		default: parse_error("unknown property");
		}
		switch (e->op) {
		case EXPR_LT: return v < e->b.num;
		case EXPR_LE: return v <= e->b.num;
		case EXPR_EQ: return v == e->b.num;
		case EXPR_NEQ: return v != e->b.num;
		case EXPR_GE: return v >= e->b.num;
		case EXPR_GT: return v > e->b.num;
		case EXPR_ALLSET: return (v & e->b.num) == e->b.num;
		case EXPR_ANYSET: return (v & e->b.num) > 0;
		default: parse_error("invalid operator");
		}
	case EXPR_CHMOD:
		return test_chmod(e->b.string, fi->sb.st_mode & 07777);
	case EXPR_TYPE:
		switch (e->a.filetype) {
		case TYPE_BLOCK: return S_ISBLK(fi->sb.st_mode);
		case TYPE_CHAR: return S_ISCHR(fi->sb.st_mode);
		case TYPE_DIR: return S_ISDIR(fi->sb.st_mode);
		case TYPE_FIFO: return S_ISFIFO(fi->sb.st_mode);
		case TYPE_REGULAR: return S_ISREG(fi->sb.st_mode);
		case TYPE_SOCKET: return S_ISSOCK(fi->sb.st_mode);
		case TYPE_SYMLINK: return S_ISLNK(fi->sb.st_mode);
		default: parse_error("invalid file type");
		}
	case EXPR_STREQ:
	case EXPR_STREQI:
	case EXPR_GLOB:
	case EXPR_GLOBI:
	case EXPR_REGEX:
	case EXPR_REGEXI:
		switch (e->a.prop) {
		case PROP_FSTYPE: s = fstype(fi->sb.st_dev); break;
		case PROP_GROUP: s = groupname(fi->sb.st_gid); break;
		case PROP_NAME: s = basenam(fi->fpath); break;
		case PROP_PATH: s = fi->fpath; break;
		case PROP_TARGET: s = readlin(fi->fpath, ""); break;
		case PROP_USER: s = username(fi->sb.st_uid); break;
		case PROP_XATTR: s = xattr_string(fi->fpath); break;
		default: parse_error("unknown property");
		}
		switch (e->op) {
		case EXPR_STREQ: return strcmp(e->b.string, s) == 0;
		case EXPR_STREQI: return strcasecmp(e->b.string, s) == 0;
		case EXPR_GLOB: return fnmatch(e->b.string, s, 0) == 0;
		case EXPR_GLOBI: return fnmatch(e->b.string, s, FNM_CASEFOLD) == 0;
		case EXPR_REGEX:
		case EXPR_REGEXI: return regexec(e->b.regex, s, 0, 0, 0) == 0;
		default: parse_error("invalid operator");
		}
	default:
		parse_error("invalid operation %d, please file a bug.", e->op);
	}

	return 0;
}

int
dircmp(char *a, char *b)
{
	char *ea = strrchr(a, '/');
	char *eb = strrchr(b, '/');
	if (!ea)
		ea = a + strlen(a);
	if (!eb)
		eb = b + strlen(b);

	while (a != ea && b != eb && *a == *b) {
		a++;
		b++;
	}

	if (a == ea && b == eb)
		return 0;
	if (a == ea)
		return -1;
	if (b == eb)
		return 1;
	return *a - *b;
}

/* taken straight from musl@a593414 */
int mystrverscmp(const char *l0, const char *r0)
{
	const unsigned char *l = (const void *)l0;
	const unsigned char *r = (const void *)r0;
	size_t i, dp, j;
	int z = 1;

	/* Find maximal matching prefix and track its maximal digit
	 * suffix and whether those digits are all zeros. */
	for (dp = i = 0; l[i] == r[i]; i++) {
		int c = l[i];
		if (!c) return 0;
		if (!isdigit(c)) dp = i+1, z = 1;
		else if (c != '0') z = 0;
	}

	if (l[dp] != '0' && r[dp] != '0') {
		/* If we're not looking at a digit sequence that began
		 * with a zero, longest digit string is greater. */
		for (j = i; isdigit(l[j]); j++)
			if (!isdigit(r[j])) return 1;
		if (isdigit(r[j])) return -1;
	} else if (z && dp < i && (isdigit(l[i]) || isdigit(r[i]))) {
		/* Otherwise, if common prefix of digit sequence is
		 * all zeros, digits order less than non-digits. */
		return (unsigned char)(l[i]-'0') - (unsigned char)(r[i]-'0');
	}

	return l[i] - r[i];
}

#define CMP(a, b) if ((a) == (b)) break; else if ((a) < (b)) return -1; else return 1
#define STRCMP(a, b) if (strcmp(a, b) == 0) break; else return (strcmp(a, b));
#define DIRCMP(a, b) { int r = dircmp(a, b); if (r == 0) break; else return r; };
#define VERCMP(a, b) if (mystrverscmp(a, b) == 0) break; else return (mystrverscmp(a, b));

int
order(const void *a, const void *b)
{
	struct fileinfo *fa = (struct fileinfo *)a;
	struct fileinfo *fb = (struct fileinfo *)b;
	char *s;

	for (s = ordering; *s; s++) {
		switch (*s) {
		/* XXX use nanosecond timestamps */
		case 'c': CMP(fa->sb.st_ctime, fb->sb.st_ctime);
		case 'C': CMP(fb->sb.st_ctime, fa->sb.st_ctime);
		case 'a': CMP(fa->sb.st_atime, fb->sb.st_atime);
		case 'A': CMP(fb->sb.st_atime, fa->sb.st_atime);
		case 'm': CMP(fa->sb.st_mtime, fb->sb.st_mtime);
		case 'M': CMP(fb->sb.st_mtime, fa->sb.st_mtime);
		case 's': CMP(fa->sb.st_size, fb->sb.st_size);
		case 'S': CMP(fb->sb.st_size, fa->sb.st_size);
		case 'i': CMP(fa->sb.st_ino, fb->sb.st_ino);
		case 'I': CMP(fb->sb.st_ino, fa->sb.st_ino);
		case 'd': CMP(fa->depth, fb->depth);
		case 'D': CMP(fb->depth, fa->depth);
		case 't': CMP("ZZZZAZZZZZZZZZZZ"[(fa->sb.st_mode >> 12) & 0x0f],
		              "ZZZZAZZZZZZZZZZZ"[(fb->sb.st_mode >> 12) & 0x0f]);
		case 'T': CMP("ZZZZAZZZZZZZZZZZ"[(fb->sb.st_mode >> 12) & 0x0f],
		              "ZZZZAZZZZZZZZZZZ"[(fa->sb.st_mode >> 12) & 0x0f]);
		case 'n': STRCMP(fa->fpath, fb->fpath);
		case 'N': STRCMP(fb->fpath, fa->fpath);
		case 'f': STRCMP(basenam(fa->fpath), basenam(fb->fpath));
		case 'F': STRCMP(basenam(fb->fpath), basenam(fa->fpath));
		case 'e': STRCMP(extnam(fa->fpath), extnam(fb->fpath));
		case 'E': STRCMP(extnam(fb->fpath), extnam(fa->fpath));
		case 'p': DIRCMP(fa->fpath, fb->fpath);
		case 'P': DIRCMP(fb->fpath, fa->fpath);
		case 'v': VERCMP(fa->fpath, fb->fpath);
		case 'V': VERCMP(fb->fpath, fa->fpath);
		default: STRCMP(fa->fpath, fb->fpath);
		}
	}

	return strcmp(fa->fpath, fb->fpath);
}

static void
free_fi(struct fileinfo *fi)
{
	if (fi)
		free(fi->fpath);
	free(fi);
}

/* AA-tree implementation, adapted from https://github.com/ccxvii/minilibs */
struct fitree {
	struct fileinfo *fi;
	struct fitree *left, *right;
	int level;
};

static struct fitree fitree_sentinel = { 0, &fitree_sentinel, &fitree_sentinel, 0 };

static struct fitree *
fitree_make(struct fileinfo *fi)
{
	struct fitree *node = malloc(sizeof (struct fitree));
	node->fi = fi;
	node->left = node->right = &fitree_sentinel;
	node->level = 1;
	return node;
}

static struct fitree *
fitree_skew(struct fitree *node)
{
	if (node->left->level == node->level) {
		struct fitree *save = node;
		node = node->left;
		save->left = node->right;
		node->right = save;
	}
	return node;
}

static struct fitree *
fitree_split(struct fitree *node)
{
	if (node->right->right->level == node->level) {
		struct fitree *save = node;
		node = node->right;
		save->right = node->left;
		node->left = save;
		node->level++;
	}
	return node;
}

struct fitree *
fitree_insert(struct fitree *node, struct fileinfo *fi)
{
	if (node && node != &fitree_sentinel) {
		int c = order(fi, node->fi);
		if (c == 0)
			return node;
		if (c < 0)
			node->left = fitree_insert(node->left, fi);
		else
			node->right = fitree_insert(node->right, fi);
		node = fitree_skew(node);
		node = fitree_split(node);
		return node;
	}
	return fitree_make(fi);
}

void
fitree_walk(struct fitree *node, void (*visit)(struct fileinfo *))
{
	if (!node)
		return;
	if (node->left != &fitree_sentinel)
		fitree_walk(node->left, visit);
	visit(node->fi);
	if (node->right != &fitree_sentinel)
		fitree_walk(node->right, visit);
}

void
fitree_free(struct fitree *node)
{
	if (node && node != &fitree_sentinel) {
		fitree_free(node->left);
		fitree_free(node->right);
		free_fi(node->fi);
		free(node);
	}
}
/**/

static int
intlen(intmax_t i)
{
	int s;
	for (s = 1; i > 9; i /= 10)
		s++;
	return s;
}

static void
print_mode(int mode)
{
	putchar("0pcCd?bB-?l?s???"[(mode >> 12) & 0x0f]);
	putchar(mode & 00400 ? 'r' : '-');
	putchar(mode & 00200 ? 'w' : '-');
	putchar(mode & 04000 ? (mode & 00100 ? 's' : 'S')
	                     : (mode & 00100 ? 'x' : '-'));
	putchar(mode & 00040 ? 'r' : '-');
	putchar(mode & 00020 ? 'w' : '-');
	putchar(mode & 02000 ? (mode & 00010 ? 's' : 'S')
	                     : (mode & 00010 ? 'x' : '-'));
	putchar(mode & 00004 ? 'r' : '-');
	putchar(mode & 00002 ? 'w' : '-');
	putchar(mode & 01000 ? (mode & 00001 ? 't' : 'T')
	                     : (mode & 00001 ? 'x' : '-'));
}

static void
fgbold()
{
	printf("\033[1m");
}

static void
fg256(int c)
{
	printf("\033[38;5;%dm", c);
}

static void
fgdefault()
{
	if (Gflag)
		printf("\033[0m");
}

static void
color_size_on(off_t s)
{
	int c;

	if (!Gflag)
		return;

	if      (s <              1024LL) c = 46;
	else if (s <            4*1024LL) c = 82;
	else if (s <           16*1024LL) c = 118;
	else if (s <           32*1024LL) c = 154;
	else if (s <          128*1024LL) c = 190;
	else if (s <          512*1024LL) c = 226;
	else if (s <         1024*1024LL) c = 220;
	else if (s <     700*1024*1024LL) c = 214;
	else if (s <  2*1024*1024*1024LL) c = 208;
	else if (s < 50*1024*1024*1024LL) c = 202;
	else				  c = 196;

	fg256(c);
}

static void
print_comma(int len, intmax_t i)
{
	char buf[64];
	char *s = buf + sizeof buf;

	*--s = 0;
	while (1) {
		*--s = (i % 10) + '0'; if ((i /= 10) <= 0) break;
		*--s = (i % 10) + '0'; if ((i /= 10) <= 0) break;
		*--s = (i % 10) + '0'; if ((i /= 10) <= 0) break;
		*--s = ',';
	}

	printf("%*s", len+len/3, s);
}

static void
print_human(intmax_t i)
{
	double d = i;
	const char *u = "\0\0K\0M\0G\0T\0P\0E\0Z\0Y\0";
	while (d >= 1024) {
		u += 2;
		d /= 1024.0;
	}

	color_size_on(i);

	if (!*u)
		printf("%5.0f", d);
	else if (d < 10.0)
		printf("%4.1f%s", d, u);
	else
		printf("%4.0f%s", d, u);

	fgdefault();

}

/* Decode one UTF-8 codepoint into cp, return number of bytes to next one.
 * On invalid UTF-8, return -1, and do not change cp.
 * Invalid codepoints are not checked.
 *
 * This code is meant to be inlined, if cp is unused it can be optimized away.
 */
static int
u8decode(const char *cs, uint32_t *cp)
{
	const uint8_t *s = (uint8_t *)cs;

	if (*s == 0)   { *cp = 0; return 0; }
	if (*s < 0x80) { *cp = *s; return 1; }
	if (*s < 0xc2) { return -1; }  /*cont+overlong*/
	if (*s < 0xe0) { *cp = *s & 0x1f; goto u2; }
	if (*s < 0xf0) {
		if (*s == 0xe0 && (s[1] & 0xe0) == 0x80) return -1; /*overlong*/
		if (*s == 0xed && (s[1] & 0xe0) == 0xa0) return -1; /*surrogate*/
		*cp = *s & 0x0f; goto u3;
	}
	if (*s < 0xf5) {
		if (*s == 0xf0 && (s[1] & 0xf0) == 0x80) return -1; /*overlong*/
		if (*s == 0xf4 && (s[1] > 0x8f)) return -1; /*too high*/
		*cp = *s & 0x07; goto u4;
	}
	return -1;

u4:	if ((*++s & 0xc0) != 0x80) return -1;  *cp = (*cp << 6) | (*s & 0x3f);
u3:	if ((*++s & 0xc0) != 0x80) return -1;  *cp = (*cp << 6) | (*s & 0x3f);
u2:	if ((*++s & 0xc0) != 0x80) return -1;  *cp = (*cp << 6) | (*s & 0x3f);
	return s - (uint8_t *)cs + 1;
}

static void
print_shquoted(const char *s)
{
	uint32_t ignored;
	int l;

	const char *t;
	int esc = 0;

	if (!Qflag) {
		printf("%s", s);
		return;
	}

	for (t = s; *t; ) {
		if ((unsigned char)*t <= 32 ||
		    strchr("`^#*[]=|\\?${}()'\"<>&;\177", *t)) {
			esc = 1;
			break;
		} else {
			if ((l = u8decode(t, &ignored)) < 0) {
				esc = 1;
				break;
			}
			t += l;
		}
	}

	if (!esc) {
		printf("%s", s);
		return;
	}

	if (Pflag) {
		printf("$'");
		for (; *s; s++)
			switch (*s) {
			case '\a': printf("\\a"); break;
			case '\b': printf("\\b"); break;
			case '\e': printf("\\e"); break;
			case '\f': printf("\\f"); break;
			case '\n': printf("\\n"); break;
			case '\r': printf("\\r"); break;
			case '\t': printf("\\t"); break;
			case '\v': printf("\\v"); break;
			case '\\': printf("\\\\"); break;
			case '\'': printf("\\\'"); break;
			default:
				if ((unsigned char)*s < 32 ||
				    (l = u8decode(s, &ignored)) < 0) {
					printf("\\%03o", (unsigned char)*s);
				} else {
					printf("%.*s", l, s);
					s += l-1;
				}
			}
		putchar('\'');
	} else {
		putchar('\'');
		for (; *s; s++)
			if (*s == '\'')
				printf("'\\''");
			else
				putchar(*s);
		putchar('\'');
	}
}

void
print_noprefix(struct fileinfo *fi)
{
	if (fi->prefixl == 0 && fi->fpath[0])
		print_shquoted(fi->fpath);
	else if (strlen(fi->fpath) > fi->prefixl + 1)  /* strip prefix */
		print_shquoted(fi->fpath + fi->prefixl +
		    (fi->fpath[fi->prefixl] == '/'));
	else if (S_ISDIR(fi->sb.st_mode))  /* turn empty string into "." */
		printf(".");
	else  /* turn empty string into basename */
		print_shquoted(basenam(fi->fpath));
}

void
analyze_format()
{
	char *s;
	for (s = format; *s; s++) {
		if (*s == '\\') {
			s++;
			continue;
		}
		if (*s != '%')
			continue;
		switch (*++s) {
		case 'g': need_group++; break;
		case 'u': need_user++; break;
		case 'Y': need_fstype++; break;
		case 'x': need_xattr++; break;
		}
		switch (*s) {
		case 'd':
		case 'I':
		case 'p':
		case 'P':
		case 'f':
			/* all good without stat */
			break;
		case 'e':
			if (!Dflag)
				need_stat++;
			break;
		default:
			need_stat++;
		}
	}

	for (s = ordering; *s; s++) {
		switch (*s) {
		case 'd':
		case 'D':
		case 'n':
		case 'N':
		case 'e':
		case 'E':
		case 'p':
		case 'P':
		case 'v':
		case 'V':
			/* all good without stat */
			break;
		default:
			need_stat++;
		}
	}

	if (Gflag)
		need_stat++;
}

static void
color_age_on(time_t t)
{
	time_t age = now - t;
	int c;

	if (!Gflag)
		return;

	if      (age <               0LL) c = 196;
	else if (age <              60LL) c = 255;
	else if (age <           60*60LL) c = 252;
	else if (age <        24*60*60LL) c = 250;
	else if (age <      7*24*60*60LL) c = 244;
	else if (age <    4*7*24*60*60LL) c = 244;
	else if (age <   26*7*24*60*60LL) c = 242;
	else if (age <   52*7*24*60*60LL) c = 240;
	else if (age < 2*52*7*24*60*60LL) c = 238;
	else				  c = 236;

	fg256(c);
}

static void
color_name_on(int c, const char *f, mode_t m)
{
	const char *b;

	if (!Gflag)
		return;

	if (c != COLOR_DEFAULT) {
		fg256(c);
		return;
	}

	b = basenam(f);

	if (m & S_IXUSR)
		fgbold();

	if (*b == '.' || (S_ISREG(m) && b[strlen(b)-1] == '~'))
		fg256(238);
	else if (S_ISDIR(m))
		fg256(242);
	else if (S_ISREG(m) && (m & S_IXUSR))
		fg256(154);
	else if (m == 0)  /* broken link */
		fg256(196);
}

static void
print_urlquoted(unsigned char *s)
{
	for (; *s; s++)
		if (*s > 127 || rfc3986[*s])
			printf("%%%02x", *s);
		else
			putchar(*s);
}

static void
hyperlink_on(char *fpath)
{
	/* OSC 8 hyperlink format */
	if (Xflag) {
		printf("\033]8;;file://%s", host);
		if (*fpath != '/') {
			print_urlquoted((unsigned char *)basepath);
			putchar('/');
		}
		print_urlquoted((unsigned char *)fpath);
		putchar('\007');
	}
}

static void
hyperlink_off()
{
	if (Xflag)
		printf("\033]8;;\007");
}

/* unused format codes: BEHJKLNOQVWXZ achjoqrvwz */
void
print_format(struct fileinfo *fi)
{
	int c, v;
	char *s;

	if (fi->color == COLOR_HIDDEN)
		return;

	for (s = format; *s; s++) {
		if (*s == '\\') {
			switch (*++s) {
			case 'a': putchar('\a'); break;
			case 'b': putchar('\b'); break;
			case 'f': putchar('\f'); break;
			case 'n': putchar('\n'); break;
			case 'r': putchar('\r'); break;
			case 't': putchar('\t'); break;
			case 'v': putchar('\v'); break;
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7':
				for (c = 3, v = 0;
				     c > 0 && *s >= '0' && *s <= '7';
				     s++, c--) {
					v = (v << 3) + ((*s) - '0');
				}
				s--;
				putchar(v & 0xff);
				break;
			case 'x':
				s++;
				for (c = 2, v = 0;
				     c > 0 && isxdigit((unsigned char)*s);
				     s++, c--) {
					v <<= 4;
					if (*s >= 'A' && *s <= 'F')
						v += *s - 'A' + 10;
					else if (*s >= 'a' && *s <= 'f')
						v += *s - 'a' + 10;
					else
						v += *s - '0';
				}
				s--;
				putchar(v);
				break;
			default: putchar(*s);
			}
			continue;
		}
		if (*s != '%') {
			putchar(*s);
			continue;
		}
		switch (*++s) {
		case '%': putchar('%'); break;
		case 's':
			if (!hflag) {
				color_size_on(fi->sb.st_size);
				if (Gflag)
					print_comma(intlen(maxsize),
					    (intmax_t)fi->sb.st_size);
				else
					printf("%*jd", intlen(maxsize),
					    (intmax_t)fi->sb.st_size);
				fgdefault();
				break;
			}
			/* FALLTHRU */
		case 'S': print_human((intmax_t)fi->sb.st_size); break;
		case 'b': printf("%*jd", intlen(maxblocks), (intmax_t)fi->sb.st_blocks); break;
		case 'k': printf("%*jd", intlen(maxblocks/2), (intmax_t)fi->sb.st_blocks / 2); break;
		case 'd': printf("%*d", intlen(maxdepth), fi->depth); break;
		case 'D': printf("%*jd", intlen(maxdev), (intmax_t)fi->sb.st_dev); break;
		case 'R': printf("%*jd", intlen(maxrdev), (intmax_t)fi->sb.st_rdev); break;
		case 'i': printf("%*jd", intlen(maxino), (intmax_t)fi->sb.st_ino); break;
		case 'I': {
			int i;
			for (i = 0; i < fi->depth; i++)
				printf(" ");
			break;
		}
		case 'p':
			if (!sflag) {
				color_name_on(fi->color, fi->fpath, fi->sb.st_mode);
				hyperlink_on(fi->fpath);
				if (!fi->fpath[0] && S_ISDIR(fi->sb.st_mode))
					print_shquoted(".");
				else
					print_shquoted(fi->fpath);
				hyperlink_off();
				fgdefault();
				break;
			}
			/* FALLTHRU */
		case 'P':
			color_name_on(fi->color, fi->fpath, fi->sb.st_mode);
			hyperlink_on(fi->fpath);
			print_noprefix(fi);
			hyperlink_off();
			fgdefault();
			break;
		case 'l':
			if (S_ISLNK(fi->sb.st_mode)) {
				char target[PATH_MAX];
				size_t j = strlen(fi->fpath);
				struct stat st;
				st.st_mode = 0;

				snprintf(target, sizeof target, "%s", fi->fpath);
				while (j && target[j-1] != '/')
					j--;
				ssize_t l = readlink(fi->fpath,
				    target+j, sizeof target - j);
				if (l > 0 && (size_t)l < sizeof target - j) {
					target[j+l] = 0;
					if (Gflag)
						lstat(target[j] == '/' ?
						    target + j : target,
						    &st);
				} else {
					*target = 0;
				}
				color_name_on(-1, target, st.st_mode);
				hyperlink_on(target);
				print_shquoted(target + j);
				hyperlink_off();
				fgdefault();
			}
			break;
		case 'n': printf("%*jd", intlen(maxnlink), (intmax_t)fi->sb.st_nlink); break;
		case 'F':
			if (S_ISDIR(fi->sb.st_mode)) {
				putchar('/');
			} else if (S_ISSOCK(fi->sb.st_mode)) {
				putchar('=');
			} else if (S_ISFIFO(fi->sb.st_mode)) {
				putchar('|');
			} else if (S_ISLNK(fi->sb.st_mode)) {
				if (lflag)
					printf(" -> ");
				else
					putchar('@');
			} else if (fi->sb.st_mode & S_IXUSR) {
				putchar('*');
			}
			break;
		case 'f':
			color_name_on(fi->color, fi->fpath, fi->sb.st_mode);
			hyperlink_on(fi->fpath);
			print_shquoted(basenam(fi->fpath));
			hyperlink_off();
			fgdefault();
			break;
		case 'A':
		case 'C':
		case 'T':
		case '\324': /* Meta-T */ {
			char tfmt[3] = "%\0\0";
			char buf[256];

			if (*s == '\324')
				*s = Tflag;

			time_t t = (*s == 'A' ? fi->sb.st_atime :
			    *s == 'C' ? fi->sb.st_ctime :
			    fi->sb.st_mtime);

			s++;
			if (!*s)
				break;

			color_age_on(t);
			if (*s == '-') {
				long diff = now - t;
				printf("%4ldd%3ldh%3ldm%3lds",
				    diff / (60*60*24),
				    (diff / (60*60)) % 24,
				    (diff / 60) % 60,
				    diff % 60);
			} else {
				tfmt[1] = *s;
				strftime(buf, sizeof buf, tfmt, localtime(&t));
				printf("%s", buf);
			}
			fgdefault();
			break;
		}
		case 'm': printf("%04o", (unsigned int)fi->sb.st_mode & 07777); break;
		case 'M': print_mode(fi->sb.st_mode); break;
		case 'y':
			putchar("0pcCd?bBf?l?s???"[(fi->sb.st_mode >> 12) & 0x0f]);
			break;

		case 'g': printf("%*s", -gwid, groupname(fi->sb.st_gid)); break;
		case 'G': printf("%*ld", intlen(maxgid), (long)fi->sb.st_gid); break;
		case 'u': printf("%*s", -uwid, username(fi->sb.st_uid)); break;
		case 'U': printf("%*ld", intlen(maxuid), (long)fi->sb.st_uid); break;

		case 'e': printf("%ld", (long)count_entries(fi)); break;
		case 't': printf("%jd", (intmax_t)fi->total); break;
		case 'Y': printf("%*s", -fwid, fstype(fi->sb.st_dev)); break;
		case 'x': printf("%*s", -maxxattr, fi->xattr); break;
		default:
			putchar('%');
			putchar(*s);
		}
	}
}

static int initial;

int
callback(const char *fpath, const struct stat *sb, int depth, ino_t entries, off_t total)
{
	struct fileinfo *fi = malloc(sizeof (struct fileinfo));
	fi->fpath = strdup(fpath);
	fi->prefixl = prefixl;
	fi->depth = Bflag ? (depth > 0 ? bflag_depth + 1 : 0) : depth;
	fi->entries = entries;
	fi->total = total;
	fi->color = current_color;
	memcpy((char *)&fi->sb, (char *)sb, sizeof (struct stat));

	prune = 0;
	if (expr && !eval(expr, fi)) {
		if (Bflag && S_ISDIR(fi->sb.st_mode) && !prune) {
			fi->color = COLOR_HIDDEN;
		} else {
			free_fi(fi);
			return 0;
		}
	}

	if (need_xattr) {
		strncpy(fi->xattr, xattr_string(fi->fpath), sizeof fi->xattr - 1);
		if (strlen(fi->xattr) > maxxattr)
			maxxattr = strlen(fi->xattr);
	} else
		memset(fi->xattr, 0, sizeof fi->xattr);

	if (Uflag || Wflag) {
		print_format(fi);
		free_fi(fi);
		return 0;
	} else if (Bflag) {
		if (initial && fi->depth == 0) {
			root = fitree_insert(root, fi);
		} else if (!initial && fi->depth == bflag_depth + 1) {
			new_root = fitree_insert(new_root, fi);
		} else {
			free_fi(fi);
			return 0;
		}
	} else {
		/* add to the tree, note that this will eliminate duplicate files */
		root = fitree_insert(root, fi);
	}

	if (depth > maxdepth)
		maxdepth = depth;

	if (need_stat) {
		if (fi->sb.st_nlink > maxnlink)
			maxnlink = fi->sb.st_nlink;
		if (fi->sb.st_size > maxsize)
			maxsize = fi->sb.st_size;
		if (fi->sb.st_rdev > maxrdev)
			maxrdev = fi->sb.st_rdev;
		if (fi->sb.st_dev > maxdev)
			maxdev = fi->sb.st_dev;
		if (fi->sb.st_blocks > maxblocks)
			maxblocks = fi->sb.st_blocks;
		if (fi->sb.st_uid > maxuid)
			maxuid = fi->sb.st_uid;
		if (fi->sb.st_gid > maxuid)
			maxgid = fi->sb.st_gid;
		if (fi->sb.st_ino > maxino)
			maxino = fi->sb.st_ino;
	}

	/* prefetch user/group/fs name for correct column widths. */
	if (need_user)
		username(fi->sb.st_uid);
	if (need_group)
		groupname(fi->sb.st_gid);
	if (need_fstype)
		fstype(fi->sb.st_dev);

	return 0;
}

/* lifted from musl nftw. */
struct history {
	struct history *chain;
	dev_t dev;
	ino_t ino;
	int level;
	off_t total;
};

struct names {
	char *path;
	int guessdir;
};

int cmpstr(void const *a, void const *b) {
    struct names *aa = (struct names *)a;
    struct names *bb = (struct names *)b;

    return strcmp(aa->path, bb->path);
}

/* NB: path is expected to have MAXPATHLEN space used in recursive calls. */
static int
recurse(char *path, struct history *h, int guessdir)
{
	size_t l = strlen(path), j = l && path[l-1] == '/' ? l - 1 : l;
	struct stat st = { 0 };
	struct history new;
	int r;
	ino_t entries;
	const char *fpath = *path ? path : ".";
	struct names *names = 0, *tmp;
	size_t len = 0;

	int resolve = Lflag || (Hflag && !h);
	int root = (path[0] == '/' && path[1] == 0);

	if (Bflag && h && h->chain)
		return 0;

	if (need_stat)
		guessdir = 1;

	if (guessdir && (resolve ? stat(fpath, &st) : lstat(fpath, &st)) < 0) {
		if (resolve && (errno == ENOENT || errno == ELOOP) &&
		    !lstat(fpath, &st)) {
			/* ignore */
		} else if (!h) {
			/* warn for toplevel arguments */
			fprintf(stderr, "lr: cannot %sstat '%s': %s\n",
			    resolve ? "" : "l",
			    fpath, strerror(errno));
			status = 1;
			return -1;
		} else {
			if (!qflag) {
				fprintf(stderr, "lr: cannot %sstat '%s': %s\n",
				    resolve ? "" : "l",
				    fpath, strerror(errno));
				status = 1;
			}
			if (errno != EACCES)
				return -1;
		}
	}

	if (guessdir && xflag && h && st.st_dev != h->dev)
		return 0;

	new.chain = h;
	new.level = h ? h->level + 1 : 0;
	new.total = 0;
	if (guessdir) {
		new.dev = st.st_dev;
		new.ino = st.st_ino;
		new.total = st.st_blocks / 2;
	}
	entries = 0;

	if (!Dflag) {
		r = callback(path, &st, new.level, 0, 0);
		if (prune)
			return 0;
		if (r)
			return r;
	}

	if (guessdir)
		for (; h; h = h->chain) {
			if (h->dev == st.st_dev && h->ino == st.st_ino) {
				if (!qflag)
					fprintf(stderr, "lr: file system loop detected: '%s'\n",
					    fpath);
				return 0;
			}
			h->total += st.st_blocks / 2;
		}

	if (guessdir && S_ISDIR(st.st_mode)) {
		DIR *d = opendir(fpath);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				if (de->d_name[0] == '.' &&
				    (!de->d_name[1] ||
				    (de->d_name[1] == '.' && !de->d_name[2])))
					continue;
				entries++;
				if (strlen(de->d_name) >= PATH_MAX-l) {
					errno = ENAMETOOLONG;
					closedir(d);
					return -1;
				}
				if (j > 0 || root) {
					path[j] = '/';
					strcpy(path + j + 1, de->d_name);
				} else {
					strcpy(path, de->d_name);
				}
#if defined(DT_DIR) && defined(DT_UNKNOWN)
				int guesssubdir = de->d_type == DT_DIR ||
				    (de->d_type == DT_LNK && resolve) ||
				    de->d_type == DT_UNKNOWN;
#else
				int guesssubdir = 1;
#endif
				if (Wflag) {
					/* store paths in names for later sorting */
					if (entries >= len) {
						len = 2 * len + 1;
						if (len > SIZE_MAX/sizeof *names)
							break;
						tmp = realloc(names, len * sizeof *names);
						if (!tmp)
							break;
						names = tmp;
					}
					names[entries-1].path = strdup(path);
					names[entries-1].guessdir = guesssubdir;
				} else if ((r = recurse(path, &new, guesssubdir))) {
					closedir(d);
					return r;
				}
			}
			closedir(d);
		} else if (qflag && (errno == EACCES || errno == ENOTDIR)) {
			return -1;
		} else {
			fprintf(stderr, "lr: cannot open directory '%s': %s\n",
			    fpath, strerror(errno));
			status = 1;
			return -1;
		}
	}

	if (Wflag && names) {
		size_t i;

		qsort(names, entries, sizeof *names, cmpstr);

		for (i = 0; i < entries; i++) {
			strcpy(path, names[i].path);
			recurse(path, &new, names[i].guessdir);
		}

		/* ensure cleanup in reverse allocation order */
		for (i = 0; i < entries; i++)
			free(names[entries-i-1].path);
		free(names);
	}

	path[l] = 0;
	if (Dflag && (r = callback(path, &st, new.level, entries, new.total)))
		return r;

	return 0;
}

void
tree_recurse(struct fileinfo *fi)
{
	if (S_ISDIR(fi->sb.st_mode)) {
		char path[PATH_MAX];

		strcpy(path, fi->fpath);
		bflag_depth = fi->depth;
		recurse(path, 0, 0);
	}
}

int
traverse_file(FILE *file)
{
	char *line = 0;
	size_t linelen = 0;
	struct stat st;
	ssize_t rd;

	prefixl = 0;

	while (1) {
		errno = 0;
		rd = getdelim(&line, &linelen, input_delim, file);
		if (rd == -1) {
			if (errno != 0)
				return -1;
			break;
		}

		if (rd > 0 && line[rd-1] == input_delim)  /* strip delimiter */
			line[rd-1] = 0;

		if (Lflag ? stat(line, &st) : lstat(line, &st) < 0)
			continue;

		callback(line, &st, 0, 0, 0);
	}

	free(line);
	return 0;
}

int
traverse(const char *path)
{
	char pathbuf[PATH_MAX + 1];

	if (path[0] == '-' && !path[1])
		return traverse_file(stdin);

	if (path[0] == '@') {
		FILE *f = fopen(path + 1, "r");
		if (!f)
			parse_error("can't open input file '%s'", path + 1);
		int r = traverse_file(f);
		fclose(f);
		return r;
	}

	prefixl = strlen(path);
	while (prefixl && path[prefixl-1] == '/')
		prefixl--;
	if (prefixl > PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(pathbuf, path, prefixl + 1);
	pathbuf[prefixl + 1] = 0;
	return recurse(pathbuf, 0, 1);
}

static char
timeflag(char *arg)
{
	if ((arg[0] == 'A' || arg[0] == 'C' || arg[0] == 'M') && !arg[1])
		return arg[0] == 'M' ? 'T' : arg[0];
	fprintf(stderr, "%s: -T only accepts A, C or M as argument.\n", argv0);
	exit(2);
}

int
main(int argc, char *argv[])
{
	int i, c;

	format = default_format;
	ordering = default_ordering;
	argv0 = argv[0];
	now = time(0);
	status = 0;

	setlocale(LC_ALL, "");

	while ((c = getopt(argc, argv, "01ABC:DFGHLQPST:UWXde:f:lho:st:qx")) != -1)
		switch (c) {
		case '0': format = zero_format; input_delim = 0; Qflag = Pflag = 0; break;
		case '1': expr = chain(parse_expr("depth > 0 ? prune : print"), EXPR_AND, expr); break;
		case 'A': expr = chain(expr, EXPR_AND, parse_expr("name =~ \"^\\.\" && path != \".\" ? (prune && skip) : print")); break;
		case 'B': Bflag++; Dflag = 0; Uflag = 0; need_stat++; break;
		case 'C':
			if ((unsigned int)Cflag <
			    sizeof Cflags / sizeof Cflags[0]) {
				Cflags[Cflag++] = optarg;
			} else {
				fprintf(stderr, "%s: too many -C\n", argv0);
				exit(111);
			}
			Gflag += 2;  /* force color on */
			break;
		case 'D': Dflag++; Bflag = 0; break;
		case 'F': if (!lflag) format = type_format; break;
		case 'G': Gflag++; break;
		case 'H': Hflag++; break;
		case 'L': Lflag++; break;
		case 'Q': Qflag++; break;
		case 'P': Pflag++; Qflag++; break;
		case 'S': Qflag++; format = stat_format; break;
		case 'T': Tflag = timeflag(optarg); break;
		case 'W': Wflag++; Bflag = Uflag = 0; break;
		case 'U': Uflag++; Bflag = Wflag = 0; break;
		case 'X': Xflag++; break;
		case 'd': expr = chain(parse_expr("type == d && prune || print"), EXPR_AND, expr); break;
		case 'e': expr = chain(expr, EXPR_AND,
		    mkstrexpr(PROP_NAME, EXPR_REGEX, optarg, 0)); break;
		case 'f': format = optarg; break;
		case 'h': hflag++; break;
		case 'l': lflag++; Qflag++; format = long_format; break;
		case 'o': Uflag = Wflag = 0; ordering = optarg; break;
		case 's': sflag++; break;
		case 't':
			need_stat++;  /* overapproximation */
			expr = chain(expr, EXPR_AND, parse_expr(optarg)); break;
		case 'q': qflag++; break;
		case 'x': xflag++; break;
		default:
			fprintf(stderr,
"Usage: %s [-0|-F|-l [-TA|-TC|-TM]|-S|-f FMT] [-B|-D] [-H|-L] [-1AGPQdhsx]\n"
"          [-U|-W|-o ORD] [-e REGEX]* [-t TEST]* [-C [COLOR:]PATH]* PATH...\n", argv0);
			exit(2);
		}

	if (isatty(1)) {
		Qflag = 1;
	} else {
		if (Gflag == 1)
			Gflag = 0;
		if (Xflag == 1)
			Xflag = 0;
	}
	if (getenv("NO_COLOR"))
		Gflag = 0;

	analyze_format();
	if (Uflag || Wflag) {
		maxnlink = 99;
		maxsize = 4*1024*1024;
		maxblocks = maxsize / 512;
		maxrdev = maxdev = 255;
		maxuid = maxgid = 65536;
		maxino = 9999999;
		maxdepth = 99;
		uwid = gwid = fwid = 8;
	}
	if (Xflag) {
		basepath = realpath(".", 0);
		if (!basepath) {
			fprintf(stderr,
			    "%s: cannot resolve base path, disabling -X: %s\n",
			    argv0, strerror(errno));
			Xflag = 0;
		}

		if (gethostname(host, sizeof host) == 0) {
			/* termination not posix guaranteed */
			host[sizeof host - 1] = 0;
		} else {
			/* ignore errors, use no host */
			*host = 0;
		}
	}

	for (i = 0; i < Cflag; i++) {
		char *r;
		errno = 0;
		current_color = strtol(Cflags[i], &r, 10);
		if (errno == 0 && r != Cflags[i] && *r == ':') {
			current_color = current_color & 0xff;
			traverse(r + 1);
		} else {
			current_color = 2;
			traverse(Cflags[i]);
		}
	}

	current_color = COLOR_DEFAULT;

	initial = 1;
	if (!Cflag && optind == argc) {
		traverse("");
	} else {
		for (i = optind; i < argc; i++)
			traverse(argv[i]);
	}
	initial = 0;

	if (Bflag) {
		while (root) {
			fitree_walk(root, print_format);
			fitree_walk(root, tree_recurse);
			fitree_free(root);

			root = new_root;
			new_root = 0;
		}
	} else if (!Uflag) {
		fitree_walk(root, print_format);
		/* no need to destroy here, we are done */
	}

	return status;
}
