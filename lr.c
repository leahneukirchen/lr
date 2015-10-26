/* lr - a better recursive ls/find */

/*
 * Copyright (C) 2015 Christian Neukirchen <purl.org/net/chneukirchen>
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

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <pwd.h>
#include <regex.h>
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int Dflag;
static int Hflag;
static int Lflag;
static int xflag;
static int Uflag;
static int lflag;
static int sflag;
static int Qflag;

static char *argv0;
static char *format;
static char *ordering;
static struct expr *expr;
static void *root = NULL; // tree
static int prune;

static char default_ordering[] = "n";
static char default_format[] = "%p\\n";
static char type_format[] = "%p%F\\n";
static char long_format[] = "%M %n %u %g %s %TY-%Tm-%Td %TH:%TM %p%F%l\n";
static char zero_format[] = "%p\\0";

static void *users;
static void *groups;

struct idmap {
	long id;
	char *name;
};

static off_t maxsize;
static nlink_t maxlinks;
static int uwid, gwid;

struct fileinfo {
	char *fpath;
	int depth, entries;
	struct stat sb;
	off_t total;
};

enum op {
	EXPR_OR = 1,
	EXPR_AND,
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
	EXPR_TYPE,
	EXPR_ALLSET,
	EXPR_ANYSET,
};

enum prop {
	PROP_ATIME = 1,
	PROP_CTIME,
	PROP_DEPTH,
	PROP_DEV,
	PROP_ENTRIES,
	PROP_GID,
	PROP_GROUP,
	PROP_INODE,
	PROP_LINKS,
	PROP_MODE,
	PROP_MTIME,
	PROP_NAME,
	PROP_PATH,
	PROP_SIZE,
	PROP_TARGET,
	PROP_TOTAL,
	PROP_UID,
	PROP_USER,
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
	} a, b;
};

static char *pos;

static void
parse_error(const char *msg)
{
	fprintf(stderr, "%s: parse error: %s at '%.15s'\n", argv0, msg, pos);
	exit(2);
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
	while (isspace(*pos))
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
	if (isdigit(*pos)) {
		int64_t n;

		for (n = 0; isdigit(*pos) && n <= INT64_MAX / 10 - 10; pos++)
			n = 10 * n + (*pos - '0');
		if (isdigit(*pos))
			parse_error("number too big");
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
	if (*pos >= '0' && *pos <= '7') {
		long n = 0;
		while (*pos >= '0' && *pos <= '7') {
			n *= 8;
			n += *pos - '0';
			pos++;
			if (n > 07777)
				parse_error("number to big");
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
	else if (token("=="))
		return EXPR_EQ;
	else if (token("!="))
		return EXPR_NEQ;

	return 0;
}

static struct expr *parse_cmp();
static struct expr *parse_or();

static struct expr *
parse_inner()
{
	if (token("prune")) {
		struct expr *e = mkexpr(EXPR_PRUNE);
		return e;
	} else if (token("print")) {
		struct expr *e = mkexpr(EXPR_PRINT);
		return e;
	} else if (token("!")) {
		struct expr *e = parse_cmp();
		struct expr *not = mkexpr(EXPR_NOT);
		not->a.expr = e;
		return not;
	} else if (token("(")) {
		struct expr *e = parse_or();
		if (token(")"))
			return e;
		parse_error("missing )");
		return 0;
	} else {
		parse_error("unknown expression");
		return 0;
	}
}

static struct expr *
parse_type()
{
	if (token("type")) {
		if (token("==")) {  // TODO !=
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
			else
				parse_error("invalid file type");
			return e;
		} else {
			parse_error("invalid file type comparison");
		}
	}

	return parse_inner();
}

static int
parse_string(char **s)
{
	if (*pos == '"') {
		pos++;
		char *e = strchr(pos, '"');
		*s = strndup(pos, e - pos);
		pos += e - pos + 1;
		ws();
		return 1;
	}

	return 0;
}

static struct expr *
parse_strcmp()
{
	enum prop prop;
	enum op op;

	if (token("group"))
		prop = PROP_GROUP;
	else if (token("name"))
		prop = PROP_NAME;
	else if (token("path"))
		prop = PROP_PATH;
	else if (token("target"))
		prop = PROP_TARGET;
	else if (token("user"))
		prop = PROP_USER;
	else
		return parse_type();

	if (token("==="))
		op = EXPR_STREQI;
	else if (token("=="))
		op = EXPR_STREQ;
	else if (token("~~~"))
		op = EXPR_GLOBI;
	else if (token("~~"))
		op = EXPR_GLOB;
	else if (token("=~~"))
		op = EXPR_REGEXI;
	else if (token("=~"))
		op = EXPR_REGEX;
	else
		parse_error("invalid string operator");

	char *s;
	if (parse_string(&s)) {
		struct expr *e = mkexpr(op);
		e->a.prop = prop;
		if (op == EXPR_REGEX) {
			e->b.regex = malloc(sizeof (regex_t));
			regcomp(e->b.regex, s, REG_EXTENDED | REG_NOSUB);
		} else if (op == EXPR_REGEXI) {
			e->b.regex = malloc(sizeof (regex_t));
			regcomp(e->b.regex, s, REG_EXTENDED | REG_NOSUB | REG_ICASE);
		} else {
			e->b.string = s;
		}
		return e;
	}

	parse_error("invalid string");
	return 0;
}

static struct expr *
parse_mode()
{
	struct expr *e = mkexpr(0);
	long n;

	e->a.prop = PROP_MODE;

	if (token("==")) {
		e->op = EXPR_EQ;
	} else if (token("&")) {
		e->op = EXPR_ALLSET;
	} else if (token("|")) {
		e->op = EXPR_ANYSET;
	} else {
		parse_error("invalid mode comparison");
	}

	if (parse_octal(&n)) {
		e->b.num = n;
	} else {
		parse_error("invalid mode");
	}

	return e;
}

static struct expr *
parse_cmp()
{
	enum prop prop;
	enum op op;

	if (token("atime"))
		prop = PROP_ATIME;
	else if (token("ctime"))
		prop = PROP_CTIME;
	else if (token("depth"))
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
	else if (token("mtime"))
		prop = PROP_MTIME;
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
		parse_error("invalid comparison");

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
chain(struct expr *e1, enum op op, struct expr *e2)
{
	struct expr *i, *j, *e;
	if (!e1)
		return e2;
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
	struct expr *e1 = parse_cmp();
	struct expr *r = e1;

	while (token("&&")) {
		struct expr *e2 = parse_cmp();
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
parse_expr(const char *s)
{
	pos = (char *)s;
	struct expr *e = parse_or();
	if (*pos)
		parse_error("trailing garbage");
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
	int r = readlink(p, b, sizeof b - 1);
	if (r < 0)
		return alt;
	b[r] = 0;
	return b;
}

int
idorder(const void *a, const void *b)
{
	struct idmap *ia = (struct idmap *)a;
	struct idmap *ib = (struct idmap *)b;

	if (ia->id == ib->id)
		return 0;
	else if (ia->id < ib->id)
		return -1;
	else
		return 1;
}

static char *
strid(long id)
{
	static char buf[32];
	sprintf(buf, "%ld", id);
	return buf;
}

static char *
groupname(gid_t gid)
{
	struct idmap key, **result;
	key.id = gid;
	key.name = 0;

	if (!(result = tfind(&key, &groups, idorder))) {
		struct group *g = getgrgid(gid);
		if (g) {
			struct idmap *newkey = malloc(sizeof (struct idmap));
			newkey->id = gid;
			newkey->name = strdup(g->gr_name);
			if ((int)strlen(g->gr_name) > gwid)
				gwid = strlen(g->gr_name);
			tsearch(newkey, &groups, idorder);
			return newkey->name;
		}
	}

	return result ? (*result)->name : strid(gid);
}

static char *
username(uid_t uid)
{
	struct idmap key, **result;
	key.id = uid;
	key.name = 0;

	if (!(result = tfind(&key, &users, idorder))) {
		struct passwd *p = getpwuid(uid);
		if (p) {
			struct idmap *newkey = malloc(sizeof (struct idmap));
			newkey->id = uid;
			newkey->name = strdup(p->pw_name);
			if ((int)strlen(p->pw_name) > uwid)
				uwid = strlen(p->pw_name);
			tsearch(newkey, &users, idorder);
			return newkey->name;
		}
	}

	return result ? (*result)->name : strid(uid);
}

int
eval(struct expr *e, struct fileinfo *fi)
{
	switch (e->op) {
	case EXPR_OR:
		return eval(e->a.expr, fi) || eval(e->b.expr, fi);
	case EXPR_AND:
		return eval(e->a.expr, fi) && eval(e->b.expr, fi);
	case EXPR_NOT:
		return !eval(e->a.expr, fi);
	case EXPR_PRUNE:
		prune = 1;
		return 1;
	case EXPR_PRINT:
		return 1;
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_EQ:
	case EXPR_NEQ:
	case EXPR_GE:
	case EXPR_GT:
	case EXPR_ALLSET:
	case EXPR_ANYSET: {
		long v = 0;
		switch (e->a.prop) {
		case PROP_ATIME: v = fi->sb.st_atime; break;
		case PROP_CTIME: v = fi->sb.st_ctime; break;
		case PROP_DEPTH: v = fi->depth; break;
		case PROP_DEV: v = fi->sb.st_dev; break;
		case PROP_ENTRIES: v = fi->entries; break;
		case PROP_GID: v = fi->sb.st_gid; break;
		case PROP_INODE: v = fi->sb.st_ino; break;
		case PROP_LINKS: v = fi->sb.st_nlink; break;
		case PROP_MODE: v = fi->sb.st_mode & 07777; break;
		case PROP_MTIME: v = fi->sb.st_mtime; break;
		case PROP_SIZE: v = fi->sb.st_size; break;
		case PROP_TOTAL: v = fi->total; break;
		case PROP_UID: v = fi->sb.st_uid; break;
		default:
			parse_error("unknown property");
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
		}
	}
	case EXPR_TYPE:	{
		switch (e->a.filetype) {
		case TYPE_BLOCK: return S_ISBLK(fi->sb.st_mode);
		case TYPE_CHAR: return S_ISCHR(fi->sb.st_mode);
		case TYPE_DIR: return S_ISDIR(fi->sb.st_mode);
		case TYPE_FIFO: return S_ISFIFO(fi->sb.st_mode);
		case TYPE_REGULAR: return S_ISREG(fi->sb.st_mode);
		case TYPE_SOCKET: return S_ISSOCK(fi->sb.st_mode);
		case TYPE_SYMLINK: return S_ISLNK(fi->sb.st_mode);
		}
	}
	case EXPR_STREQ:
	case EXPR_STREQI:
	case EXPR_GLOB:
	case EXPR_GLOBI:
	case EXPR_REGEX:
	case EXPR_REGEXI: {
		const char *s = "";
		switch(e->a.prop) {
		case PROP_GROUP: s = groupname(fi->sb.st_gid); break;
		case PROP_NAME: s = basenam(fi->fpath); break;
		case PROP_PATH: s = fi->fpath; break;
		case PROP_TARGET: s = readlin(fi->fpath, ""); break;
		case PROP_USER: s = username(fi->sb.st_uid); break;
		}
		switch (e->op) {
		case EXPR_STREQ:
			return strcmp(e->b.string, s) == 0;
		case EXPR_STREQI:
			return strcasecmp(e->b.string, s) == 0;
		case EXPR_GLOB:
			return fnmatch(e->b.string, s, FNM_PATHNAME) == 0;
		case EXPR_GLOBI:
			return fnmatch(e->b.string, s,
			    FNM_PATHNAME | FNM_CASEFOLD) == 0;
		case EXPR_REGEX:
		case EXPR_REGEXI:
			return regexec(e->b.regex, s, 0, 0, 0) == 0;
		}
	}
	}

	return 0;
}

#define CMP(a, b) if ((a) == (b)) break; else if ((a) < (b)) return -1; else return 1
#define STRCMP(a, b) if (strcmp(a, b) == 0) break; else return (strcmp(a, b));

int
order(const void *a, const void *b)
{
	struct fileinfo *fa = (struct fileinfo *)a;
	struct fileinfo *fb = (struct fileinfo *)b;
	char *s;

	for (s = ordering; *s; s++) {
		switch (*s) {
		// XXX use nanosecond timestamps
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
		case 'e': STRCMP(extnam(fa->fpath), extnam(fb->fpath));
		case 'E': STRCMP(extnam(fb->fpath), extnam(fa->fpath));
		default: STRCMP(fa->fpath, fb->fpath);
		}
	}

	return strcmp(fa->fpath, fb->fpath);
}

static int
intlen(int i)
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
	putchar(mode & 01000 ? (mode & 00001 ? 'T' : 't')
	                     : (mode & 00001 ? 'x' : '-'));
}

static void
shquote(const char *s)
{
	if (Qflag || !strpbrk(s, "\001\002\003\004\005\006\007\010"
	                         "\011\012\013\014\015\016\017\020"
	                         "\021\022\023\024\025\026\027\030"
	                         "\031\032\033\034\035\036\037\040"
	                         "`^#*[]=|\\?${}()'\"<>&;\127")) {
		printf("%s", s);
		return;
	}

	putchar('\'');
	for (; *s; s++)
		if (*s == '\'')
			printf("'\\''");
		else
			putchar(*s);
	putchar('\'');
}

void
print(const void *nodep, const VISIT which, const int depth)
{
	(void)depth;

	if (which == postorder || which == leaf) {
		struct fileinfo *fi = *(struct fileinfo **)nodep;

		char *s;
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
				case '0': putchar('\0'); break;
					// TODO: \NNN
				default:
					putchar(*s);
				}
			} else if (*s == '%') {
				switch (*++s) {
				case '%': putchar('%'); break;
				case 's': printf("%*jd", intlen(maxsize), (intmax_t)fi->sb.st_size); break;
				case 'b': printf("%jd", (intmax_t)fi->sb.st_blocks); break;
				case 'k': printf("%jd", (intmax_t)fi->sb.st_blocks / 2); break;
				case 'd': printf("%d", fi->depth); break;
				case 'D': printf("%jd", (intmax_t)fi->sb.st_dev); break;
				case 'i': printf("%jd", (intmax_t)fi->sb.st_ino); break;
				case 'I': {
					int i;
					for (i=0; i<fi->depth; i++)
						printf(" ");
					break;
				}
				case 'p': shquote(
					    sflag && strncmp(fi->fpath, "./", 2) == 0 ?
					    fi->fpath+2 : fi->fpath);
					break;
				case 'l':
					if (S_ISLNK(fi->sb.st_mode))
						shquote(readlin(fi->fpath, ""));
					break;
				case 'n': printf("%*jd", intlen(maxlinks), (intmax_t)fi->sb.st_nlink); break;
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
				case 'f': shquote(basenam(fi->fpath)); break;
				case 'A':
				case 'C':
				case 'T': {
					char tfmt[3] = "%\0\0";
					char buf[256];
					s++;
					if (!*s)
						break;
					tfmt[1] = *s;
					strftime(buf, sizeof buf, tfmt,
					    localtime(
					    *s == 'A' ? &fi->sb.st_atime :
					    *s == 'C' ? &fi->sb.st_ctime :
					    &fi->sb.st_mtime));
					printf("%s", buf);
					break;
				}
				case 'm': printf("%04o", fi->sb.st_mode & 07777); break;
				case 'M': print_mode(fi->sb.st_mode); break;
				case 'y':
					putchar("0pcCd?bBf?l?s???"[(fi->sb.st_mode >> 12) & 0x0f]);
					break;

				case 'g': {
					char *s = groupname(fi->sb.st_gid);
					if (s) {
						printf("%*s", -gwid, s);
						break;
					}
					/* FALLTHRU */
				}
				case 'G':
					printf("%*ld", gwid, (long)fi->sb.st_gid);
					break;

				case 'u': {
					char *s = username(fi->sb.st_uid);
					if (s) {
						printf("%*s", -uwid, s);
						break;
					}
					/* FALLTHRU */
				}
				case 'U':
					printf("%*ld", uwid, (long)fi->sb.st_uid);
					break;

				case 'e':
					printf("%ld", (long)fi->entries);
					break;
				case 't':
					printf("%jd", (intmax_t)fi->total);
					break;

				default:
					putchar('%');
					putchar(*s);
				}
			} else {
				putchar(*s);
			}
		}
	}
}

