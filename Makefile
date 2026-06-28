# Builds one binary per variant: build/jet[suffix]
#
#		make										release build (default)
#		make debug							debug build:	 build/jet-debug
#		make profile						profile build: build/jet-profile
#		make all-variants				build release + debug + profile in one go
#
#		make test								run tests against release binary
#		make test-debug					run tests against debug binary
#		make test-profile				run tests against profile binary
#
#		make bench							run benches against release binary
#		make bench-debug				run benches against debug binary
#		make bench-profile			run benches against profile binary (prints opcode histogram)
#
#		make cross-bench				run cross-language benches against release binary
#		make cross-bench-debug	cross-language benches against debug
#		make cross-bench-profile cross-language benches against profile
#
#		make clean							wipe build/
#
# Object files live under build/<variant>/ so all three variants coexist.

CXX				 := clang++
ASAN_RTDIR := $(shell $(CXX) -print-runtime-dir 2>/dev/null)

VARIANT ?= release

SRC			 := src
BUILD		 := build

# --- Variant selection ---------------------------------------------------

ifeq ($(VARIANT),debug)
	OPT		 := -g3 -DJET_DEBUG -DJET_TRACE -fsanitize=undefined -fno-sanitize=vptr,function,alignment -fno-omit-frame-pointer -O1
	LDOPT	 := -fsanitize=undefined -Wl,-rpath,$(ASAN_RTDIR)
	SUFFIX := -debug
else ifeq ($(VARIANT),profile)
	OPT		 := -O2 -g3
	SUFFIX := -profile
	PROFILE_DEF := -DJET_PROFILE
	LDOPT	 :=
else ifeq ($(VARIANT),release)
	OPT		 := -O2 -g3
	SUFFIX :=
	LDOPT	 :=
else
	$(error unknown VARIANT '$(VARIANT)'; use release, debug, or profile)
endif

OBJDIR := $(BUILD)/$(VARIANT)

JET_BIN := $(BUILD)/jet$(SUFFIX)

# --- Flags ---------------------------------------------------------------

CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti -fno-strict-aliasing \
						-Wall -Werror -pipe -Wold-style-cast -Wextra -Wno-unused-parameter \
						$(OPT) $(PROFILE_DEF) -I$(SRC) -I$(BUILD)

LDFLAGS	 := $(LDOPT)

# --- Sources -------------------------------------------------------------

ALL_CC	:= $(wildcard $(SRC)/*.cc)
ALL_OBJ := $(patsubst $(SRC)/%.cc,$(OBJDIR)/%.o,$(ALL_CC))

DEPS := $(ALL_OBJ:.o=.d)

# --- Targets -------------------------------------------------------------

.PHONY: all release debug profile all-variants \
				test test-debug test-profile \
				bench bench-debug bench-profile \
				cross-bench cross-bench-debug cross-bench-profile \
				clean
.DEFAULT_GOAL := all

all: $(JET_BIN)

release:
	@$(MAKE) VARIANT=release

debug:
	@$(MAKE) VARIANT=debug

profile:
	@$(MAKE) VARIANT=profile

all-variants:
	@$(MAKE) VARIANT=release
	@$(MAKE) VARIANT=debug
	@$(MAKE) VARIANT=profile

# --- Run targets (variant-aware via JET env var) --------------------

test: release
	cd tests && JET=../build/jet ./run-tests

test-debug: debug
	cd tests && JET=../build/jet-debug ./run-tests

test-profile: profile
	cd tests && JET=../build/jet-profile ./run-tests

bench: release
	cd tests && JET=../build/jet ./run-bench

bench-debug: debug
	cd tests && JET=../build/jet-debug ./run-bench

bench-profile: profile
	cd tests && JET=../build/jet-profile ./run-bench

cross-bench: release
	cd tests/cross-bench && JET=../../build/jet ./run-cross-bench

cross-bench-debug: debug
	cd tests/cross-bench && JET=../../build/jet-debug ./run-cross-bench

cross-bench-profile: profile
	cd tests/cross-bench && JET=../../build/jet-profile ./run-cross-bench

clean:
	rm -rf $(BUILD)

$(JET_BIN): $(ALL_OBJ) | $(BUILD)
	$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/%.o: $(SRC)/%.cc | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	@mkdir -p $@

$(OBJDIR): | $(BUILD)
	@mkdir -p $@

-include $(DEPS)
