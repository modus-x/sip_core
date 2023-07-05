noinst_LTLIBRARIES += libconnectivity.la

include ./connectivity/security/Makefile.am

libconnectivity_la_SOURCES = \
		./connectivity/generic_io.h \
		./connectivity/ip_utils.cpp \
		./connectivity/ip_utils.h \
		./connectivity/sip_utils.cpp \
		./connectivity/sip_utils.h \
		./connectivity/utf8_utils.cpp \
		./connectivity/utf8_utils.h

libsip_core_la_LIBADD += libconnectivity.la