int
callback(const char *fpath, const struct stat *sb, int depth, int entries, off_t total)
{
	struct fileinfo *fi = malloc(sizeof (struct fileinfo));
	fi->fpath = strdup(fpath);
	fi->depth = depth;
	fi->entries = entries;
	fi->total = total;
	memcpy((char *)&fi->sb, (char *)sb, sizeof (struct stat));

	if (expr && !eval(expr, fi))
		return 0;

	if (Uflag)
		print(&fi, postorder, depth);
	else
		// add to the tree, note that this will elimnate duplicate files
		tsearch(fi, &root, order);

	if (fi->sb.st_nlink > maxlinks)
		maxlinks = fi->sb.st_nlink;
	if (fi->sb.st_size > maxsize)
		maxsize = fi->sb.st_size;

	return 0;
}

// lifted from musl nftw.
struct history {
	struct history *chain;
	dev_t dev;
	ino_t ino;
	int level;
	off_t total;
};

static int
recurse(char *path, struct history *h)
{
	size_t l = strlen(path), j = l && path[l-1]=='/' ? l - 1 : l;
	struct stat st;
	struct history new;
	int r, entries;

	int resolve = Lflag || (Hflag && h);

	if (resolve ? stat(path, &st) : lstat(path, &st) < 0) {
		if (resolve && errno == ENOENT && !lstat(path, &st))
			;
		else if (errno != EACCES)
			return -1;
	}

	if (xflag && h && st.st_dev != h->dev)
		return 0;

	new.chain = h;
	new.dev = st.st_dev;
	new.ino = st.st_ino;
	new.level = h ? h->level + 1 : 0;
	entries = 0;
	new.total = st.st_blocks / 2;

	if (!Dflag) {
		prune = 0;
		r = callback(path, &st, new.level, 0, 0);
		if (prune)
			return 0;
		if (r)
			return r;
	}

	for (; h; h = h->chain) {
		if (h->dev == st.st_dev && h->ino == st.st_ino)
			return 0;
		h->total += st.st_blocks / 2;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				entries++;
				if (de->d_name[0] == '.' &&
				    (!de->d_name[1] ||
				     (de->d_name[1]=='.' && !de->d_name[2])))
					continue;
				if (strlen(de->d_name) >= PATH_MAX-l) {
					errno = ENAMETOOLONG;
					closedir(d);
					return -1;
				}
				path[j] = '/';
				strcpy(path + j + 1, de->d_name);
				if ((r = recurse(path, &new))) {
					closedir(d);
					return r;
				}
			}
			closedir(d);
		} else if (errno != EACCES) {
			return -1;
		}
	}

	path[l] = 0;
	if (Dflag && (r = callback(path, &st, new.level, entries, new.total)))
		return r;

	return 0;
}

