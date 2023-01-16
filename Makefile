# contrib/ptrack/Makefile

MODULE_big = ptrack
OBJS = ptrack.o datapagemap.o engine.o $(WIN32RES)
PGFILEDESC = "ptrack - block-level incremental backup engine"

EXTENSION = ptrack
EXTVERSION = 2.4
DATA = ptrack--2.1.sql ptrack--2.0--2.1.sql ptrack--2.1--2.2.sql ptrack--2.2--2.3.sql \
       ptrack--2.3--2.4.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ptrack
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
