# libWebKitLegacy.so.1: the legacy BWebView API (WebPositive's) on Summit's
# engine. Runs on Haiku.
#
#   make ENGINE_HEADERS=DIR ENGINE_LIB=DIR
#
# ENGINE_HEADERS holds WebKit/WebKitView.h and the rest of the engine's public
# headers; ENGINE_LIB holds libWebKit.so.1 to link against. The defaults are
# those of the installed summit_webkit package, where the library also finds
# the engine at run time.

ENGINE_HEADERS ?= /boot/system/develop/headers/summit-webkit
ENGINE_LIB ?= /boot/system/lib/summit-webkit/lib
ENGINE_RUNPATH ?= /boot/system/lib/summit-webkit/lib
VERSION ?= 1.10.0
OUT ?= build

CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-multichar -Wno-unused-parameter
CPPFLAGS += -Iheaders -I$(ENGINE_HEADERS) -I/boot/system/develop/headers/private/netservices
LDFLAGS += -shared -Wl,-soname,libWebKitLegacy.so.1 -Wl,-rpath,$(ENGINE_RUNPATH) -L$(ENGINE_LIB)
LIBS = -lWebKit -lbe -ltranslation -lnetwork

SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:src/%.cpp=$(OUT)/%.o)
LIBRARY = $(OUT)/libWebKitLegacy.so.$(VERSION)

all: $(LIBRARY)

$(OUT)/%.o: src/%.cpp src/Private.h $(wildcard headers/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(LIBRARY): $(OBJECTS)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)
	ln -sf libWebKitLegacy.so.$(VERSION) $(OUT)/libWebKitLegacy.so.1
	ln -sf libWebKitLegacy.so.1 $(OUT)/libWebKitLegacy.so

clean:
	rm -rf $(OUT)

.PHONY: all clean
