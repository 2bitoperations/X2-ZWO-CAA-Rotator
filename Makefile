# Makefile for libx2caarotator

CC    = g++
RM    = rm -f
STRIP = strip

GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

UNAME_S := $(shell uname -s)

ifneq (,$(findstring MINGW,$(UNAME_S)))
  # Windows — MSYS2 / MinGW64
  TARGET_LIB  = libx2caarotator.dll
  OS_FLAG     = -DSB_WIN_BUILD
  # hidapi via MSYS2: pacman -S mingw-w64-x86_64-hidapi
  # MSYS2 puts headers at /mingw64/include/hidapi/hidapi.h; pkg-config only
  # returns -I/mingw64/include (parent), so we point directly at the subdir.
  HIDAPI_CFLAGS  = -I/mingw64/include/hidapi
  HIDAPI_LIBS    = -lhidapi
  LDFLAGS     = -shared -static-libstdc++ -static-libgcc
  STRIP_FLAGS =

else ifeq ($(UNAME_S),Darwin)
  TARGET_LIB  = libx2caarotator.dylib
  OS_FLAG     = -DSB_MACOSX_BUILD
  # hidapi via Homebrew: brew install hidapi
  HIDAPI_CFLAGS  = $(shell pkg-config --cflags hidapi 2>/dev/null || echo -I/opt/homebrew/include/hidapi -I/usr/local/include/hidapi)
  HIDAPI_LIBS    = $(shell pkg-config --libs   hidapi 2>/dev/null || echo -lhidapi -framework IOKit -framework CoreFoundation)
  LDFLAGS     = -dynamiclib -lstdc++
  STRIP_FLAGS = -x

else
  TARGET_LIB  = libx2caarotator.so
  OS_FLAG     = -DSB_LINUX_BUILD
  # hidapi-hidraw (kernel driver backend, preferred for HID feature reports on Linux)
  # Install: sudo apt install libhidapi-dev
  HIDAPI_CFLAGS  = $(shell pkg-config --cflags hidapi-hidraw 2>/dev/null || pkg-config --cflags hidapi 2>/dev/null)
  HIDAPI_LIBS    = $(shell pkg-config --libs   hidapi-hidraw 2>/dev/null || pkg-config --libs   hidapi 2>/dev/null || echo -lhidapi-hidraw)
  LDFLAGS     = -shared -lstdc++ -lpthread
  STRIP_FLAGS = --strip-unneeded

endif

CPPFLAGS = -fPIC -Wall -Wextra -O2 $(OS_FLAG) -std=gnu++11 \
           -I. -Ilicensedinterfaces \
           -DX2_FLAT_INCLUDES \
           -DGIT_HASH=\"$(GIT_HASH)\" \
           $(HIDAPI_CFLAGS)

# BUILD_NUMBER is passed from CI: make BUILD_NUMBER=42
ifdef BUILD_NUMBER
  CPPFLAGS += -DBUILD_NUMBER=$(BUILD_NUMBER)
endif

SRCS = main.cpp x2caarotator.cpp caarotator.cpp
OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean validate_ui install udev-install

all: validate_ui $(TARGET_LIB)

validate_ui:
	@if command -v uic >/dev/null 2>&1; then \
		echo "Validating x2caarotator.ui..."; \
		uic x2caarotator.ui > /dev/null || (echo "UI validation failed!" && exit 1); \
		echo "UI OK."; \
	else \
		echo "uic not found, skipping UI validation."; \
	fi

$(TARGET_LIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(HIDAPI_LIBS)
	$(STRIP) $(STRIP_FLAGS) $@ >/dev/null 2>&1 || true

%.o: %.cpp
	$(CC) $(CPPFLAGS) -c $< -o $@

install: $(TARGET_LIB)
	./install.sh

udev-install:
	@echo "Installing udev rule for ZWO CAA..."
	sudo install -m 644 99-zwocaa.rules /etc/udev/rules.d/99-zwocaa.rules
	sudo udevadm control --reload-rules && sudo udevadm trigger
	@echo ""
	@echo "Rule installed.  Please replug the CAA USB cable."

clean:
	$(RM) libx2caarotator.so libx2caarotator.dylib libx2caarotator.dll $(OBJS)
