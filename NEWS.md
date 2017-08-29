## HEAD

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
