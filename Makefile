
# contrib/ptrack/Makefile

MODULE_big = ptrack
OBJS = ptrack.o datapagemap.o engine.o $(WIN32RES)
PGFILEDESC = "ptrack - block-level incremental backup engine"

EXTENSION = ptrack
EXTVERSION = 2.4
DATA = ptrack--2.1.sql ptrack--2.0--2.1.sql ptrack--2.1--2.2.sql ptrack--2.2--2.3.sql \
       ptrack--2.3--2.4.sql

TAP_TESTS = 1

# This line to link with pgport.lib on Windows compilation
# with Mkvcbuild.pm on PGv15+
PG_LIBS_INTERNAL += $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
top_builddir = ../..
# Makefile.global is a build artifact and initially may not be available
ifneq ($(wildcard $(top_builddir)/src/Makefile.global), )
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
endif

# Assuming make is started in the ptrack directory
patch:
	@cd $(top_builddir) && \
	echo Applying the ptrack patch... && \
	git apply --verbose --3way $(CURDIR)/patches/${PG_BRANCH}-ptrack-core.diff
ifeq ($(MODE), paranoia)
	@echo Applying turn-off-hint-bits.diff for the paranoia mode... && \
	git apply --verbose --3way $(CURDIR)/patches/turn-off-hint-bits.diff
endif

TEST_MODE ?= normal
test-tap:
ifeq ($(TEST_MODE), legacy)
	setarch x86_64 --addr-no-randomize $(MAKE) installcheck USE_PGXS=$(USE_PGXS) PG_CONFIG=$(PG_CONFIG)
else
	$(MAKE) installcheck USE_PGXS=$(USE_PGXS) PG_CONFIG=$(PG_CONFIG)
endif

pg_probackup_dir ?= $(CURDIR)/../pg_probackup
# Pg_probackup's Makefile uses top_srcdir when building via PGXS so set it when calling this target
# At the moment building pg_probackup with multiple threads may run some jobs too early and end with an error so do not set the -j option
install-pg-probackup:
	$(MAKE) -C $(pg_probackup_dir) install USE_PGXS=$(USE_PGXS) PG_CONFIG=$(PG_CONFIG) top_srcdir=$(top_srcdir)

PYTEST_PROCESSES ?= $(shell nproc)
test-python:
	cd $(pg_probackup_dir); \
	env="PG_PROBACKUP_PTRACK=ON PG_CONFIG=$(PG_CONFIG)"; \
	if [ "$(TEST_MODE)" = normal ]; then \
		env="$$env PG_PROBACKUP_TEST_BASIC=ON"; \
	elif [ "$(TEST_MODE)" = paranoia ]; then \
		env="$$env PG_PROBACKUP_PARANOIA=ON"; \
	fi; \
	env $$env python3 -m pytest -svv -n $(PYTEST_PROCESSES)$(if $(TESTS), -k '$(TESTS)') tests/ptrack_test.py

coverage:
	gcov *.c *.h
