noinst_LTLIBRARIES += libsecurity.la

libsecurity_la_CXXFLAGS = -I$(top_srcdir)/src $(AM_CXXFLAGS)

libsecurity_la_SOURCES = \
		./connectivity/security/memory.cpp \
		./connectivity/security/memory.h

libsip_core_la_LIBADD += libsecurity.la
