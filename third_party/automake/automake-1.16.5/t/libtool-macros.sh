#! /bin/sh
# Copyright (C) 2011-2021 Free Software Foundation, Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Try to find the libtool '.m4' files and make them easily accessed
# to the test cases requiring them.
# See also automake bug#9807.

. test-init.sh

echo "# Automatically generated by $me." > get.sh
echo : >> get.sh

# The 'libtoolize' script will look into Makefile.am.
echo ACLOCAL_AMFLAGS = -I m4 > Makefile.am

if libtoolize --copy --install && test -f m4/libtool.m4; then
  echo "ACLOCAL_PATH='$(pwd)/m4':\$ACLOCAL_PATH" >> get.sh
  echo "export ACLOCAL_PATH" >> get.sh
else
  # Libtoolize from libtool < 2.0 didn't support the '--install' option,
  # but this doesn't mean the user hasn't made the libtool macros
  # available, e.g., by properly setting ACLOCAL_PATH.
  rm -rf m4
  mkdir m4
  echo AC_PROG_LIBTOOL >> configure.ac
  # See below for an explanation about the use the of '-Wno-syntax'.
  if $ACLOCAL -Wno-syntax -I m4 --install && test -f m4/libtool.m4; then
    : # Libtool macros already accessible by default.
  else
    echo "skip_all_ \"couldn't find or get libtool macros\"" >> get.sh
  fi
fi

. ./get.sh

$ACLOCAL --force -I m4 || cat >> get.sh <<'END'
# We need to use '-Wno-syntax', since we do not want our test suite
# to fail merely because some third-party '.m4' file is underquoted.
ACLOCAL="$ACLOCAL -Wno-syntax"
END

# The file libtoolize might have just copied in the 'm4' subdirectory of
# the test directory are going to be needed by other tests, so we must
# not remove the test directory.
keep_testdirs=yes

:
