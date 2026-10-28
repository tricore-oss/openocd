# SPDX-License-Identifier: GPL-2.0-or-later

SUBDIRS = tcl_commands
DIST_SUBDIRS = tcl_commands

check_PROGRAMS = flash-update
TESTS = $(check_PROGRAMS)

AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src -I$(top_builddir)/src
AM_CFLAGS = $(GCC_WARNINGS)

if INTERNAL_JIMTCL
AM_CPPFLAGS += -I$(top_srcdir)/jimtcl -I$(top_builddir)/jimtcl
else
AM_CPPFLAGS += $(JIMTCL_CFLAGS)
endif

flash_update_SOURCES = flash_update.c ../src/flash/nor/core.c
flash_update_CPPFLAGS = $(AM_CPPFLAGS) \
	-Dmalloc=flash_test_malloc \
	-Dcalloc=flash_test_calloc \
	-Drealloc=flash_test_realloc
