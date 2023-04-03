noinst_LTLIBRARIES += libclient.la

noinst_HEADERS += \
	./client/ring_signal.h

PRESENCE_SRC = ./client/presencemanager.cpp

VIDEO_SRC = ./client/videomanager.cpp
noinst_HEADERS += ./client/videomanager.h

libclient_la_SOURCES = \
	./client/ring_signal.cpp \
	./client/callmanager.cpp \
	./client/configurationmanager.cpp \
	$(PRESENCE_SRC) \
	$(VIDEO_SRC)

libsip_core_la_LIBADD += libclient.la
