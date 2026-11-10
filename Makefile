CXX := clang++
CLANG_FORMAT ?= clang-format
PYTHON ?= python3
BUILD ?= Build
CPPFLAGS += -ISources/DbC
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror
CORE := Sources/DbC/DbCController.cpp Sources/DbC/DbCCapability.cpp
HEADERS := $(wildcard Sources/DbC/*.hpp)
DARWIN := $(wildcard Sources/Darwin/*.cpp)
DARWIN_HEADERS := $(wildcard Sources/Darwin/*.hpp)
SOURCES := $(CORE) $(HEADERS) $(DARWIN) $(DARWIN_HEADERS) Tests/ControllerTests.cpp
SOURCES += Sources/Darwin/DbCModule.c Tests/Headers/kern_patcher.hpp
CONSOLE := Sources/Darwin/DbCConsole.cpp Sources/Darwin/DbCPanic.cpp
CONSOLE_HEADERS := Sources/Darwin/DbCConsole.hpp Sources/Darwin/DbCPanic.hpp
CONSOLE_HEADERS += Sources/Darwin/DbCXNU.hpp Tests/Headers/kern_patcher.hpp Tests/kdp/kdp_callout.h

KERNEL_SDK ?=
LILU_RESOURCES ?=
KEXT := $(BUILD)/DarwinDbC.kext
KEXT_OBJECTS := $(patsubst %.cpp,$(BUILD)/Kernel/%.o,$(CORE) $(DARWIN))
KEXT_OBJECTS += $(BUILD)/Kernel/DbCModule.o $(BUILD)/Kernel/LiluEntry.o
KEXT_FLAGS := -arch x86_64 -mkernel -mmacosx-version-min=13.0
KEXT_FLAGS += -nostdinc -fno-builtin -fno-common -fno-stack-protector
KEXT_FLAGS += -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
KEXT_FLAGS += -DDEBUG=1 -DPRODUCT_NAME=DarwinDbC -DMODULE_VERSION=0.1.11
KEXT_FLAGS += -DLILU_CUSTOM_IOKIT_INIT=1
KEXT_FLAGS += -isystem "$(KERNEL_SDK)/Headers" -isystem "$(LILU_RESOURCES)"
KEXT_FLAGS += -ISources/DbC -ISources/Darwin -O2 -g
KEXT_CXX_FLAGS := -std=c++17 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext -fblocks
KEXT_CXX_FLAGS += -Wall -Wextra -Werror -Wno-deprecated-declarations

.PHONY: all check check-format kext check-kext-tools receiver panic-held
all: $(BUILD)/ControllerTests

receiver: $(BUILD)/darwindbc-receive

panic-held: $(BUILD)/darwindbc-panic-held

$(BUILD)/darwindbc-panic-held: Sources/Host/PanicHeld.cpp | $(BUILD)
	xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
		-arch x86_64 -mmacosx-version-min=13.0 \
		-framework IOKit -framework CoreFoundation "$<" -o "$@"
	codesign --force --sign - "$@"
	codesign --verify --strict "$@"

$(BUILD)/darwindbc-receive: Sources/Host/Receive.cpp | $(BUILD)
	xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
		-arch arm64 -arch x86_64 -mmacosx-version-min=13.0 \
		-framework IOKit -framework CoreFoundation "$<" -o "$@"
	codesign --force --sign - "$@"
	codesign --verify --strict "$@"

$(BUILD):
	mkdir -p "$@"

$(BUILD)/ControllerTests: $(CORE) $(HEADERS) $(CONSOLE) $(CONSOLE_HEADERS) Tests/ControllerTests.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) -ISources/Darwin -ITests $(CXXFLAGS) -pthread \
		$(CORE) $(CONSOLE) Tests/ControllerTests.cpp $(LDFLAGS) -o "$@"

check: $(BUILD)/ControllerTests
	"$(BUILD)/ControllerTests"
	$(PYTHON) Tests/PersonalityTests.py

check-format:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

check-kext-tools:
	@test "$$(uname -s)" = Darwin || { echo "kext requires the macOS compiler host"; exit 1; }
	@test -f "$(KERNEL_SDK)/Headers/IOKit/IOService.h"
	@test -f "$(KERNEL_SDK)/Library/x86_64/libkmod.a"
	@test -f "$(KERNEL_SDK)/LICENSE.txt"
	@test -f "$(LILU_RESOURCES)/Headers/kern_api.hpp"
	@test -f "$(LILU_RESOURCES)/Library/plugin_start.cpp"

$(BUILD)/Kernel/%.o: %.cpp $(HEADERS) $(DARWIN_HEADERS) Makefile | check-kext-tools
	mkdir -p "$(@D)"
	xcrun clang++ $(KEXT_FLAGS) $(KEXT_CXX_FLAGS) \
		-MD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(BUILD)/Kernel/DbCModule.o: Sources/Darwin/DbCModule.c Makefile | check-kext-tools
	mkdir -p "$(@D)"
	xcrun clang $(KEXT_FLAGS) -Wall -Wextra -Werror \
		-MD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(BUILD)/Kernel/LiluEntry.o: Makefile | check-kext-tools
	mkdir -p "$(@D)"
	xcrun clang++ $(KEXT_FLAGS) $(KEXT_CXX_FLAGS) -Wno-unused-parameter \
		-MD -MP -MF "$(@:.o=.d)" \
		-c "$(LILU_RESOURCES)/Library/plugin_start.cpp" -o "$@"

$(KEXT)/Contents/MacOS/DarwinDbC: $(KEXT_OBJECTS) Makefile
	mkdir -p "$(@D)"
	xcrun clang++ -arch x86_64 -mkernel -nostdlib -static \
		-mmacosx-version-min=13.0 -Wl,-kext -Wl,-export_dynamic \
		$(KEXT_OBJECTS) "$(KERNEL_SDK)/Library/x86_64/libkmod.a" -lcc_kext -o "$@"

$(KEXT)/Contents/Info.plist: Sources/Darwin/Info.plist
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(KEXT)/Contents/Resources/LICENSE: LICENSE
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(KEXT)/Contents/Resources/ThirdPartyNotices.txt: Documentation/ThirdPartyNotices.txt
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(KEXT)/Contents/Resources/APSL.txt: | check-kext-tools
	mkdir -p "$(@D)"
	cp "$(KERNEL_SDK)/LICENSE.txt" "$@"

kext: $(KEXT)/Contents/MacOS/DarwinDbC $(KEXT)/Contents/Info.plist \
	$(KEXT)/Contents/Resources/LICENSE $(KEXT)/Contents/Resources/ThirdPartyNotices.txt \
	$(KEXT)/Contents/Resources/APSL.txt
	plutil -lint "$(KEXT)/Contents/Info.plist"
	codesign --force --sign - "$(KEXT)"
	codesign --verify --strict "$(KEXT)"

-include $(KEXT_OBJECTS:.o=.d)