int
traverse(const char *path)
{
	char pathbuf[PATH_MAX + 1];
	size_t l = strlen(path);
	if (l > PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(pathbuf, path, l + 1);
	return recurse(pathbuf, 0);
}

int
main(int argc, char *argv[])
{
	int i;
	char c;

	format = default_format;
	ordering = default_ordering;
	argv0 = argv[0];

	while ((c = getopt(argc, argv, "01DFHLQUdf:lo:st:x")) != -1)
		switch(c) {
		case '0': format = zero_format; Qflag++; break;
		case '1': expr = chain(expr, EXPR_AND, parse_expr("depth == 0 || prune")); break;
		case 'D': Dflag++; break;
		case 'F': format = type_format; break;
		case 'H': Hflag++; break;
		case 'L': Lflag++; break;
		case 'Q': Qflag++; break;
		case 'U': Uflag++; break;
		case 'd': expr = chain(expr, EXPR_AND, parse_expr("type == d && prune || print")); break;
		case 'f': format = optarg; break;
		case 'l': lflag++; format = long_format; break;
		case 'o': ordering = optarg; break;
		case 's': sflag++; break;
		case 't': expr = chain(expr, EXPR_AND, parse_expr(optarg)); break;
		case 'x': xflag++; break;
		default:
			fprintf(stderr, "Usage: %s [-0|-F|-l|-f FMT] [-D] [-H|-L] [-1Qdsx] [-U|-o ORD] [-t TEST]* PATH...\n", argv0);
			exit(2);
		}

	if (optind == argc) {
		sflag++;
		traverse(".");
	} else {
		for (i = optind; i < argc; i++)
			traverse(argv[i]);
	}

	if (!Uflag)
		twalk(root, print);

	return 0;
}
