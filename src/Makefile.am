include $(top_srcdir)/globals.mk

if HAVE_OSX
AM_OBJCXXFLAGS=-std=c++17
endif

noinst_LTLIBRARIES=
noinst_HEADERS=
nobase_include_HEADERS=

lib_LTLIBRARIES = libsip_core.la
libsip_core_la_LIBADD=

include ./client/Makefile.am
include ./config/Makefile.am
include ./connectivity/Makefile.am
include ./im/Makefile.am
include ./media/Makefile.am
include ./sip/Makefile.am

# libsip_core

libsip_core_la_LIBADD += \
	$(ALSA_LIBS) \
	$(PULSEAUDIO_LIBS) \
	$(PORTAUDIO_LIBS) \
	$(SPEEXDSP_LIBS) \
	$(PJPROJECT_LIBS) \
	$(LIBAVCODEC_LIBS) \
	$(LIBAVFORMAT_LIBS) \
	$(LIBAVDEVICE_LIBS) \
	$(LIBAVFILTER_LIBS) \
	$(LIBSWRESAMPLE_LIBS) \
	$(LIBSWSCALE_LIBS) \
	$(LIBAVUTIL_LIBS) \
	$(WEBRTC_LIBS) \
	$(YAMLCPP_LIBS) \
	$(JSONCPP_LIBS) \
	$(FMT_LIBS) \
	$(LIBSSL_LIBS) \
	$(LIBCRYPTO_LIBS)

# XXX: It seems this should be the default value when otherwise left
# unspecified, but for some reason this is required otherwise the
# shared object fails to link against ffmpeg's libswscale.a.
libsip_core_la_LDFLAGS = $(AM_LDFLAGS)

if HAVE_WIN32
libsip_core_la_LDFLAGS += -no-undefined -avoid-version
endif

if HAVE_OSX
#FIXME necessary for -lintl
libsip_core_la_LDFLAGS += -L/usr/local/opt/gettext/lib
endif

libsip_core_la_SOURCES = \
		buildinfo.cpp \
		conference.cpp \
		conference_protocol.cpp \
		account_factory.cpp \
		call_factory.cpp \
		preferences.cpp \
		manager.cpp \
		call.cpp \
		account.cpp \
		account_config.cpp \
		logger.cpp \
		fileutils.cpp \
		threadloop.cpp \
		vcard.cpp \
		threadloop.h \
		conference.h \
		conference_protocol.h \
		account_factory.h \
		call_factory.h \
		preferences.h \
		manager.h \
		account.h \
		call.h \
		logger.h \
		fileutils.h \
		noncopyable.h \
		ring_types.h \
		compiler_intrinsics.h \
		account_schema.h \
		registration_states.h \
		map_utils.h \
		ring_api.cpp \
		rational.h \
		base64.h \
		base64.cpp \
		scheduled_executor.h \
		scheduled_executor.cpp \
		string_utils.cpp \
		string_utils.h \
		uri.h \
		uri.cpp \
		vcard.h

if HAVE_WIN32
libsip_core_la_SOURCES += \
		dlfcn.c \
		winsyslog.c
endif

nobase_include_HEADERS += \
	./sip_core/account_const.h \
	./sip_core/call_const.h \
	./sip_core/callmanager_interface.h \
	./sip_core/configurationmanager_interface.h \
	./sip_core/def.h \
	./sip_core/sip_core.h \
	./sip_core/media_const.h \
	./sip_core/presence_const.h \
	./sip_core/presencemanager_interface.h \
	./sip_core/trace-tools.h

if ENABLE_VIDEO
nobase_include_HEADERS += \
		./sip_core/videomanager_interface.h
endif

if ENABLE_TRACEPOINTS
libsip_core_la_SOURCES += sip_core/tracepoint.h sip_core/tracepoint-def.h sip_core/tracepoint.c
libsip_core_la_LIBADD  += $(LTTNG_LIBS)
endif
