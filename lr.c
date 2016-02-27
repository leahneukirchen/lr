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

#ifdef __linux__
#include <attr/xattr.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <regex.h>
#include <search.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* For Solaris. */
#if !defined(FNM_CASEFOLD) && defined(FNM_IGNORECASE)
#define FNM_CASEFOLD FNM_IGNORECASE
#endif

static int Gflag;
static int Dflag;
static int Hflag;
static int Lflag;
static int Qflag;
static int Uflag;
static int hflag;
static int lflag;
static int sflag;
static int xflag;
static char Tflag = 'T';

static char *argv0;
static char *format;
static char *ordering;
static struct expr *expr;
static void *root = NULL; // tree
static int prune;
static size_t prefixl;

static char default_ordering[] = "n";
static char default_format[] = "%p\\n";
static char type_format[] = "%p%F\\n";
static char long_format[] = "%M%x %n %u %g %s %\324F %\324R %p%F%l\n";
static char zero_format[] = "%p\\0";
static char stat_format[] = "%D %i %M %n %u %g %R %s \"%Ab %Ad %AT %AY\" \"%Tb %Td %TT %TY\" \"%Cb %Cd %CT %CY\" %b %p\n";

static void *users;
static void *groups;
static void *filesystems;
static int scanned_filesystems;

struct idmap {
	long id;
	char *name;
};

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

static int maxdepth;
static int uwid, gwid, fwid;

static time_t now;

struct fileinfo {
	char *fpath;
	size_t prefixl;
	int depth;
	ino_t entries;
	struct stat sb;
	off_t total;
	char xattr[4];
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
	} a, b;
};

static char *pos;

