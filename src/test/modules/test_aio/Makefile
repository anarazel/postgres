# src/test/modules/delay_execution/Makefile

PGFILEDESC = "test_aio - test code for AIO"

MODULE_big = test_aio
OBJS = \
	$(WIN32RES) \
	test_aio.o

EXTENSION = test_aio
DATA = test_aio--1.0.sql

REGRESS = prep ownership io

ifeq ($(enable_injection_points),yes)
REGRESS += inject
endif

# FIXME: with meson this runs the tests once with worker and once - if
# supported - with io_uring.

# requires custom config
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_aio
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
