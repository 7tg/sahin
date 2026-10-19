# Prospero BlackBox - The file worker, built as a payload for the console's loader.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

OUTPUT ?= ../build/store-worker/store-worker.elf
WORK := $(dir $(OUTPUT))
SOURCE := ../src
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I$(SOURCE) \
	-DSTORE_WORKER_CONSOLE
CFLAGS := -std=c11 -O2 -w -I$(SOURCE)
LDFLAGS := -Wl,--build-id=none

CXX_SOURCES := main.cpp $(SOURCE)/install/archive.cpp $(SOURCE)/install/files.cpp \
	$(SOURCE)/install/worker.cpp
C_SOURCES := $(addprefix $(SOURCE)/third_party/miniz/,miniz.c miniz_tinfl.c miniz_tdef.c miniz_zip.c)
OBJECTS := $(addprefix $(WORK),$(notdir $(CXX_SOURCES:.cpp=.o) $(C_SOURCES:.c=.o)))
vpath %.cpp . $(SOURCE)/install
vpath %.c $(SOURCE)/third_party/miniz

.PHONY: all clean

all: $(OUTPUT)

$(WORK)%.o: %.cpp $(wildcard $(SOURCE)/install/*.hpp)
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(WORK)%.o: %.c
	@mkdir -p -- $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -lSceUserService

clean:
	rm -rf -- $(WORK)
