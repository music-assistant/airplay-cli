# cliairplay - Unified AirPlay streaming binary
# Supports both RAOP (AirPlay 1) and AirPlay 2 protocols

ifeq ($(CC),cc)
CC=$(lastword $(subst /, ,$(shell readlink -f `which cc` 2>/dev/null || which cc)))
endif

ifeq ($(findstring gcc,$(CC)),gcc)
CFLAGS  += -Wno-stringop-truncation -Wno-stringop-overflow -Wno-format-truncation -Wno-multichar
LDFLAGS += -s -lstdc++ -latomic
else
CFLAGS += -fno-temp-file
LDFLAGS += -lc++
endif

PLATFORM ?= $(firstword $(subst -, ,$(CC)))
HOST ?= $(word 2, $(subst -, ,$(CC)))

# Default host detection for native builds
ifeq ($(HOST),)
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
HOST = macos
ifeq ($(UNAME_M),arm64)
PLATFORM = arm64
else
PLATFORM = x86_64
endif
else
HOST = linux
PLATFORM = $(UNAME_M)
endif
endif

# Directories
SRC         = src
LIBRAOP     = libraop
BUILDDIR    = build/$(HOST)/$(PLATFORM)
BINDIR      = bin

# Output binary
EXECUTABLE  = $(BINDIR)/cliairplay-$(HOST)-$(PLATFORM)

# Compiler flags
DEFINES  = -DNDEBUG -D_GNU_SOURCE -DOPENSSL_SUPPRESS_DEPRECATED
CFLAGS  += -Wall -fPIC -ggdb -O2 $(DEFINES) -fdata-sections -ffunction-sections
# The C++ sources (ours and libraop's) use C++11 features (lambdas, auto, range-for);
# pin the standard since some compilers still default to an older one. Use the GNU
# dialect (g++'s default) so GNU extensions in libraop's sources keep working.
CXXFLAGS += -std=gnu++17
LDFLAGS += -lpthread -ldl -lm -L.

# POSIX shared memory (shm_open/shm_unlink) for the --ptp-daemon shared clock:
# glibc exposes these via librt; macOS has them in libSystem, so link -lrt on
# Linux only.
ifeq ($(HOST),linux)
LDFLAGS += -lrt
endif

# Extra flags for cross-compilation, e.g. building macOS x86_64 on an arm64 host:
#   make HOST=macos PLATFORM=x86_64 EXTRA_CFLAGS="-arch x86_64" EXTRA_LDFLAGS="-arch x86_64"
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# Paths into libraop submodule
TOOLS       = $(LIBRAOP)/crosstools/src
DMAP_PARSER = $(LIBRAOP)/dmap-parser
MDNS        = $(LIBRAOP)/libmdns/targets
CODECS      = $(LIBRAOP)/libcodecs/targets
OPENSSL     = $(LIBRAOP)/libopenssl/targets/$(HOST)/$(PLATFORM)

vpath %.c $(TOOLS):$(LIBRAOP)/src:$(DMAP_PARSER):$(SRC)
vpath %.cpp $(TOOLS):$(LIBRAOP)/src:$(SRC)

INCLUDE = -I$(TOOLS) \
	-I$(DMAP_PARSER) \
	-I$(MDNS)/include/mdnssvc -I$(MDNS)/include/mdnssd \
	-I$(OPENSSL)/include \
	-I$(CODECS)/include/addons -I$(CODECS)/include/flac \
	-I$(CODECS)/include/shine -I$(CODECS)/include/faac \
	-I$(CODECS)/include/alac \
	-I$(LIBRAOP)/src -I$(LIBRAOP)/src/inc \
	-I$(LIBRAOP)/crosstools/src \
	-I$(SRC)

# libraop core sources (RAOP protocol)
RAOP_SOURCES = raop_client.c rtsp_client.c \
	aes.c aes_ctr.c \
	dmap_parser.c \
	alac.c

# AirPlay 2 sources (new)
AP2_SOURCES = ap2_client.c ap2_hap.c ap2_ptp.c ap2_ptp_shm.c ap2_plist.c ap2_mrp.c

# Common/CLI sources
CLI_SOURCES = cross_log.c cross_ssl.c cross_util.c cross_net.c platform.c \
	cliairplay.c pairing.cpp bplist.cpp ap2_bplist.cpp alac_ext.cpp

# Pre-built static libraries
# We use a patched copy of libcodecs.a with the buggy alac_create_encoder removed,
# replaced by our alac_ext.cpp which properly handles 24-bit audio.
# Keep the copy next to the original: on some platforms libcodecs.a is a "thin"
# archive that references its member libraries by path relative to its own
# directory, so copying it elsewhere would break those references at link time.
LIBCODECS_PATCHED = $(CODECS)/$(HOST)/$(PLATFORM)/libcodecs_patched.a
LIBRARY = $(LIBCODECS_PATCHED) $(MDNS)/$(HOST)/$(PLATFORM)/libmdns.a

ifneq ($(STATIC),)
LIBRARY += $(OPENSSL)/libopenssl.a
DEFINES += -DSSL_STATIC_LIB
endif

# Object files
OBJECTS_RAOP = $(patsubst %.c,$(BUILDDIR)/%.o,$(RAOP_SOURCES))
OBJECTS_AP2  = $(patsubst %.c,$(BUILDDIR)/%.o,$(AP2_SOURCES))
OBJECTS_CLI  = $(patsubst %.c,$(BUILDDIR)/%.o,$(filter %.c,$(CLI_SOURCES)))
OBJECTS_CLI += $(patsubst %.cpp,$(BUILDDIR)/%.o,$(filter %.cpp,$(CLI_SOURCES)))

OBJECTS_ALL = $(OBJECTS_RAOP) $(OBJECTS_AP2) $(OBJECTS_CLI)

all: directory $(EXECUTABLE)

directory:
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(BINDIR)

# Create patched libcodecs without the buggy alac_create_encoder
$(LIBCODECS_PATCHED): $(CODECS)/$(HOST)/$(PLATFORM)/libcodecs.a
	cp $< $@
	ar d $@ alac_wrapper.o 2>/dev/null || true

$(EXECUTABLE): $(OBJECTS_ALL) $(LIBCODECS_PATCHED)
	$(CXX) $(OBJECTS_ALL) $(LIBRARY) $(LDFLAGS) -o $@

$(BUILDDIR)/%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

$(BUILDDIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

clean:
	rm -rf $(BUILDDIR) $(EXECUTABLE) $(LIBCODECS_PATCHED)

.PHONY: all directory clean
