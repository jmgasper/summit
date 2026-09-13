CXX ?= g++
SUMMIT_MODERN_WEBKIT ?= 0
ifeq ($(SUMMIT_MODERN_WEBKIT),1)
BUILD ?= build-modern-haiku
else ifeq ($(SUMMIT_MODERN_WEBKIT),0)
BUILD ?= build-haiku
else
$(error SUMMIT_MODERN_WEBKIT must be 0 or 1)
endif
WEBKIT_PREFIX ?= /boot/system
WEBKIT_LIBRARY_DIR ?= $(WEBKIT_PREFIX)/develop/lib
CPPFLAGS += -Isrc -Ivendor -I/boot/system/develop/headers/private/netservices -DSUMMIT_MODERN_WEBKIT=$(SUMMIT_MODERN_WEBKIT)
CXXFLAGS ?= -O2 -g
ifeq ($(SUMMIT_MODERN_WEBKIT),1)
WEBKIT_INCLUDE_DIR ?= $(WEBKIT_PREFIX)/develop/headers
CPPFLAGS += -I$(WEBKIT_INCLUDE_DIR) -DBUILDING_HAIKU__=1
CXXFLAGS += -std=c++23 -Wall -Wextra -Wno-multichar
WEBKIT_LINK_LIBRARY = WebKit
else
CPPFLAGS += -I$(WEBKIT_PREFIX)/develop/headers/webkit -I$(WEBKIT_PREFIX)/develop/headers/webkit/haiku
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-multichar
WEBKIT_LINK_LIBRARY = WebKitLegacy
endif
CORE = $(wildcard src/core/*.cpp) $(wildcard src/extensions/*.cpp)
UI = $(wildcard src/ui/*.cpp) src/main.cpp
CORE_OBJ = $(CORE:%.cpp=$(BUILD)/%.o)
UI_OBJ = $(UI:%.cpp=$(BUILD)/%.o)
LIBS = -L$(WEBKIT_LIBRARY_DIR) -l$(WEBKIT_LINK_LIBRARY) -lbe -lnetwork -lbnetapi -ltranslation -ltracker
.PHONY: all check clean package browser-smoke
all: $(BUILD)/Summit $(BUILD)/resources/start.html
$(BUILD)/Summit: $(CORE_OBJ) $(UI_OBJ) resources/Summit.rdef
	$(CXX) $(LDFLAGS) -o $@.new $(CORE_OBJ) $(UI_OBJ) $(LIBS)
	rc -o $(BUILD)/Summit.rsrc resources/Summit.rdef
	xres -o $@.new $(BUILD)/Summit.rsrc
	mv $@.new $@
$(BUILD)/resources/start.html: resources/start.html
	mkdir -p $(dir $@)
	cp $< $@
$(BUILD)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/summit_tests: $(CORE_OBJ) $(BUILD)/tests/CoreTests.o
	$(CXX) -o $@ $^
check: $(BUILD)/summit_tests
	$(BUILD)/summit_tests
$(BUILD)/summit_browser_smoke: $(BUILD)/tests/BrowserSmoke.o
	$(CXX) -o $@ $^ -lbe
$(BUILD)/summit_browser_probe: $(BUILD)/tests/BrowserProbe.o
	$(CXX) -o $@ $^ -lbe
browser-smoke: $(BUILD)/summit_browser_smoke
	$(BUILD)/summit_browser_smoke
package: all
	bash tools/package-haiku.sh
clean:
	rm -rf $(BUILD)
-include $(CORE_OBJ:.o=.d) $(UI_OBJ:.o=.d) $(BUILD)/tests/CoreTests.d