static void
parse_error(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	fprintf(stderr, "%s: parse error: ", argv0);
	vfprintf(stderr, msg, ap);
	fprintf(stderr, "\n");
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
	while (isspace((unsigned char) *pos))
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
	if (isdigit((unsigned char) *pos)) {
		int64_t n;

		for (n = 0; isdigit((unsigned char) *pos) && n <= INT64_MAX / 10 - 10; pos++)
			n = 10 * n + (*pos - '0');
		if (isdigit((unsigned char) *pos))
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
			if (len >= bufsiz) {
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
		*s = buf ? buf : (char *) "";
		return 1;
	} else if (*pos == '$') {
		char t;
		char *e = ++pos;

		while (isalnum((unsigned char) *pos) || *pos == '_')
			pos++;
		if (e == pos)
			parse_error("invalid environment variable name");

		t = *pos;
		*pos = 0;
		*s = getenv(e);
		if (!*s)
			*s = (char *) "";
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
parse_strcmp()
{
	enum prop prop;
	enum op op;

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

	if (token("==="))
		op = EXPR_STREQI;
	else if (token("=="))
		op = EXPR_STREQ;
	else if (token("="))
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
		parse_error("invalid string operator at '%.15s'", pos);

	char *s;
	if (parse_string(&s)) {
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

		return e;
	}

	parse_error("invalid string at '%.15s'", pos);
	return 0;
}

static struct expr *
parse_mode()
{
	struct expr *e = mkexpr(0);
	long n;

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
parse_expr(const char *s)
{
	pos = (char *)s;
	struct expr *e = parse_or();
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
	snprintf(buf, sizeof buf, "%ld", id);
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
	if (!mtab)
		return;

	while ((mnt = getmntent(mtab))) {
		if (stat(mnt->mnt_dir, &st) < 0)
			continue;

		struct idmap *newkey = malloc(sizeof (struct idmap));
		newkey->id = st.st_dev;
		newkey->name = strdup(mnt->mnt_type);
		tsearch(newkey, &filesystems, idorder);
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

		struct idmap *newkey = malloc(sizeof (struct idmap));
		newkey->id = st.st_dev;
		newkey->name = strdup(mnt.mnt_fstype);
		tsearch(newkey, &filesystems, idorder);
	};

	fclose(mtab);

	scanned_filesystems = 1;
}
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
void
scan_filesystems()
{
	struct statfs *mnt;
	struct stat st;
	int i = getmntinfo(&mnt, MNT_NOWAIT);

	while (i-- > 0) {
		if (stat(mnt->f_mntonname, &st) < 0)
			continue;

		struct idmap *newkey = malloc(sizeof (struct idmap));
		newkey->id = st.st_dev;
		newkey->name = strdup(mnt->f_fstypename);
		tsearch(newkey, &filesystems, idorder);
		mnt++;
	};

	scanned_filesystems = 1;
}
#elif defined(__NetBSD__)
#include <sys/types.h>
#include <sys/statvfs.h>
void
scan_filesystems()
{
	struct statvfs *mnt;
	struct stat st;
	int i = getmntinfo(&mnt, MNT_NOWAIT);

	while (i-- > 0) {
		if (stat(mnt->f_mntonname, &st) < 0)
			continue;

		struct idmap *newkey = malloc(sizeof (struct idmap));
		newkey->id = st.st_dev;
		newkey->name = strdup(mnt->f_fstypename);
		tsearch(newkey, &filesystems, idorder);
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
	struct idmap key, **result;
	key.id = devid;
	key.name = 0;

	if (!scanned_filesystems)
		scan_filesystems();
	result = tfind(&key, &filesystems, idorder);

	if (result && (int)strlen((*result)->name) > fwid)
		fwid = strlen((*result)->name);

	return result ? (*result)->name : strid(devid);
}

static char*
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
	// ignoring ENOTSUP or r == 0

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
	return "";		// No support for xattrs on this platform.
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

	d = opendir(fi->fpath);
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' &&
		    (!de->d_name[1] || (de->d_name[1]=='.' && !de->d_name[2])))
			continue;
		c++;
	}
	closedir(d);

	return c;
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
		case PROP_FSTYPE: s = fstype(fi->sb.st_dev); break;
		case PROP_GROUP: s = groupname(fi->sb.st_gid); break;
		case PROP_NAME: s = basenam(fi->fpath); break;
		case PROP_PATH: s = fi->fpath; break;
		case PROP_TARGET: s = readlin(fi->fpath, ""); break;
		case PROP_USER: s = username(fi->sb.st_uid); break;
		case PROP_XATTR: s = xattr_string(fi->fpath); break;
		}
		switch (e->op) {
		case EXPR_STREQ: return strcmp(e->b.string, s) == 0;
		case EXPR_STREQI: return strcasecmp(e->b.string, s) == 0;
		case EXPR_GLOB: return fnmatch(e->b.string, s, 0) == 0;
		case EXPR_GLOBI: return fnmatch(e->b.string, s, FNM_CASEFOLD) == 0;
		case EXPR_REGEX:
		case EXPR_REGEXI: return regexec(e->b.regex, s, 0, 0, 0) == 0;
		}
	}
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

// taken straight from musl@a593414
int mystrverscmp(const char *l0, const char *r0)
{
	const unsigned char *l = (const void *)l0;
	const unsigned char *r = (const void *)r0;
	size_t i, dp, j;
	int z = 1;

	/* Find maximal matching prefix and track its maximal digit
	 * suffix and whether those digits are all zeros. */
	for (dp=i=0; l[i]==r[i]; i++) {
		int c = l[i];
		if (!c) return 0;
		if (!isdigit(c)) dp=i+1, z=1;
		else if (c!='0') z=0;
	}

	if (l[dp]!='0' && r[dp]!='0') {
		/* If we're not looking at a digit sequence that began
		 * with a zero, longest digit string is greater. */
		for (j=i; isdigit(l[j]); j++)
			if (!isdigit(r[j])) return 1;
		if (isdigit(r[j])) return -1;
	} else if (z && dp<i && (isdigit(l[i]) || isdigit(r[i]))) {
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
		case 'p': DIRCMP(fa->fpath, fb->fpath);
		case 'P': DIRCMP(fb->fpath, fa->fpath);
		case 'v': VERCMP(fa->fpath, fb->fpath);
		case 'V': VERCMP(fb->fpath, fa->fpath);
		default: STRCMP(fa->fpath, fb->fpath);
		}
	}

	return strcmp(fa->fpath, fb->fpath);
}

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
	putchar(mode & 01000 ? (mode & 00001 ? 'T' : 't')
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

	if	(s <		  1024LL) c = 46;
	else if (s <		4*1024LL) c = 82;
	else if (s <	       16*1024LL) c = 118;
	else if (s <	       32*1024LL) c = 154;
	else if (s <	      128*1024LL) c = 190;
	else if (s <	      512*1024LL) c = 226;
	else if (s <	     1024*1024LL) c = 220;
	else if (s <	 700*1024*1024LL) c = 214;
	else if (s <  2*1048*1024*1024LL) c = 208;
	else if (s < 50*1024*1024*1024LL) c = 202;
	else				  c = 196;

	fg256(c);
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

static void
print_shquoted(const char *s)
{
	if (Qflag || !strpbrk(s, "\001\002\003\004\005\006\007\010"
	                         "\011\012\013\014\015\016\017\020"
	                         "\021\022\023\024\025\026\027\030"
	                         "\031\032\033\034\035\036\037\040"
	                         "`^#*[]=|\\?${}()'\"<>&;\177")) {
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
print_noprefix(struct fileinfo *fi)
{
	if (strlen(fi->fpath) > fi->prefixl + 1)  /* strip prefix */
		print_shquoted(fi->fpath + fi->prefixl + 1);
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
	}
}

static void
color_age_on(time_t t)
{
	time_t age = now - t;
	int c;

	if (!Gflag)
		return;

	if	(age <		     0LL) c = 196;
	else if (age <		    60LL) c = 255;
	else if (age <		 60*60LL) c = 252;
	else if (age <	      24*60*60LL) c = 250;
	else if (age <	    7*24*60*60LL) c = 244;
	else if (age <	  4*7*24*60*60LL) c = 244;
	else if (age <	 26*7*24*60*60LL) c = 242;
	else if (age <	 52*7*24*60*60LL) c = 240;
	else if (age < 2*52*7*24*60*60LL) c = 238;
	else				  c = 236;

	fg256(c);
}

static void
color_name_on(const char *f, mode_t m)
{
	const char *b;

	if (!Gflag)
		return;

	b = basenam(f);

	if (m & S_IXUSR)
		fgbold();

	if (*b == '.' || (S_ISREG(m) && b[strlen(b)-1] == '~'))
		fg256(238);
	else if (S_ISDIR(m))
		fg256(242);
	else if (S_ISREG(m) && (m & S_IXUSR))
		fg256(154);
}

// unused format codes: BEHJKLNOQVWXZ achjoqrvwz
void
print_format(struct fileinfo *fi)
{
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
			for (i=0; i<fi->depth; i++)
				printf(" ");
			break;
		}
		case 'p':
			if (!sflag) {
				color_name_on(fi->fpath, fi->sb.st_mode);
				print_shquoted(fi->fpath);
				fgdefault();
				break;
			}
			/* FALLTHRU */
		case 'P':
			color_name_on(fi->fpath, fi->sb.st_mode);
			print_noprefix(fi);
			fgdefault();
			break;
		case 'l':
			if (S_ISLNK(fi->sb.st_mode))
				print_shquoted(readlin(fi->fpath, ""));
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
			color_name_on(fi->fpath, fi->sb.st_mode);
			print_shquoted(basenam(fi->fpath));
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
				printf("%3ldd%3ldh%3ldm%3lds",
				    ((now - t) / (60*60*24)),
				    ((now - t) / (60*60)) % 24,
				    ((now - t) / 60) % 60,
				    (now - t) % 60);
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

void
print(const void *nodep, const VISIT which, const int depth)
{
	(void)depth;

	if (which == postorder || which == leaf)
		print_format(*(struct fileinfo **)nodep);
}

int
callback(const char *fpath, const struct stat *sb, int depth, int entries, off_t total)
{
	struct fileinfo *fi = malloc(sizeof (struct fileinfo));
	fi->fpath = strdup(fpath);
	fi->prefixl = prefixl;
	fi->depth = depth;
	fi->entries = entries;
	fi->total = total;
	memcpy((char *)&fi->sb, (char *)sb, sizeof (struct stat));

	if (expr && !eval(expr, fi))
		return 0;

	if (need_xattr) {
		strncpy(fi->xattr, xattr_string(fi->fpath), sizeof fi->xattr);
		if (strlen(fi->xattr) > maxxattr)
			maxxattr = strlen(fi->xattr);
	} else
		memset(fi->xattr, 0, sizeof fi->xattr);

	if (Uflag)
		print(&fi, postorder, depth);
	else
		// add to the tree, note that this will elimnate duplicate files
		tsearch(fi, &root, order);

	if (depth > maxdepth)
		maxdepth = depth;

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

	/* prefetch user/group/fs name for correct column widths. */
	if (need_user)
		username(fi->sb.st_uid);
	if (need_group)
		groupname(fi->sb.st_gid);
	if (need_fstype)
		fstype(fi->sb.st_dev);

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

	int resolve = Lflag || (Hflag && !h);

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
				if (de->d_name[0] == '.' &&
				    (!de->d_name[1] ||
				     (de->d_name[1]=='.' && !de->d_name[2])))
					continue;
				entries++;
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
	prefixl = strlen(path);
	while (path[prefixl-1] == '/')
		prefixl--;
	if (prefixl > PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(pathbuf, path, prefixl + 1);
	return recurse(pathbuf, 0);
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
	int i;
	char c;

	format = default_format;
	ordering = default_ordering;
	argv0 = argv[0];
	now = time(0);

	while ((c = getopt(argc, argv, "01ADFGHLQST:Udf:lho:st:x")) != -1)
		switch(c) {
		case '0': format = zero_format; Qflag++; break;
		case '1': expr = chain(expr, EXPR_AND, parse_expr("depth == 0 || prune")); break;
		case 'A': expr = chain(expr, EXPR_AND, parse_expr("!(path ~~ \"*/.*\" && prune) && !path == \".\"")); break;
		case 'D': Dflag++; break;
		case 'F': format = type_format; break;
		case 'G': Gflag++; break;
		case 'H': Hflag++; break;
		case 'L': Lflag++; break;
		case 'Q': Qflag++; break;
		case 'S': format = stat_format; break;
		case 'T': Tflag = timeflag(optarg); break;
		case 'U': Uflag++; break;
		case 'd': expr = chain(expr, EXPR_AND, parse_expr("type == d && prune || print")); break;
		case 'f': format = optarg; break;
		case 'h': hflag++; break;
		case 'l': lflag++; format = long_format; break;
		case 'o': ordering = optarg; break;
		case 's': sflag++; break;
		case 't': expr = chain(expr, EXPR_AND, parse_expr(optarg)); break;
		case 'x': xflag++; break;
		default:
			fprintf(stderr, "Usage: %s [-0|-F|-l [-TA|-TC|-TM]|-S|-f FMT] [-D] [-H|-L] [-1AGQdhsx] [-U|-o ORD] [-t TEST]* PATH...\n", argv0);
			exit(2);
		}

	if (Gflag == 1 && !isatty(1))
		Gflag = 0;

	analyze_format();

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
