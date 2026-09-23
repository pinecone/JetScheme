# Builds one binary per variant: build/jet[suffix]
#
#		make										release build (default)
#		make debug							debug build:	 build/jet-debug
#		make profile						profile build: build/jet-profile
#		make all-variants				build release + debug + profile in one go
#
#		make test								run the release, profile, and sanitize targets
#		make test-release				run tests against release binary
#		make test-profile				run tests against profile binary
#		make sanitize					run debug-binary tests and benchmarks under UBSan
#
#		make clean							wipe build/
#
# Object files live under build/<variant>/ so all three variants coexist.

CXX				 := clang++
ASAN_RTDIR = $(shell $(CXX) -print-runtime-dir 2>/dev/null)

# Parallel build by default; override with `make JOBS=1` or `make -j1`.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
MAKEFLAGS += -j$(JOBS)

V ?= 0

ifeq ($(V),1)
	Q :=
else
	Q := @
endif

VARIANT ?= release

SRC			 := src
BUILD		 := build
PRELUDE := lib/prelude.ss
PRELUDE_H := $(BUILD)/prelude.h

# --- Variant selection ---------------------------------------------------

ifeq ($(VARIANT),debug)
	OPT		 := -g3 -DJET_DEBUG -DJET_TRACE -fsanitize=address,undefined -fno-sanitize=vptr,function \
						-fno-omit-frame-pointer -O1
	LDOPT	 := -fsanitize=address,undefined -Wl,-rpath,$(ASAN_RTDIR)
	SUFFIX := -debug
else ifeq ($(VARIANT),profile)
	OPT		 := -O2 -g3
	SUFFIX := -profile
	PROFILE_DEF := -DJET_PROFILE -pthread
	LDOPT	 := -pthread
else ifeq ($(VARIANT),release)
	OPT		 := -O2 -g3
	SUFFIX :=
	LDOPT	 :=
else
	$(error unknown VARIANT '$(VARIANT)'; use release, debug, or profile)
endif

OBJDIR := $(BUILD)/$(VARIANT)

JET_BIN := $(BUILD)/jet$(SUFFIX)

# Opcode-table generator and the header it emits (included by src/opcodes.h).
GENOPCODES := $(OBJDIR)/generate-opcodes
OPCODES_GEN_H := $(BUILD)/opcodes_gen.h

# --- Flags ---------------------------------------------------------------

CXXFLAGS := -stdlib=libc++ -std=c++20 -fno-exceptions -fno-rtti \
						-Wall -Werror -pipe -Wold-style-cast -Wextra -Wno-unused-parameter \
						$(OPT) $(PROFILE_DEF) -I$(SRC) -I$(BUILD) -Ivendor

# --- Modules -------------------------------------------------------------

