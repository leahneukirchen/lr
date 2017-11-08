## lr: list files, recursively

`lr` is a new tool for generating file listings, which includes the
best features of `ls(1)`, `find(1)`, `stat(1)` and `du(1)`.

`lr` has been tested on Linux 4.1, FreeBSD 10.2, OpenBSD 5.7,
NetBSD 5.2.3, DragonFlyBSD 5.0, Mac OS X 10.10, OmniOS 5.11 and Cygwin 1.7.32.
It will likely work on other Unix-like systems with C99, but you'll
need to port `scan_filesystems` for `fstype` to work.

## Screenshot

![Screenshot of lr -AFGl -ovh](lr.png)

## Benefits

Over find:
* friendly and logical C-style filter syntax
* getopt is used, can mix filters and arguments in any order
* can sort
* compute directory sizes
* can strip leading `./`
* can do breadth first search

Over ls:
* sorts over all files, not per directory
* copy & paste file names from the output since they are relative to pwd
* ISO dates
* powerful filters

## Rosetta stone

* `ls`: `lr -1 | column`
* `find .`: `lr` (or `lr -U` for speed.)
* `ls -l`: `lr -1l`
* `ls -ltrc`: `lr -l1Aoc`
* `find . -name '*.c'`: `lr -t 'name ~~ "*.c"'`
* `find . -regex '.*c'`: `lr -t 'path =~ "c$"'`
* `find -L /proc/*/fd -maxdepth 1 -type f -links 0 -printf '%b %p\n'`:
`lr -UL1 -t 'type == f && links == 0' -f '%b %p\n' /proc/*/fd`
* `find "${@:-.}" -name HEAD -execdir sh -c 'git rev-parse --resolve-git-dir . >/dev/null 2>/dev/null && pwd' ';'`: `lr -0U -t 'name == "HEAD"' "$@" | xe -0 -s 'cd ${1%/*} && git rev-parse --resolve-git-dir . >/dev/null && pwd; true' 2>/dev/null`
* Filter list of files for existence: `xe lr -dQU <list`
* replacement for who(1): `lr -om -t 'name =~ "[0-9][0-9]*$" && uid != 0' -f '%u\t%p\t%CY-%Cm-%Cd %CH:%CM\n' /dev/pts /dev/tty*`
* Find files with setuid or setgid: `lr -t 'mode | 06000' /usr/bin` or `lr -t 'mode = "u+s" || mode = "g+s"' /usr/bin`
* Find files with non-umask permissions: `lr -t '!(mode = "=rw,+X")' -l`
* Find broken symlinks: `lr -L -t 'type = l'`

## Usage:

	lr [-0|-F|-l [-TA|-TC|-TM]|-S|-f FMT] [-B|-D] [-H|-L] [-1AGQXdhsx] [-U|-o ORD] [-e REGEX]* [-t TEST]* PATH...

The special path argument `-` makes `lr` read file names from standard
input, instead of traversing path.

* `-0`: output filenames separated by NUL bytes.
  Likewise, read input filenames separated by NUL bytes.
* `-F`: output filenames and an indicator of their file type (`*/=>@|`).
* `-l`: long output a la `ls -l` (implies `-Q`).
* `-TA`: with `-l`, output atime.
* `-TC`: with `-l`, output ctime.
* `-TM`: with `-l`, output mtime (default).
* `-S`: BSD stat(1)-inspired output (implies `-Q`).
* `-f FMT`: custom formatting, see below.
* `-B`: breadth first traversal.
* `-D`: depth first traversal. `prune` does not work, but `entries`
  and `total` are computed on the fly.
* `-H`: only follow symlinks on command line.
* `-L`: follow all symlinks.
* `-1`: don't go below one level of directories.
* `-A`: don't list files starting with a dot.
* `-G`: colorize output to tty.  Use twice to force colorize.
* `-X`: print OSC 8 hyperlinks to tty.  Use twice to force.
* `-Q`: shell quote file names (default for output to TTY).
* `-d`: don't enter directories.
* `-h`: print human readable size for `-l` (also `%s`).
* `-s`: strip directory prefix passed on command line.
* `-x`: don't enter other filesystems.
* `-U`: don't sort results.
* `-o ORD`: sort according to the string `ORD`, see below.
* `-e REGEX`: only show files where basename matches `REGEX`.
* `-t TEST`: only show files matching all `TEST`s, see below.

