# src/test/modules/deform_bench/Makefile

MODULE_big = deform_bench
OBJS = deform_bench.o

EXTENSION = deform_bench
DATA = deform_bench--1.0.sql
PGFILEDESC = "deform_bench - tuple deform benchmarking"

REGRESS = deform_bench

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/deform_bench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
