#
# Makefile for storage/aio
#
# src/backend/storage/aio/Makefile
#

subdir = src/backend/storage/aio
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	aio.o \
	aio_exchange.o \
	aio_funcs.o \
	aio_io.o \
	aio_scb.o \
	aio_util.o \
	aio_worker.o

ifeq ($(with_liburing),yes)
override CPPFLAGS += $(LIBURING_CFLAGS)
OBJS += aio_uring.o
endif

ifeq ($(with_posix_aio),yes)
OBJS += aio_posix.o
endif

include $(top_srcdir)/src/backend/common.mk
