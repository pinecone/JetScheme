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
#		make clean							wipe build/
#
# Object files live under build/<variant>/ so all three variants coexist.

CXX				 := clang++
UNCRUSTIFY := uncrustify
ASAN_RTDIR := $(shell $(CXX) -print-runtime-dir 2>/dev/null)

# Parallel build by default; override with `make JOBS=1` or `make -j1`.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
MAKEFLAGS += -j$(JOBS)

VARIANT ?= release

SRC			 := src
BUILD		 := build
PRELUDE := lib/prelude.ss
PRELUDE_H := $(BUILD)/prelude.h

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
						$(OPT) $(PROFILE_DEF) -I$(SRC) -I$(BUILD) -Ivendor

LDFLAGS	 := $(LDOPT)

# --- Sources -------------------------------------------------------------

ALL_CC	:= $(wildcard $(SRC)/*.cc)
ALL_CPP := $(ALL_CC) $(wildcard $(SRC)/*.h)
ALL_OBJ := $(patsubst $(SRC)/%.cc,$(OBJDIR)/%.o,$(ALL_CC))

DEPS := $(ALL_OBJ:.o=.d)

# --- Targets -------------------------------------------------------------

.PHONY: all release debug profile all-variants \
				test test-debug test-profile \
				ab-cross-bench format format-check clean
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

# Builds its own worktree of REF (default HEAD), so no build dependency.
ab-cross-bench:
	cd bench && ./ab-cross $(REF)

format:
	$(UNCRUSTIFY) -q -c uncrustify.cfg --replace --no-backup $(ALL_CPP)

format-check:
	$(UNCRUSTIFY) -q -c uncrustify.cfg --check $(ALL_CPP)

clean:
	rm -rf $(BUILD)

$(JET_BIN): $(ALL_OBJ) | $(BUILD)
	$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/main.o: $(PRELUDE_H)

$(OBJDIR)/%.o: $(SRC)/%.cc | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(PRELUDE_H): $(PRELUDE) | $(BUILD)
	@{ \
		printf '%s\n' '#pragma once' '#include <cstddef>' \
			'inline constexpr unsigned char prelude_source[] = {'; \
		LC_ALL=C od -An -v -tx1 $< | \
			awk '{ for (i = 1; i <= NF; ++i) printf "0x%s,", $$i; print "" }'; \
		printf '%s\n' '};' \
			'inline constexpr std::size_t prelude_source_size = sizeof(prelude_source);'; \
	} > $@.tmp
	@mv $@.tmp $@

$(BUILD):
	@mkdir -p $@

$(OBJDIR): | $(BUILD)
	@mkdir -p $@

-include $(DEPS)
