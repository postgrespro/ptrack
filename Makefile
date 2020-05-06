# contrib/ptrack/Makefile

MODULE_big = ptrack
OBJS = ptrack.o datapagemap.o engine.o $(WIN32RES)
EXTENSION = ptrack
EXTVERSION = 2.1
DATA = ptrack.sql ptrack--2.0--2.1.sql
DATA_built = $(EXTENSION)--$(EXTVERSION).sql
PGFILEDESC = "ptrack - block-level incremental backup engine"

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

# check: isolationcheck

# ISOLATIONCHECKS=corner_cases

# submake-isolation:
# 	$(MAKE) -C $(top_builddir)/src/test/isolation all

# isolationcheck: | submake-isolation temp-install
# 	$(MKDIR_P) isolation_output
# 	$(pg_isolation_regress_check) \
# 	  --temp-config $(top_srcdir)/contrib/pg_query_state/test.conf \
#       --outputdir=isolation_output \
# 	  $(ISOLATIONCHECKS)

# isolationcheck-install-force: all | submake-isolation temp-install
# 	$(MKDIR_P) isolation_output
# 	$(pg_isolation_regress_installcheck) \
#       --outputdir=isolation_output \
# 	  $(ISOLATIONCHECKS)

# .PHONY: isolationcheck isolationcheck-install-force check

temp-install: EXTRA_INSTALL=contrib/ptrack
