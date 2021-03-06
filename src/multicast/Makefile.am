# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

pkgcfgdir= $(pkgdatadir)/config.d/

libexecdir= $(pkglibdir)/libexec/

pkgcfg_DATA = \
  multicast.conf

if MINGW
 WINFLAGS = -Wl,--no-undefined -Wl,--export-all-symbols
endif

if USE_COVERAGE
  AM_CFLAGS = -fprofile-arcs -ftest-coverage
endif

lib_LTLIBRARIES = libgnunetmulticast.la

libgnunetmulticast_la_SOURCES = \
  multicast_api.c
libgnunetmulticast_la_LIBADD = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(GN_LIBINTL) $(XLIB)
libgnunetmulticast_la_LDFLAGS = \
  $(GN_LIB_LDFLAGS)  $(WINFLAGS) \
  -version-info 0:0:0


bin_PROGRAMS = \
 gnunet-multicast

libexec_PROGRAMS = \
 gnunet-service-multicast \
 $(EXP_LIBEXEC)

gnunet_multicast_SOURCES = \
 gnunet-multicast.c
gnunet_multicast_LDADD = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(GN_LIBINTL)

gnunet_service_multicast_SOURCES = \
 gnunet-service-multicast.c
gnunet_service_multicast_LDADD = \
  $(top_builddir)/src/util/libgnunetutil.la \
  $(top_builddir)/src/cadet/libgnunetcadet.la \
  $(top_builddir)/src/statistics/libgnunetstatistics.la \
  $(GN_LIBINTL)

check_PROGRAMS = \
  test_multicast \
  test_multicast_multipeer_star \
  test_multicast_multipeer_line
#  test_multicast_2peers

if ENABLE_TEST_RUN
AM_TESTS_ENVIRONMENT=export GNUNET_PREFIX=$${GNUNET_PREFIX:-@libdir@}; export PATH=$${GNUNET_PREFIX:-@prefix@}/bin:$$PATH; unset XDG_DATA_HOME; unset XDG_CONFIG_HOME;
TESTS = $(check_PROGRAMS)
endif

test_multicast_SOURCES = \
 test_multicast.c
test_multicast_LDADD = \
  libgnunetmulticast.la \
  $(top_builddir)/src/testing/libgnunettesting.la \
  $(top_builddir)/src/util/libgnunetutil.la
test_multicast_multipeer_star_SOURCES = \
  test_multicast_multipeer.c
test_multicast_multipeer_star_LDADD = \
  libgnunetmulticast.la \
  $(top_builddir)/src/testbed/libgnunettestbed.la \
  $(top_builddir)/src/util/libgnunetutil.la
test_multicast_multipeer_line_SOURCES = \
  test_multicast_multipeer.c
test_multicast_multipeer_line_LDADD = \
  libgnunetmulticast.la \
  $(top_builddir)/src/testbed/libgnunettestbed.la \
  $(top_builddir)/src/util/libgnunetutil.la

test_multicast_2peers_SOURCES = \
  test_multicast_2peers.c
test_multicast_2peers_LDADD = \
  libgnunetmulticast.la \
  $(top_builddir)/src/testbed/libgnunettestbed.la \
  $(top_builddir)/src/util/libgnunetutil.la
