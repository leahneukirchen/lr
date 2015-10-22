// lr - a better recursive ls/find
/*
##% gcc -Os -Wall -g -o $STEM $FILE -Wno-switch -Wextra -Wwrite-strings
*/

/*
TODO:
- expression parsing
- %NNx formatting strings
- dynamic columns? i'd rather not (easy to compute for all but names...)
- error handling? keep going
- avoid stat in recurse
- multiple -t
- don't default to ./ prefix
*/

#define _XOPEN_SOURCE 700

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int dflag = 0;
int Hflag = 0;
int Lflag = 0;
int xflag = 0;
int Uflag = 0;

char *format;
char *ordering;
struct expr *e;
void *root = NULL; // tree
static int prune;

struct fileinfo {
	char *fpath;
	int depth;
	struct stat sb;
};

/*
>> (false && true) || true
=> true
>> false && true || true
=> true

  - expr && expr
  - expr || expr
  - (expr)
  - value > < >= <= == !=
  - !expr
  - prune?
*/

enum op {
	EXPR_OR = 1,
	EXPR_AND,
	EXPR_NOT,
	EXPR_LT,
	EXPR_LE,
	EXPR_EQ,
	EXPR_GE,
	EXPR_GT,
	EXPR_GLOB,
	EXPR_PRUNE,
	EXPR_TYPE,
};

enum prop {
	PROP_ATIME = 1,
	PROP_CTIME,
	PROP_DEPTH,
	PROP_DEV,
//	PROP_ENTRIES,
	PROP_INODE,
	PROP_LINKS,
	PROP_MODE,
	PROP_MTIME,
	PROP_NAME,
	PROP_PATH,
	PROP_SIZE,
//	PROP_TARGET
//	PROP_TOTAL
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
		long num;
	} a, b;
};

static char *pos;

void
ws()
{
	while (isspace(*pos))
		pos++;
}

int
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

int
parse_num(long *r)
{
	// TODO -negative
	// TODO octal?
	// TODO postfix G M K?
	if (isdigit(*pos)) {
		long n = 0;
		while (isdigit(*pos)) {
			n *= 10;
			n += *pos - '0';
			pos++;
		}
		ws();
		*r = n;
		return 1;
	} else {
		return 0;
	}
}

enum op
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
	// TODO glob
	// TODO re

	return 0;
}

struct expr *parse_cmp();
struct expr *parse_or();

struct expr *
parse_inner()
{
	if (token("prune")) {
		struct expr *e = malloc (sizeof (struct expr));
		e->op = EXPR_PRUNE;
		return e;
	} else if (token("!")) {
		struct expr *e = parse_cmp();
		struct expr *not = malloc (sizeof (struct expr));
		not->op = EXPR_NOT;
		not->a.expr = e;
		return not;
	} else if (token("(")) {
		struct expr *e = parse_or();
		if (token(")"))
			return e;
		return 0; // TODO ERROR;
	} else
		return 0;
}

struct expr *
parse_type()
{
	if (token("type")) {
		if(token("==")) {  // TODO !=
			struct expr *e = malloc (sizeof (struct expr));
			e->op = EXPR_TYPE;
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
				return 0;  // TODO ERROR
			return e;
		}
	}

	return parse_inner();
}

struct expr *
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
	else if (token("inode"))
		prop = PROP_INODE;
	else if (token("links"))
		prop = PROP_LINKS;
	else if (token("mode"))
		prop = PROP_MODE;
	else if (token("mtime"))
		prop = PROP_MTIME;
	else if (token("size"))
		prop = PROP_SIZE;
	else
		return parse_type();
	
	op = parse_op();
	if (!op)
		return 0;  // TODO ERROR

	long n;
	if (parse_num(&n)) {
		struct expr *e = malloc(sizeof (struct expr));
		e->op = op;
		e->a.prop = prop;
		e->b.num = n;
		return e;
	}

	return 0;
}

struct expr *
parse_and()
{
	struct expr *e1 = parse_cmp();
	struct expr *r = e1;
	struct expr *l;

	if (token("&&")) {
		struct expr *and = malloc(sizeof (struct expr));
		struct expr *e2 = parse_and();
		and->op = EXPR_AND;
		and->a.expr = r;
		and->b.expr = e2;
		r = and;
		l = and;
	}
	while (token("&&")) {
		struct expr *e2 = parse_and();
		struct expr *and = malloc(sizeof (struct expr));
		and->op = EXPR_AND;
		and->a.expr = l->b.expr;
		and->b.expr = e2;
		l->b.expr = and;
	}
	
	return r;
}

