# Adapter Makefile for the Data Frog multicore build system.
#
# sf2000_multicore does `make -C <CORE> platform=sf2000` and then copies the
# *.a it finds here, so this file only has to delegate to the real build in
# ../../Makefile and put the resulting archive where the multicore looks.
#
# The multicore's own flags (SDK include path, RE3_CONSOLE_SDK, the FROGGY
# defines) come from the top-level Makefile in re3-sf2000, which is the single
# place that knows how the port is compiled - keeping it that way means the core
# built here and the one built by `make sf2000` are the same binary.

TARGET_NAME := re3
RE3_PORT    ?= /home/Sajnaps/re3-sf2000
TARGET      := $(TARGET_NAME)_libretro_$(platform).a
ARCHIVE     := $(RE3_PORT)/build/$(platform)/libretro_re3.a

ifeq ($(platform), sf2000)
	STATIC_LINKING = 1
endif

all:
	$(MAKE) -C "$(RE3_PORT)" $(platform)
	cp -f "$(ARCHIVE)" ./$(TARGET)

clean:
	rm -f ./*.a

.PHONY: all clean