MODULES ?= $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard modules/*/module.mk)))))
MODULE_DIRS := $(addprefix modules/,$(MODULES))
MODULE_MKS := $(addsuffix /module.mk,$(MODULE_DIRS))
MODULE_CC :=
MODULE_CPP :=
MODULE_SOKOL_CC :=
MODULE_VENDOR_CC :=
MODULE_INIT :=
MODULE_PRELUDE :=
MODULE_TESTS :=

include $(MODULE_MKS)

UNAME_S := $(shell uname -s)

ifneq ($(strip $(MODULE_SOKOL_CC)),)
ifeq ($(UNAME_S),Darwin)
	SOKOL_LANG := -x objective-c++
	MODULE_LDFLAGS := -framework AppKit -framework QuartzCore -framework OpenGL -framework AudioToolbox
else
	SOKOL_LANG :=
	MODULE_LDFLAGS := -pthread -lX11 -lXi -lXcursor -lGL -lasound -ldl -lm
endif
endif

LDOPT += -stdlib=libc++
ifeq ($(UNAME_S),Darwin)
LDOPT += --ld-path=/usr/bin/ld
else
LDOPT += -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind
endif
LDFLAGS	 := $(LDOPT) $(MODULE_LDFLAGS)

# Third-party code, so warnings-as-errors and the house warning set do not apply.
VENDOR_CXXFLAGS := $(filter-out -Wall -Wextra -Werror -Wold-style-cast,$(CXXFLAGS)) -w
SOKOL_CXXFLAGS := $(VENDOR_CXXFLAGS) $(SOKOL_LANG)

# --- Sources -------------------------------------------------------------

CORE_CC := $(filter-out $(SRC)/generate-opcodes.cc,$(wildcard $(SRC)/*.cc))
ALL_CC := $(CORE_CC) $(MODULE_CC)
SOKOL_OBJ := $(patsubst %.cc,$(OBJDIR)/%.o,$(MODULE_SOKOL_CC))
VENDOR_OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(MODULE_VENDOR_CC))
ALL_OBJ := $(patsubst %.cc,$(OBJDIR)/%.o,$(ALL_CC)) $(SOKOL_OBJ) $(VENDOR_OBJ)
PRELUDE_SOURCES := $(PRELUDE) $(MODULE_PRELUDE)
MODULES_H := $(BUILD)/modules.h
BENCHMARK_SS := $(sort $(wildcard bench/bench-*.ss))
SANITIZE_OPTIONS ?= halt_on_error=1:print_stacktrace=1
SANITIZE_MARKER := Sanitizer
SANITIZE_ENV := ASAN_OPTIONS='$(SANITIZE_OPTIONS)' UBSAN_OPTIONS='$(SANITIZE_OPTIONS)' \
								JET_MODULE_TESTS='$(MODULE_TESTS)' JET_TEST_DIAGNOSTICS=1

DEPS := $(ALL_OBJ:.o=.d) $(OBJDIR)/tests/profile.d

# --- Targets -------------------------------------------------------------

.PHONY: all release debug profile all-variants \
				test test-release test-profile sanitize show-sanitizers \
				ab-cross-bench clean tags FORCE \
				check-deps check-tags-deps check-python-deps
.DEFAULT_GOAL := all

all: $(JET_BIN)

release:
	$(Q)$(MAKE) VARIANT=release

debug:
	$(Q)$(MAKE) VARIANT=debug

profile:
	$(Q)$(MAKE) VARIANT=profile

all-variants:
	$(Q)$(MAKE) VARIANT=release
	$(Q)$(MAKE) VARIANT=debug
	$(Q)$(MAKE) VARIANT=profile

# --- Dependency checks ---------------------------------------------------

# Order-only prerequisites keep checks ahead of parallel compilation without forcing rebuilds.
$(ALL_OBJ) $(OBJDIR)/tests/profile.o $(JET_BIN) $(OBJDIR)/profile-test \
$(PRELUDE_H) $(MODULES_H) $(GENOPCODES) $(OPCODES_GEN_H): | check-deps

define dependency_diagnostics
	failed=0; apt_packages=; brew_packages=; apple_tools=0; \
	missing() { \
		printf '  Missing or unusable: %s\n' "$$1" >&2; \
		failed=1; \
		for package in $$2; do \
			case " $$apt_packages " in \
				*" $$package "*) ;; \
				*) apt_packages="$${apt_packages:+$$apt_packages }$$package" ;; \
			esac; \
		done; \
		for package in $$3; do \
			case " $$brew_packages " in \
				*" $$package "*) ;; \
				*) brew_packages="$${brew_packages:+$$brew_packages }$$package" ;; \
			esac; \
		done; \
	}; \
	finish() { \
		if [ "$$failed" = 0 ]; then return; fi; \
		case '$(UNAME_S)' in \
			Darwin) \
				if [ -n "$$brew_packages" ]; then \
					printf '\nWith Homebrew installed, install the packages with:\n  brew install %s\n' \
						"$$brew_packages" >&2; \
				fi; \
				if [ "$$apple_tools" = 1 ]; then \
					printf '\nIf Apple command-line tools are missing, run:\n  xcode-select --install\n' >&2; \
					printf '\nTo select Homebrew LLVM, run:\n' >&2; \
					printf '  make CXX="$$(brew --prefix llvm)/bin/clang++"\n' >&2; \
				fi ;; \
			Linux) \
				distribution=$$( \
					if [ -r /etc/os-release ]; then . /etc/os-release; fi; \
					printf '%s %s' "$${ID:-}" "$${ID_LIKE:-}" \
				); \
				case " $$distribution " in \
					*' debian '*|*' ubuntu '*) \
						printf '\nOn Debian/Ubuntu, install the packages with:\n  sudo apt install %s\n' \
							"$$apt_packages" >&2; \
						printf 'Package names and LLVM versions can differ between OS releases.\n' >&2 ;; \
					*) printf '\nUse your distribution package manager to install the dependencies above.\n' >&2 ;; \
				esac ;; \
			*) printf '\nAutomatic installation guidance is available for macOS and Debian/Ubuntu.\n' >&2 ;; \
		esac; \
		printf '\nIf already installed, check compiler selection and header/library search paths.\n' >&2; \
		exit 1; \
	};
endef

check-deps:
	@$(dependency_diagnostics) \
	case '$(UNAME_S)' in \
		Linux|Darwin) ;; \
		*) printf 'Unsupported build system: %s. Expected Linux or macOS.\n' '$(UNAME_S)' >&2; exit 1 ;; \
	esac; \
	probe_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/jet-deps.XXXXXX") || exit 1; \
	trap 'rm -rf "$$probe_dir"' 0; \
	trap 'exit 1' 1 2 3 15; \
	printf '%s\n' '#include <format>' '#include <string>' \
		'int main() { return std::format("{}", 42).empty(); }' > "$$probe_dir/core.cc"; \
	toolchain_ok=0; \
	if $(CXX) $(CXXFLAGS) $(LDOPT) "$$probe_dir/core.cc" -o "$$probe_dir/core" \
		> "$$probe_dir/toolchain.log" 2>&1; then \
		toolchain_ok=1; \
	else \
		missing '$(CXX): C++20 toolchain with libc++ std::format and the selected linker/runtime' \
			'clang lld libc++-dev libc++abi-dev libclang-rt-dev' 'llvm'; \
		cat "$$probe_dir/toolchain.log" >&2; \
		apple_tools=1; \
	fi; \
	if [ -n '$(strip $(MODULE_SOKOL_CC))' ]; then \
		case '$(UNAME_S)' in \
			Linux) \
				if command -v $(firstword $(CXX)) >/dev/null 2>&1; then \
					set -f; \
					for dependency in \
						'X11:X11/Xlib.h:X11:libx11-dev' \
						'XInput:X11/extensions/XInput2.h:Xi:libxi-dev' \
						'Xcursor:X11/Xcursor/Xcursor.h:Xcursor:libxcursor-dev' \
						'OpenGL:GL/gl.h:GL:libgl-dev' \
						'ALSA:alsa/asoundlib.h:asound:libasound2-dev'; do \
						saved_ifs=$$IFS; IFS=:; set -- $$dependency; IFS=$$saved_ifs; \
						printf '%s\n' "#if !__has_include(<$$2>)" '#error Missing header' '#endif' \
							'int main() { return 0; }' > "$$probe_dir/module.cc"; \
						if ! $(CXX) $(CXXFLAGS) -E -x c++ "$$probe_dir/module.cc" -o /dev/null \
							> "$$probe_dir/module.log" 2>&1; then \
							missing "$$1 development headers" "$$4" ''; \
						elif [ "$$toolchain_ok" = 1 ] && \
							! $(CXX) $(CXXFLAGS) $(LDOPT) "$$probe_dir/module.cc" -l"$$3" \
								-o "$$probe_dir/module" > "$$probe_dir/module.log" 2>&1; then \
							missing "$$1 development library" "$$4" ''; \
							cat "$$probe_dir/module.log" >&2; \
						fi; \
					done; \
				fi ;; \
			Darwin) \
				if [ "$$toolchain_ok" = 1 ]; then \
					printf '%s\n' '#import <AppKit/AppKit.h>' '#import <QuartzCore/QuartzCore.h>' \
						'#include <OpenGL/gl.h>' '#include <AudioToolbox/AudioToolbox.h>' \
						'int main() { return 0; }' > "$$probe_dir/module.cc"; \
					if ! $(CXX) $(SOKOL_CXXFLAGS) $(LDFLAGS) "$$probe_dir/module.cc" \
						-o "$$probe_dir/module" > "$$probe_dir/module.log" 2>&1; then \
						missing 'macOS SDK frameworks and Objective-C++ support for the DOS module' '' 'llvm'; \
						cat "$$probe_dir/module.log" >&2; \
						apple_tools=1; \
					fi; \
				fi ;; \
		esac; \
	fi; \
	finish

check-tags-deps check-python-deps:
	@$(dependency_diagnostics) \
	case '$@' in \
		check-tags-deps) \
			if ! ctags --version >/dev/null 2>&1; then \
				missing 'Universal Ctags (ctags on PATH)' 'universal-ctags' 'universal-ctags'; \
			fi ;; \
		check-python-deps) \
			if ! python3 --version >/dev/null 2>&1; then \
				missing 'Python 3' 'python3' 'python'; \
			fi ;; \
	esac; \
	finish

# --- Run targets (variant-aware via JET env var) --------------------

# Recipe lines run in order, so a test target never builds variants in parallel.
test: | check-python-deps
	$(Q)$(MAKE) test-release
	$(Q)$(MAKE) test-profile
	$(Q)$(MAKE) sanitize

test-release:
	$(Q)$(MAKE) VARIANT=release
	@printf '  TEST release\n'
	$(Q)cd tests && JET=../build/jet JET_MODULE_TESTS='$(MODULE_TESTS)' ./run-tests

test-profile: | check-python-deps
	$(Q)$(MAKE) VARIANT=profile
	@printf '  TEST profile\n'
	$(Q)cd tests && JET=../build/jet-profile JET_MODULE_TESTS='$(MODULE_TESTS)' ./run-tests
	$(Q)$(MAKE) VARIANT=profile $(BUILD)/profile/profile-test
	$(Q)cd tests && JET=../build/jet-profile ./run-profile-tests

show-sanitizers:
	@list=$$($(CXX) $(CXXFLAGS) -x c++ /dev/null -c -o /dev/null -### 2>&1 | tr ' ' '\n' | \
		sed -n 's/^"-fsanitize=\(.*\)"$$/\1/p'); \
		printf 'sanitizers (%s): %s\n' '$(VARIANT)' "$${list:-none}"

define sanitize_run
	$(Q)$(MAKE) VARIANT=$(1) --no-print-directory show-sanitizers
	@printf '  TEST $(1)\n'
	$(Q)cd tests && $(SANITIZE_ENV) JET=../$(BUILD)/jet$(2) ./run-tests
	@set -e; \
		echo "runnin' benchmarks:"; \
		for benchmark in $(BENCHMARK_SS); do \
			name=$${benchmark#bench/bench-}; \
			name=$${name%.ss}; \
			printf "   %s... " "$$name"; \
			if diagnostics=$$($(SANITIZE_ENV) $(BUILD)/jet$(2) run "$$benchmark" 2>&1 >/dev/null) && \
				! printf '%s' "$$diagnostics" | grep -q '$(SANITIZE_MARKER)'; then \
				echo "success"; \
			else \
				echo "fail"; \
				printf '%s\n' "$$diagnostics" >&2; \
				exit 1; \
			fi; \
		done
endef

sanitize:
	$(Q)$(MAKE) VARIANT=debug
	$(call sanitize_run,debug,-debug)

# Builds its own worktree of REF (default HEAD), so no build dependency.
ab-cross-bench:
	@printf '  BENCH cross\n'
	$(Q)cd bench && ./ab-cross $(REF)

clean:
	@printf '  CLEAN build\n'
	$(Q)rm -rf $(BUILD)

# Sorted tag file so readtags can binary-search (O(log n)) instead of
# scanning linearly. --sort=yes is ctags' default, but we pass it
# explicitly since correctness here depends on it.
tags: | $(BUILD) check-tags-deps
	@printf '  TAGS  %s\n' '$(BUILD)/TAGS'
	$(Q)ctags --sort=yes -f $(BUILD)/TAGS -R $(SRC) $(MODULE_DIRS)

$(JET_BIN): $(ALL_OBJ) | $(BUILD)
	@printf '  LINK  %s\n' '$@'
	$(Q)$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/profile-test: $(OBJDIR)/tests/profile.o \
                      $(patsubst %.cc,$(OBJDIR)/%.o,$(filter-out $(SRC)/main.cc,$(CORE_CC)))
	@printf '  LINK  %s\n' '$@'
	$(Q)$(CXX) $(LDOPT) -o $@ $^

$(OBJDIR)/src/main.o: $(PRELUDE_H) $(MODULES_H)

# opcodes.h includes opcodes_gen.h, so no object can compile before it exists.
$(ALL_OBJ) $(OBJDIR)/tests/profile.o: $(OPCODES_GEN_H)

$(GENOPCODES): $(SRC)/generate-opcodes.cc Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$@'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) $(LDOPT) -o $@ $<

$(OPCODES_GEN_H): $(GENOPCODES) | $(BUILD)
	@printf '  GEN   %s\n' '$@'
	$(Q)$< > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(OBJDIR)/vendor/sokol/%.o: vendor/sokol/%.cc Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(SOKOL_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/vendor/%.o: vendor/%.cpp Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(VENDOR_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/%.o: %.cc Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

FORCE:

$(MODULES_H): FORCE Makefile $(MODULE_MKS) | $(BUILD)
	@{ \
		printf '%s\n' '#pragma once' 'struct VmState;'; \
		for initializer in $(MODULE_INIT); do \
			printf 'void %s(VmState& state);\n' "$$initializer"; \
		done; \
		printf '%s\n' 'inline void init_modules(VmState& state)' '{'; \
		for initializer in $(MODULE_INIT); do \
			printf '\t%s(state);\n' "$$initializer"; \
		done; \
		printf '%s\n' '}'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(PRELUDE_H): FORCE Makefile $(MODULE_MKS) $(PRELUDE_SOURCES) | $(BUILD)
	@{ \
		printf '%s\n' '#pragma once' '#include <cstddef>' \
			'inline constexpr unsigned char prelude_source[] = {'; \
		LC_ALL=C cat $(PRELUDE_SOURCES) | od -An -v -tx1 | \
			awk '{ for (i = 1; i <= NF; ++i) printf "0x%s,", $$i; print "" }'; \
		printf '%s\n' '};' \
			'inline constexpr std::size_t prelude_source_size = sizeof(prelude_source);'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(BUILD):
	@mkdir -p $@

$(OBJDIR): | $(BUILD)
	@mkdir -p $@

-include $(DEPS)
