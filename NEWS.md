## HEAD

* **Breaking change**: the `-Q` flag changed meaning to *enable* quoting
  (as it does in GNU ls), since shell quoting is not so useful in many
  cases using a pipe.  Filenames are quoted by default when printing
  to TTY.

## 0.4 (2017-04-25)

* Feature: argument '-' means read files from standard input

## 0.3.2 (2016-05-20)

* Bug: getopt was called in a wrong way from ARM platforms

## 0.3.1 (2016-03-31)

* Bug: =~ was not recognized (broken since 0.3)
* Add emacs contrib (lr.el)

## 0.3 (2016-02-28)

* Checking permissions against chmod-style symbolic modes
* `-TA`/`-TC`/`-TM` to select which timestamp to show in `-l`
* Colorize symlink targets
* Show broken links
* On Linux: Display of capabilities, ACL and xattrs in `-l`
* zsh completion
* Some small things in contrib/