struct expr *
parse_or()
{
	struct expr *e1 = parse_and();
	struct expr *r = e1;
	struct expr *l;

	if (token("||")) {
		struct expr *or = malloc(sizeof (struct expr));
		struct expr *e2 = parse_and();
		or->op = EXPR_OR;
		or->a.expr = r;
		or->b.expr = e2;
		r = or;
		l = or;
	}
	while (token("||")) {
		struct expr *e2 = parse_and();
		struct expr *or = malloc(sizeof (struct expr));
		or->op = EXPR_OR;
		or->a.expr = l->b.expr;
		or->b.expr = e2;
		l->b.expr = or;
	}
	
	return r;
}

struct expr *
parse_expr(char *s)
{
	pos = s;
	return parse_or();
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
	case EXPR_LT:
	case EXPR_LE:
	case EXPR_EQ:
	case EXPR_GE:
	case EXPR_GT:
	{
		long v = 0;
		switch (e->a.prop) {
		case PROP_ATIME: v = fi->sb.st_atime; break;
		case PROP_CTIME: v = fi->sb.st_ctime; break;
		case PROP_DEPTH: v = fi->depth; break;
		case PROP_DEV: v = fi->sb.st_dev; break;
		case PROP_INODE: v = fi->sb.st_ino; break;
		case PROP_LINKS: v = fi->sb.st_nlink; break;
		case PROP_MODE: v = fi->sb.st_mode; break;
		case PROP_MTIME: v = fi->sb.st_mtime; break;
		case PROP_SIZE: v = fi->sb.st_size; break;
		default:
			return 0;  // TODO ERROR
		}

		switch (e->op) {
		case EXPR_LT: return v < e->b.num;
		case EXPR_LE: return v <= e->b.num;
		case EXPR_EQ: return v == e->b.num;
		case EXPR_GE: return v >= e->b.num;
		case EXPR_GT: return v > e->b.num;
		}
	}
	case EXPR_TYPE:
	{
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
	}

	return 0;
}

const char *
basenam(const char *s)
{
	char *r = strrchr(s, '/');
	return r ? r + 1 : s;
}

const char *
readlin(const char *p, const char *alt)
{
	static char b[PATH_MAX];
	int r = readlink(p, b, sizeof b - 1);
	if (r < 0)
		return alt;
	b[r] = 0;
	return b;
}

#define CMP(a, b) if ((a) == (b)) break; else if ((a) < (b)) return -1; else return 1
#define STRCMP(a, b) if (strcmp(a, b) == 0) break; else return (strcmp(a, b));

int
order(const void *a, const void *b)
{
	struct fileinfo *fa = (struct fileinfo *) a;
	struct fileinfo *fb = (struct fileinfo *) b;
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
		case 'd': CMP(fa->depth, fb->depth);
		case 'D': CMP(fb->depth, fa->depth);
		case 'n': STRCMP(fa->fpath, fb->fpath);
		case 'N': STRCMP(fb->fpath, fa->fpath);
		default: STRCMP(fa->fpath, fb->fpath);
		}
	}

	return strcmp(fa->fpath, fb->fpath);
}