## Output formatting:

* `\a`, `\b`, `\f`, `\n`, `\r`, `\v`, `\0` as in C.
* `%%`: plain `%`.
* `%s`: file size in bytes.
* `%S`: file size, with human readable unit.
* `%b`: file size in 512-byte blocks.
* `%k`: file size in 1024-byte blocks.
* `%d`: path depth.
* `%D`: device number (`stat.st_dev`).
* `%R`: device ID for special files (`stat.st_rdev`).
* `%i`: inode number.
* `%I`: one space character for every depth level.
* `%p`: full path (`%P` if `-s`).
* `%P`: full path without command line argument prefix.
* `%l`: symlink target.
* `%n`: number of hardlinks.
* `%F`: file indicator type symbol (`*/=>@|`).
* `%f`: file basename (everything after last `/`).
* `%A-`, `%C-`, `%T-`: relative age for atime/ctime/mtime.
* `%Ax`, `%Cx`, `%Tx`: result of `strftime` for `%x` on atime/ctime/mtime.
* `%m`: octal file permissions.
* `%M`: ls-style symbolic file permissions.
* `%y`: ls-style symbolic file type (`bcdfls`).
* `%g`: group name.
* `%G`: numeric gid.
* `%u`: user name.
* `%U`: numeric uid.
* `%e`: number of entries in directories.
* `%t`: total size used by accepted files in directories (only with `-D`).
* `%Y`: type of the filesystem the file resides on.
* `%x`: Linux-only: a combination of: `#` for files with security capabilities, `+` for files with an ACL, `@` for files with other extended attributes.

## Sort order

Sort order is string consisting of the following letters.
Uppercase letters reverse sorting.
E.g. `Sn` sorts first by size, smallest last, and then by name (in
case sizes are equal).

Default: `n`.

* `a`: atime.
* `c`: ctime.
* `d`: path depth.
* `e`: file extension.
* `i`: inode number.
* `m`: mtime.
* `n`: file name.
* `p`: directory name.
* `s`: file size.
* `t`: file type.  This sorts all directories before other files.
* `v`: file name as version numbers (sorts "2" before "10").

## Filter expressions

`lr` filters are given by the following EBNF:

	<expr>     ::= <expr> || <expr>  -- disjunction
	             | <expr> && <expr>  -- conjunction
	             | <expr> ? <expr> : <expr>  -- ternary operator
	             | ! <expr>          -- negation
	             | ( <expr )
	             | <timeprop> <numop> <dur>
	             | <numprop> <numop> <num>
	             | <strprop> <strop> <str>
	             | <typetest>
	             | <modetest>
	             | prune             -- do not traverse into subdirectories
	             | print             -- always true value
	             | color <num>       -- always true value, override 256-color

        <timeprop> ::= atime | ctime | mtime
	
	<numprop>  ::= depth | dev | entries | gid | inode
	             | links | mode | rdev | size | total | uid
	
	<numop>    ::= <= | < | >= | > | == | = | !=

        <dur>      ::= "./path"          -- mtime of relative path
                     | "/path"           -- mtime of absolute path
                     | "YYYY-MM-DD HH:MM:SS"
                     | "YYYY-MM-DD"      -- at midnight
                     | "HH:MM:SS"        -- today
                     | "HH:MM"           -- today
                     | "-[0-9]+d"        -- n days ago at midnight
                     | "-[0-9]+h"        -- n hours before now
                     | "-[0-9]+m"        -- n minutes before now
                     | "-[0-9]+s"        -- n seconds before now
                     | [0-9]+            -- absolute epoch time
	
	<num>      ::= [0-9]+ ( c        -- *1
	                      | b        -- *512
	                      | k        -- *1024
	                      | M        -- *1024*1024
	                      | G        -- *1024*1024*1024
	                      | T )?     -- *1024*1024*1024*1024
	
	<strprop>  ::= fstype | group | name | path | target | user | xattr
	
	<strop>    ::= == | = | !=       -- string (in)equality
	             | ===    | !===     -- case insensitive string (in)equality
	             | ~~     | !~~      -- glob (fnmatch)
	             | ~~~    | !~~~     -- case insensitive glob (fnmatch)
	             | =~     | !=~ | !~ -- POSIX Extended Regular Expressions
	             | =~~    | !=~~     -- case insensitive POSIX Extended Regular Expressions
	
	<str>      ::= " ([^"] | "")+ "  -- use "" for a single " inside "
	             | $[A-Za-z0-9_]     -- environment variable

	<typetest> ::= type ( == | = | != ) ( b | c | d | p | f | l )

	<modetest> ::= mode ( == | =     -- exact permissions
	                    | &          -- check if all bits of <octal> set
	                    | |          -- check if any bit of <octal> set
	                    ) <octal>
	             | mode = "<chmod>"  -- check if symbolic mode is satisfied
	
	<octal> ::= [0-7]+

	<chmod> ::= <clause> (, <clause>)+

	<clause> ::= [guoa]* [+-=] [rwxXstugo]*  -- see chmod(1)

