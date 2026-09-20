// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "debug.h"
#include "error.h"
#include "modules.h"
#include "prelude.h"
#include "runtime.h"
#include "vm.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

static bool slurp_text(const std::string& path, std::string& out)
{
	FILE* file{(path == "-") ? stdin : std::fopen(path.c_str(), "rb")};
	if (!file)
	{
		return false;
	}
	char buf[4096];
	std::size_t len;
	while ((len = std::fread(buf, 1, sizeof(buf), file)) > 0)
	{
		out.append(buf, len);
	}
	if (file != stdin)
	{
		std::fclose(file);
	}
	return true;
}

static bool slurp_bytes(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* file{(path == "-") ? stdin : std::fopen(path.c_str(), "rb")};
	if (!file)
	{
		return false;
	}
	uint8_t buf[4096];
	std::size_t len;
	while ((len = std::fread(buf, 1, sizeof(buf), file)) > 0)
	{
		out.insert(out.end(), buf, buf + len);
	}
	if (file != stdin)
	{
		std::fclose(file);
	}
	return true;
}

static Bytecode compile_source(
	std::string source,
	std::string filename,
	const std::string& prelude_path,
	std::string_view built_in_prelude,
	CompileFlags flags)
{
	if (!prelude_path.empty())
	{
		source = "(include \"" + prelude_path + "\")\n" + source;
	}
	return compile(std::move(source), std::move(filename), flags, built_in_prelude);
}

[[noreturn]] static void execute_bytecode(const CodeImage& image, int script_argc, char* script_argv[])
{
	Env primitives_env;
	VmState vm{.env = primitives_env};

	init_runtime(vm);
	init_modules(vm);
	init_cmdline(vm, script_argc, script_argv);

	LoadedProgram prog{load_program(vm, image.bytes, image.size)};

	Frame frame{prog.code, nullptr, 0, prog.n_toplevel_slots};
	eval(vm, frame, prog.constants.data(), prog.constants.size(), prog.n_toplevel_slots);
}

static void usage(FILE* output)
{
	static constexpr char USAGE_TEXT[] =
		R"(usage: jet <command> [args]
  jet <file.ss> [script-args]          shorthand for 'jet run <file.ss>'
  jet run <file.ss> [script-args]      compile and execute in one step
  jet eval <expr> [script-args]        compile and execute an inline expression
  jet compile [file.ss|-]              compile source to bytecode on stdout
  jet exec [file.bc|-] [script-args]   run pre-compiled bytecode
  jet disasm [file.ss|file.bc|-]       compile (if .ss) and disassemble

options for run/compile:
  --no-prelude                          skip the bundled prelude

optimization options (generated code is slower):
  --no-inline                           disable the inliner
  --no-lift-lambdas                     disable the lambda lifter
  --no-specialize-ops                   emit generic opcodes only
options for run/exec (debug build only):
  --trace                               print one trace line per opcode dispatched

env:
  JET_PRELUDE=<path>                   override the prelude path
)";
	std::fputs(USAGE_TEXT, output);
}

static bool ends_with(const std::string& text, const char* suffix)
{
	std::size_t len{std::strlen(suffix)};
	return text.size() >= len && text.compare(text.size() - len, len, suffix) == 0;
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		usage(stderr);
		return 1;
	}

	std::string cmd{argv[1]};

	if (cmd == "help" || cmd == "-h" || cmd == "--help")
	{
		usage(stdout);
		return 0;
	}

	int args_start{2};
	if (cmd != "compile" && cmd != "run" && cmd != "exec" && ends_with(cmd, ".ss"))
	{
		cmd = "run";
		args_start = 1;
	}

	bool want_eval{cmd == "eval"};
	bool want_compile{cmd == "compile" || cmd == "run" || cmd == "disasm" || want_eval};
	bool want_exec{cmd == "exec" || cmd == "run" || want_eval};
	bool want_disasm{cmd == "disasm"};
	if (!want_compile && !want_exec)
	{
		print(stderr, "error: unknown command '{}'\n", cmd);
		usage(stderr);
		return 1;
	}

	bool no_prelude{false};
	CompileFlags flags{};
	std::string input_path;
	int script_arg_start{argc};
	for (int i{args_start}; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--no-prelude") == 0)
		{
			no_prelude = true;
			continue;
		}
		if (std::strcmp(argv[i], "--no-inline") == 0)
		{
			flags.inlining = false;
			continue;
		}
		if (std::strcmp(argv[i], "--no-lift-lambdas") == 0)
		{
			flags.lift_lambdas = false;
			continue;
		}
		if (std::strcmp(argv[i], "--no-specialize-ops") == 0)
		{
			flags.specialize_ops = false;
			continue;
		}
		if (std::strcmp(argv[i], "--trace") == 0)
		{
#ifdef JET_TRACE
			g_trace_enabled = true;
#else
			print(stderr, "warning: --trace is only available in debug builds (build/jet-debug)\n");
#endif
			continue;
		}
		input_path = argv[i];
		script_arg_start = i + 1;
		break;
	}

	JET_DIE_WHEN(nullptr, cmd == "run" && input_path.empty(), "error: 'jet run' requires a source file");
	JET_DIE_WHEN(nullptr, want_eval && input_path.empty(), "error: 'jet eval' requires an expression");
	if (input_path.empty())
	{
		input_path = "-";
	}

	std::vector<uint8_t> bc;

	if (bool input_is_bc = want_disasm && ends_with(input_path, ".bc"); want_compile && !input_is_bc)
	{
		const char* prelude_override{no_prelude ? nullptr : std::getenv("JET_PRELUDE")};
		std::string prelude_path{prelude_override && *prelude_override ? prelude_override : ""};
		std::string_view built_in_prelude;
		if (!no_prelude && prelude_path.empty())
		{
			built_in_prelude = std::string_view{
				reinterpret_cast<const char*>(prelude_source),
				prelude_source_size};
		}
		std::string source;
		std::string filename;
		if (want_eval)
		{
			source = input_path;
			filename = "<eval>";
		}
		else
		{
			JET_DIE_UNLESS(nullptr, slurp_text(input_path, source), "error: cannot read '{}'", input_path);
			filename = input_path != "-" ? input_path : "<stdin>";
		}
		bc = compile_source(std::move(source), std::move(filename), prelude_path, built_in_prelude, flags);
	}
	else
	{
		JET_DIE_UNLESS(nullptr, slurp_bytes(input_path, bc), "error: cannot read '{}'", input_path);
	}

	CodeImage image{bc.data(), bc.size()};

	if (want_disasm)
	{
		disassemble(stdout, image.bytes, image.size);
		return 0;
	}

	if (want_exec)
	{
		std::vector<char*> shim;
		shim.push_back(argv[0]);
		for (int i{script_arg_start}; i < argc; ++i)
		{
			shim.push_back(argv[i]);
		}
		execute_bytecode(image, static_cast<int>(shim.size()), shim.data());
	}

	std::fwrite(bc.data(), 1, bc.size(), stdout);
	return 0;
}
