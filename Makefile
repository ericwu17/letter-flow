# Letter Flow — SDL2 + Metal + Dear ImGui
#
# Requires SDL2:  brew install sdl2
# Build & run:    make && ./letter-flow
#
# Layout mirrors imgui/examples/example_sdl2_metal/Makefile, with our own
# sources under src/ and all object files under build/ (keeping the imgui
# submodule pristine).

EXE       = letter-flow
IMGUI_DIR = imgui
BUILD_DIR = build
# Vendored single-header libraries: nlohmann/json (save files)
THIRD_PARTY_DIR = third_party

CXX ?= clang++

CXXFLAGS = -std=c++20 -g
CXXFLAGS += -Isrc
# Vendored headers are treated as system headers so they can't trip the
# warnings-as-errors build of our own sources.
CXXFLAGS += -isystem $(IMGUI_DIR) -isystem $(IMGUI_DIR)/backends -isystem $(THIRD_PARTY_DIR)
CXXFLAGS += $(shell sdl2-config --cflags)
CXXFLAGS += -MMD -MP   # header dependency tracking

# Comprehensive but non-opinionated warning set for our own code (src/, tests/),
# with warnings treated as errors. Third-party code (imgui) is built with
# neither these flags nor -Werror.
WARNINGS  = -Wall -Wextra -Wpedantic -Werror
WARNINGS += -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
WARNINGS += -Wcast-align -Wnull-dereference -Wimplicit-fallthrough
WARNINGS += -Wformat=2 -Wdouble-promotion

LIBS  = -framework Metal -framework MetalKit -framework Cocoa -framework IOKit -framework CoreVideo -framework QuartzCore
LIBS += $(shell sdl2-config --libs)

# Simulation layer: pure logic, no SDL/imgui/Metal — the headless tests link
# against exactly these objects. Keep presentation code out of this list.
SIM_SOURCES  = src/truck.cpp src/world.cpp src/scenario.cpp src/serialization.cpp
APP_SOURCES  = $(SIM_SOURCES) src/ui.cpp src/main.mm
# One headless test binary per tests/test_*.cpp file (see the test rules below).
TEST_SOURCES := $(wildcard tests/*.cpp)
TEST_EXES    := $(patsubst tests/%.cpp,$(BUILD_DIR)/%,$(TEST_SOURCES))

IMGUI_SOURCES  = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp
IMGUI_SOURCES += $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp $(IMGUI_DIR)/backends/imgui_impl_metal.mm

VENDOR_SOURCES = $(IMGUI_SOURCES)

# src/world.cpp -> build/src/world.o, imgui/imgui.cpp -> build/imgui/imgui.o, etc.
APP_CPP_OBJS    := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(APP_SOURCES)))
APP_MM_OBJS     := $(patsubst %.mm,$(BUILD_DIR)/%.o,$(filter %.mm,$(APP_SOURCES)))
TEST_OBJS       := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
VENDOR_CPP_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(filter %.cpp,$(VENDOR_SOURCES)))
VENDOR_MM_OBJS  := $(patsubst %.mm,$(BUILD_DIR)/%.o,$(filter %.mm,$(VENDOR_SOURCES)))
# OBJS links into $(EXE); test objects stay out (they contain their own main).
OBJS := $(APP_CPP_OBJS) $(APP_MM_OBJS) $(VENDOR_CPP_OBJS) $(VENDOR_MM_OBJS)
SIM_OBJS  := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SIM_SOURCES))
DEPS := $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)
# Objects built with the full warning set (everything we wrote, tests included).
OUR_CPP_OBJS := $(APP_CPP_OBJS) $(TEST_OBJS)
OUR_MM_OBJS  := $(APP_MM_OBJS)

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

# Our own code: full warning set, warnings as errors. Static pattern rules
# must list exactly the objects they build (split by extension above): an
# empty target list would degrade the line to a bare pattern rule that
# matches every object in the build.
$(OUR_CPP_OBJS): $(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARNINGS) -c -o $@ $<

# Objective-C++ files (main.mm) need the ObjC++ flags.
$(OUR_MM_OBJS): $(BUILD_DIR)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARNINGS) -ObjC++ -fobjc-weak -fobjc-arc -c -o $@ $<

# Vendored code (imgui): no extra warnings, no -Werror.
$(VENDOR_CPP_OBJS): $(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(VENDOR_MM_OBJS): $(BUILD_DIR)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -ObjC++ -fobjc-weak -fobjc-arc -c -o $@ $<

# Headless tests (no window, no SDL): make test builds and runs one binary
# per tests/test_*.cpp, each linked against exactly the simulation objects.
# Tests run from the repo root; artifacts go under build/test-saves/.
test: $(TEST_EXES)
	@for exe in $(TEST_EXES); do ./$$exe || exit 1; done

$(BUILD_DIR)/test_%: $(BUILD_DIR)/tests/test_%.o $(SIM_OBJS)
	$(CXX) -o $@ $^

# The tests are assert-based: force asserts on for test objects so a stray
# -DNDEBUG in CXXFLAGS can never turn them into silent no-ops.
$(TEST_OBJS): CXXFLAGS += -UNDEBUG

clean:
	rm -rf $(BUILD_DIR) $(EXE)

-include $(DEPS)

.PHONY: all clean test
