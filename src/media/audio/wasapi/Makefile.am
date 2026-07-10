noinst_LTLIBRARIES += libwasapilayer.la

libwasapilayer_la_SOURCES = \
		./media/audio/wasapi/wasapilayer.cpp

noinst_HEADERS += \
		./media/audio/wasapi/wasapilayer.h \
		./media/audio/wasapi/wasapi_convert.h

libwasapilayer_la_LIBADD = -lole32 -lmmdevapi -lavrt -lksuser -luuid
libaudio_la_LIBADD += libwasapilayer.la