## EWONTFIX

The following features won't be implemented:

* `-exec`: use `-0` and `xargs`
  (or even better [xe](https://github.com/chneukirchen/xe)).
* columns: use `column`, `git-column` (supports colors), Plan 9 `mc`.
  (e.g. `lr -1AGFs | git column --mode=dense --padding=2`)

## "Screenshots"

Default output, sorted by name:

```
% lr
.
.git
.git/HEAD
.git/config
[...]
Makefile
README.md
lr.c
```

Long output format:

```
% lr -l
drwxrwxr-x 3 chris users   120 2015-10-27 13:56 ./
drwxrwxr-x 7 chris users   240 2015-10-27 13:56 .git/
-rw-rw-r-- 1 chris users    23 2015-10-27 13:56 .git/HEAD
-rw-rw-r-- 1 chris users   257 2015-10-27 13:56 .git/config
[...]
-rw-rw-r-- 1 chris users   297 2015-10-27 13:56 Makefile
-rw-rw-r-- 1 chris users  5828 2015-10-27 13:56 README.md
-rw-rw-r-- 1 chris users 27589 2015-10-27 13:56 lr.c
```

Simple test:

```
% lr -F -t 'type == d'
./
.git/
.git/hooks/
.git/info/
.git/logs/
.git/logs/refs/
.git/logs/refs/heads/
.git/logs/refs/remotes/
.git/logs/refs/remotes/origin/
.git/objects/
.git/objects/info/
.git/objects/pack/
.git/refs/
.git/refs/heads/
.git/refs/remotes/
.git/refs/remotes/origin/
.git/refs/tags/
```

List regular files by size, largest first:

```
% lr -f '%S %f\n' -1 -t 'type == f' -oS
  27K lr.c
 5.7K README.md
  297 Makefile
```

List directory total sizes, indented:

```
% lr -D -t 'type == d' -f '%I%I%t %p\n'
172 .
  132 .git
    40 .git/hooks
    4 .git/info
    12 .git/logs
      8 .git/logs/refs
        4 .git/logs/refs/heads
        4 .git/logs/refs/remotes
          4 .git/logs/refs/remotes/origin
    48 .git/objects
      0 .git/objects/info
      48 .git/objects/pack
    8 .git/refs
      4 .git/refs/heads
      4 .git/refs/remotes
        4 .git/refs/remotes/origin
      0 .git/refs/tags
```

List all files, but print them in red if they match "havoc":

```
% lr -G -t 'name =~ "havoc" && color 160 || print'
```

Do not enter `.git` or `.hg` directories:

```
% lr -t 'name = ".git" || name = ".hg" ? prune : print' .
```

## Installation

Use `make all` to build, `make install` to install relative to `PREFIX`
(`/usr/local` by default).  The `DESTDIR` convention is respected.
You can also just copy the binary into your `PATH`.

## Copyright

Copyright (C) 2015-2017 Leah Neukirchen <purl.org/net/chneukirchen>

Licensed under the terms of the MIT license, see lr.c.
