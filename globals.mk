# Global variables

src=$(abs_top_srcdir)

SIP_CORE_DIRTY_REPO ?= $(shell git diff-index --quiet HEAD 2>/dev/null || echo dirty)
SIP_CORE_REVISION ?= $(shell git log -1 --format="%h" --abbrev=10 2>/dev/null)
SIP_CORE_DATADIR ?= $(datadir)/sip_core

# Preprocessor flags
AM_CPPFLAGS += \
	-I$(src)/src \
	-I$(src)/src/config \
	-I$(src)/src/media \
	-I$(src)/src/sip_core \
	$(SIP_CFLAGS) \
	-DPREFIX=\"$(prefix)\" \
	-DSIP_CORE_DATADIR=\"$(SIP_CORE_DATADIR)\" \
	-DENABLE_TRACE \
	-DSIP_CORE_REVISION=\"$(SIP_CORE_REVISION)\" \
	-DSIP_CORE_DIRTY_REPO=\"$(SIP_CORE_DIRTY_REPO)\" \
	-DPJSIP_MAX_PKT_LEN=8000 \
	-DPJ_AUTOCONF=1
