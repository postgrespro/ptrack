# contrib/ptrack/Makefile

MODULE_big = ptrack
OBJS = ptrack.o $(WIN32RES)
EXTENSION = ptrack
EXTVERSION = 2.0
DATA = ptrack.sql
DATA_built = $(EXTENSION)--$(EXTVERSION).sql
PGFILEDESC = "ptrack - public API for internal ptrack engine"

EXTRA_CLEAN = $(EXTENSION)--$(EXTVERSION).sql

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

$(EXTENSION)--$(EXTVERSION).sql: ptrack.sql
	cat $^ > $@

temp-install: EXTRA_INSTALL=contrib/ptrack
