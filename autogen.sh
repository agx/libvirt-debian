#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname "$0"`
test -z "$srcdir" && srcdir=.

THEDIR=`pwd`
cd "$srcdir"
DIE=0

(autopoint --version) < /dev/null > /dev/null 2>&1 || {
        echo
        echo "You must have autopoint installed to compile libvirt."
        echo "Download the appropriate package for your distribution,"
        echo "or see http://www.gnu.org/software/gettext"
        DIE=1
}

(autoconf --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have autoconf installed to compile libvirt."
	echo "Download the appropriate package for your distribution,"
	echo "or see http://www.gnu.org/software/autoconf"
	DIE=1
}

(libtool --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have libtool installed to compile libvirt."
	echo "Download the appropriate package for your distribution,"
	echo "or see http://www.gnu.org/software/libtool"
	DIE=1
}

(automake --version) < /dev/null > /dev/null 2>&1 || {
	echo
	DIE=1
	echo "You must have automake installed to compile libvirt."
	echo "Download the appropriate package for your distribution,"
	echo "or see http://www.gnu.org/software/automake"
}

if test "$DIE" -eq 1; then
	exit 1
fi

test -f src/libvirt.c || {
	echo "You must run this script in the top-level libvirt directory"
	exit 1
}


EXTRA_ARGS=
if test "x$1" = "x--system"; then
    shift
    EXTRA_ARGS="--prefix=/usr --sysconfdir=/etc --localstatedir=/var"
    echo "Running ./configure with $EXTRA_ARGS $@"
else
    if test -z "$*" && test ! -f "$THEDIR/config.status"; then
        echo "I am going to run ./configure with no arguments - if you wish "
        echo "to pass any to it, please specify them on the $0 command line."
    fi
fi

# Compute the hash we'll use to determine whether rerunning bootstrap
# is required.  The first is just the SHA1 that selects a gnulib snapshot.
# The second ensures that whenever we change the set of gnulib modules used
# by this package, we rerun bootstrap to pull in the matching set of files.
bootstrap_hash()
{
    git submodule status | sed 's/^[ +-]//;s/ .*//'
    git hash-object bootstrap.conf
}

# Ensure that whenever we pull in a gnulib update or otherwise change to a
# different version (i.e., when switching branches), we also rerun ./bootstrap.
curr_status=.git-module-status
t=$(bootstrap_hash; git diff .gnulib)
if test "$t" = "$(cat $curr_status 2>/dev/null)"; then
    : # good, it's up to date, all we need is autoreconf
    autoreconf -if
else
    echo running bootstrap...
    ./bootstrap && bootstrap_hash > $curr_status \
      || { echo "Failed to bootstrap gnulib, please investigate."; exit 1; }
fi

cd "$THEDIR"

if test "x$OBJ_DIR" != x; then
    mkdir -p "$OBJ_DIR"
    cd "$OBJ_DIR"
fi

if test -z "$*" && test -f config.status; then
    ./config.status --recheck
else
    $srcdir/configure $EXTRA_ARGS "$@"
fi && {
    echo
    echo "Now type 'make' to compile libvirt."
}
