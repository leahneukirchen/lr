## HEAD

## 1.3 (2018-04-05)

* Feature: new option `-P` to quote filenames with `$'...'` syntax.
* Feature: invalid UTF-8 filenames are quoted now.
* Feature: colorized file size output now uses groups of three digits.
* Feature: support $NO_COLOR: http://no-color.org/

## 1.2 (2017-11-17)

* Feature: new option `-B` for breadth first traversal.
* Feature: new syntax `? :` for ternary operator.
* Feature: new action `skip` which is always false.
  The common find(1) idiom `-name x -prune -o -print`
  is now best written as `name = "x" ? prune && skip : print`.
* Significant speed-up as tsearch is not used anymore.
* Lower memory usage for -U.
* Default widths for -U.

## 1.1 (2017-10-29)

* Feature: lr is substantially faster as files only are stat(2)ed if
  the output requires it.
* Feature: new option `-X` to print OSC 8 hyperlinks.
* Feature: new option `-e` for the common case of filtering file names.
* Feature: support for DragonFlyBSD.
* Bug: lr doesn't fail on symlinks refering to themselves anymore.

## 1.0 (2017-08-29)

* **Breaking change**: the `-Q` flag changed meaning to *enable* quoting
  (as it does in GNU ls), since shell quoting is not so useful in many
  cases using a pipe.  Filenames are quoted by default when printing
  to TTY.

* Feature: lr now respects the locale, which mainly influences date format.
* Feature: new option `-C` to change the color of files.
* Feature: new action `color <num>` to change the color of files.
* Feature: new argument `@file` to read file names from a file.
* Feature: negated string operations `!=`, `!===`, `!~~`, `!~`, `!=~~`.
* Bug: lr now reports errors and sets exit code when toplevel
  arguments can not be stat'ed.

## 0.4 (2017-04-25)

* Feature: argument `-` means read files from standard input

## 0.3.2 (2016-05-20)

* Bug: getopt was called in a wrong way from ARM platforms

## 0.3.1 (2016-03-31)

* Bug: `=~` was not recognized (broken since 0.3)
* Add emacs contrib (lr.el)

## 0.3 (2016-02-28)

* Checking permissions against chmod-style symbolic modes
* `-TA`/`-TC`/`-TM` to select which timestamp to show in `-l`
* Colorize symlink targets
* Show broken links
* On Linux: Display of capabilities, ACL and xattrs in `-l`
* zsh completion
* Some small things in contrib/
