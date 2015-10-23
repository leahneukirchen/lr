## lr: list files, recursively

`lr` is a new tool for generating file listings, which includes the
best features of `ls(1)`, `find(1)` and `du(1)`.

## Usage:

	lr [-0|-F|-l|-f FMT] [-D] [-H|-L] [-1dsx] [-U|-o ORD] [-t TEST]* PATH...

* `-0`: output filenames seperated by NUL bytes.
* `-F`: output filenames and an indicator of their file type (`*/=>@|`).
* `-l`: long output ala `ls -l`.
* `-f FMT`: custom formatting, see below.
* `-D`: depth first traversal. `prune` does not work, but `entries`
  and `total` is computed.
* `-H`: only follow symlinks on command line.
* `-L`: follow all symlinks.
* `-1`: don't go below one level of directories.
* `-d`: don't enter directories.
* `-s`: don't print leading `./`.
* `-x`: don't enter other filesystems.
* `-U`: don't sort results.
* `-o ORD`: sort according to the string `ORD`, see below.
* `-t TEST`: only show files matching all `TEST`s, see below.

## Output formatting:

* `\a`, `\b`, `\f`, `\n`, `\r`, `\v`, `\0` as in C.
* `%%`: plain `%`.
* `%s`: file size in bytes.
* `%b`: file size in 512-byte blocks.
* `%k`: file size in 1024-byte blocks.
* `%d`: path depth.
* `%D`: device number (`stat.st_dev`).
* `%i`: inode number.
* `%I`: one space character for every depth level.
* `%p`: full path (without `./` if `-s`).
* `%l`: symlink target.
* `%n`: number of hardlinks.
* `%F`: file indicator type symbol (`*/=>@|`).
* `%f`: file basename (everything after last `/`).
* `%Ax`, `%Cx`, `%Tx`: result of `strftime` for `%x` on atime/ctime/mtime.
* `%m`: octal file permissions.
* `%M`: ls-style symbolic file permissions.
* `%y`: ls-style symbolic file type (`bcdfls`).
* `%g`: group name.
* `%G`: numeric gid.
* `%u`: user name.
* `%U`: numeric uid.
* `%e`: number of entries in directories (only with `-D`).
* `%t`: total size used by accepted files in directories (only with `-D`).

## Sort order

Sort order is string consisting of the following letters.
Uppercase letters reverse sorting.
E.g. `Sn` sorts first by size, smallest last, and then by name (in
case sizes are equal).

* `a`: atime.
* `c`: ctime.
* `d`: path depth.
* `i`: inode number.
* `m`: mtime.
* `n`: file name.
* `s`: file size.

## Filter expressions

`lr` filters are given by the following EBNF:

	<expr>     ::= <expr> || <expr>  -- disjunction
	             | <expr> && <expr>  -- conjunction
	             | ! <expr>          -- negation
	             | ( <expr )
	             | <numprop> <numop> <num>
	             | <strprop> <strop> <str>
	             | <typetest>
	             | <modetest>
	             | prune             -- do not traverse into subdirectories
	             | print             -- always true value
	
	<numprop>  ::= atime | ctime | depth | dev | entries | inode
	             | links | mode | mtime | size | total
	
	<numop>    ::= <= | < | >= | > | == | !=
	
	<num>      ::= [0-9]+ ( c        -- *1
	                      | b        -- *512
	                      | k        -- *1024
	                      | M        -- *1024*1024
	                      | G        -- *1024*1024*1024
	                      | T )?     -- *1024*1024*1024*1024
	
	<strprop>  ::= name | path | target
	
	<strop>    ::= ==                -- string equality
	             | ===               -- case insensitive string equality
	             | ~~                -- glob (fnmatch)
	             | ~~~               -- case insensitive glob (fnmatch)
	             | =~                -- POSIX Extended Regular Expressions
	             | =~~               -- case insensitive POSIX Extended Regular Expressions
	
	<str>      ::= " [^"]+ "

	<typetest> ::= type == ( b | c | d | p | f | l )

	<modetest> ::= mode ( ==         -- exact permissions
	                    | &          -- check if all bits of <octal> set
	                    | |          -- check if any bit of <octal> set
	                    ) <octal>
	
	<octal> ::= [0-7]+

## EWONTFIX

The following features won't be implemented:

* `-exec`: use `-0` and `xargs` (or a future replacement).
* columns: use `column`, `git-column`, Plan 9 `mc`.

## Installation

Use `make all` to build, `make install` to install relative to `PREFIX`
(`/usr/local` by default).  The `DESTDIR` convention is respected.
You can also just copy the binary into your `PATH`.

## Copyright

Copyright (C) 2015 Christian Neukirchen <purl.org/net/chneukirchen>

Licensed under the terms of the MIT license, see lr.c.
