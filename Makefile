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

CXX ?= clang++

CXXFLAGS = -std=c++20 -g -Wall -Wextra
CXXFLAGS += -Isrc -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
CXXFLAGS += $(shell sdl2-config --cflags)
CXXFLAGS += -MMD -MP   # header dependency tracking

LIBS  = -framework Metal -framework MetalKit -framework Cocoa -framework IOKit -framework CoreVideo -framework QuartzCore
LIBS += $(shell sdl2-config --libs)

APP_SOURCES = src/truck.cpp src/world.cpp src/scenario.cpp src/ui.cpp src/main.mm

IMGUI_SOURCES  = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp
IMGUI_SOURCES += $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp $(IMGUI_DIR)/backends/imgui_impl_metal.mm

SOURCES = $(APP_SOURCES) $(IMGUI_SOURCES)

# src/world.cpp -> build/src/world.o, imgui/imgui.cpp -> build/imgui/imgui.o, etc.
OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJS := $(patsubst %.mm,$(BUILD_DIR)/%.o,$(OBJS))
DEPS := $(OBJS:.o=.d)

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Objective-C++ files (main.mm, imgui_impl_metal.mm) need the ObjC++ flags.
$(BUILD_DIR)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -ObjC++ -fobjc-weak -fobjc-arc -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(EXE)

-include $(DEPS)

.PHONY: all clean
