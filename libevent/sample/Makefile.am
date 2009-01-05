AUTOMAKE_OPTIONS = foreign no-dependencies

LDADD = ../libevent.la
AM_CFLAGS = -I$(top_srcdir) -I$(top_srcdir)/compat -I$(top_srcdir)/include

noinst_PROGRAMS = event-test time-test signal-test aio-test

event_test_sources = event-test.c
time_test_sources = time-test.c
signal_test_sources = signal-test.c
aio_test_sources = aio-test.c

aio_test_LDADD = $(ZLIB_LIBS)
aio_test_CFLAGS = ../libevent.la -I$(top_srcdir) -I$(top_srcdir)/compat -I$(top_srcdir)/include $(ZLIB_CFLAGS)

verify:

DISTCLEANFILES = *~