void
print(const void *nodep, const VISIT which, const int depth)
{
	if (which == postorder || which == leaf) {
		struct fileinfo *fi = *(struct fileinfo **) nodep;
//		printf("%d %s\n", depth, fi->fpath);

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
				case 's': printf("%ld", fi->sb.st_size); break;
				case 'b': printf("%ld", fi->sb.st_blocks); break;
				case 'k': printf("%ld", fi->sb.st_blocks/2); break;
				case 'd': printf("%d", fi->depth); break;
				case 'D': printf("%ld", fi->sb.st_dev); break;
				case 'i': printf("%ld", fi->sb.st_ino); break;
				case 'p': printf("%s", fi->fpath); break;
				case 'l': printf("%s", readlin(fi->fpath, "")); break;
				case 'L': if (S_ISLNK(fi->sb.st_mode))
						printf(" -> "); break;
				case 'n': printf("%ld", fi->sb.st_nlink); break;
				case 'F':
					if (S_ISDIR(fi->sb.st_mode)) {
						putchar('/');
					} else if (S_ISSOCK(fi->sb.st_mode)) {
						putchar('=');
					} else if (S_ISFIFO(fi->sb.st_mode)) {
						putchar('|');
					} else if (S_ISLNK(fi->sb.st_mode)) {
						putchar('@');
					} else if (fi->sb.st_mode & S_IXUSR) {
						putchar('*');
					}
					break;
				case 'f': printf("%s", basenam(fi->fpath)); break;
				case 'A':
				case 'C':
				case 'M':
					{
					char tfmt[3] = "%\0\0";
					char buf[256];
					s++;
					if (!*s) break;
					tfmt[1] = *s;
					strftime(buf, sizeof buf, tfmt,
					    localtime(
					    *s == 'A' ? &fi->sb.st_atime :
					    *s == 'C' ? &fi->sb.st_ctime :
					    &fi->sb.st_mtime));
					printf("%s", buf);
					break;
					}
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
callback(const char *fpath, const struct stat *sb, int depth)
{
	struct fileinfo *fi = malloc (sizeof (struct fileinfo));
	fi->fpath = strdup(fpath);
	fi->depth = depth;
	memcpy((char *) &fi->sb, (char *) sb, sizeof (struct stat));

	if (e && !eval(e, fi))
		return 0;

	if (Uflag)
		print(&fi, postorder, depth);
	else
		// add to the tree, note that this will elimnate duplicate files
		tsearch(fi, &root, order);

	return 0;
}

// lifted from musl nftw.
struct history
{
        struct history *chain;
        dev_t dev;
        ino_t ino;
        int level;
};
static int
recurse(char *path, struct history *h)
{
        size_t l = strlen(path), j = l && path[l-1]=='/' ? l-1 : l;
        struct stat st;
        struct history new;
        int r;

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
        new.level = h ? h->level+1 : 0;
        
        if (!dflag) {
		prune = 0;
		r = callback(path, &st, new.level);
		if (prune)
			return 0;
		if (r)
			return r;
	}

        for (; h; h = h->chain)
                if (h->dev == st.st_dev && h->ino == st.st_ino)
                        return 0;

        if (S_ISDIR(st.st_mode)) {
                DIR *d = opendir(path);
                if (d) {
                        struct dirent *de;
                        while ((de = readdir(d))) {
                                if (de->d_name[0] == '.'
                                 && (!de->d_name[1]
                                  || (de->d_name[1]=='.'
                                   && !de->d_name[2]))) continue;
                                if (strlen(de->d_name) >= PATH_MAX-l) {
                                        errno = ENAMETOOLONG;
                                        closedir(d);
                                        return -1;
                                }
                                path[j]='/';
                                strcpy(path+j+1, de->d_name);
                                if ((r=recurse(path, &new))) {
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
        if (dflag && (r=callback(path, &st, new.level)))
                return r;

        return 0;
}

int traverse(const char *path)
{
	char pathbuf[PATH_MAX+1];
	size_t l = strlen(path);
        if (l > PATH_MAX) {
                errno = ENAMETOOLONG;
                return -1;
        }
        memcpy(pathbuf, path, l+1);
	return recurse(pathbuf, 0);
}

int
main(int argc, char *argv[])
{
	int i;
	char c;

	format = "%p\\n";
	ordering = "n";

	while ((c = getopt(argc, argv, "df:t:o:xLHUl0")) != -1)
		switch(c) {
		case 'd': dflag++; break;
		case 'f': format = optarg; break;
		case 'o': ordering = optarg; break;
		case 't': e = parse_expr(optarg); break;
		case 'x': xflag++; break;
		case 'H': Hflag++; break;
		case 'L': Lflag++; break;
		case 'U': Uflag++; break;
		case '0': format = "%p\\0"; break;
		case 'l':
			// todo: print symlinks
			// "%M %2n %u %g %9s %TY-%Tm-%Td %TH:%TM %p%F\n"; break;
			format = "%M %n %u %g %s %MY-%Mm-%Md %MH:%MM %p%F%L%l\n"; break;
		default:
			fprintf(stderr, "Usage: %s [-oxL] PATH...\n", argv[0]);
			exit(2);
		}
	
	if (optind == argc)
		traverse(".");
	for (i = optind; i < argc; i++)
		traverse(argv[i]);

	if (!Uflag)
		twalk(root, print);

	return 0;
}
