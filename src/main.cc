// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "compiler.h"
#include "debug.h"
#include "error.h"
#include "runtime.h"
#include "vm.h"

using namespace std;

static string find_prelude(const char* argv0)
{
	const char* env = getenv("CITY_PRELUDE");
	if (env && *env)
	{
		return env;
	}
	if (argv0)
	{
		char buf[PATH_MAX];
		if (realpath(argv0, buf))
		{
			string p{buf};
			size_t slash = p.find_last_of('/');
			if (slash != string::npos)
			{
				p.resize(slash);
			}
			return p + "/../lib/prelude.ss";
		}
	}
	return "lib/prelude.ss";
}

static bool slurp_text(const string& path, string& out)
{
	FILE* f = (path == "-") ? stdin : fopen(path.c_str(), "rb");
	if (!f)
	{
		return false;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.append(buf, n);
	}
	if (f != stdin)
	{
		fclose(f);
	}
	return true;
}

static bool slurp_bytes(const string& path, vector<uint8_t>& out)
{
	FILE* f = (path == "-") ? stdin : fopen(path.c_str(), "rb");
	if (!f)
	{
		return false;
	}
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.insert(out.end(), buf, buf + n);
	}
	if (f != stdin)
	{
		fclose(f);
	}
	return true;
}

static int compile_to_bytecode(const string& source_path, const string& prelude_path, bool no_inline,
							   Bytecode& out)
{
	string source;
	CITY_DIE_UNLESS(slurp_text(source_path, source), "error: cannot read '%s'", source_path.c_str());
	if (!prelude_path.empty())
	{
		source = "(include \"" + prelude_path + "\")\n" + source;
	}
	string filename = source_path != "-" ? source_path : "<stdin>";
	out = compile(std::move(source), std::move(filename), no_inline);
	return 0;
}

static int execute_bytecode(vector<uint8_t>& bytecode, int script_argc, char* script_argv[])
{
	Gc gc;
	g_gc = &gc;

	Env primitives_env;
	init_primitives(primitives_env);
	init_cmdline(primitives_env, script_argc, script_argv);

	LoadedProgram prog = load_program(bytecode.data(), bytecode.size(), primitives_env);

	Frame frame = {prog.code, nullptr, 0};
	eval(frame, prog.constants.data(), prog.constants.size(), prog.n_toplevel_slots);

	g_gc = nullptr;
	return 0;
}

static void usage(FILE* o)
{
	fprintf(o, "%s", R"(usage: city <command> [args]
  city <file.ss> [script-args]          shorthand for 'city run <file.ss>'
  city run <file.ss> [script-args]      compile and execute in one step
  city compile [file.ss|-]              compile source to bytecode on stdout
  city exec [file.bc|-] [script-args]   run pre-compiled bytecode
  city disasm [file.ss|file.bc|-]       compile (if .ss) and disassemble

options for run/compile:
  --no-prelude                          skip the bundled prelude
  --no-inline                           disable the function inliner
options for run/exec (debug build only):
  --trace                               print one trace line per opcode dispatched

env:
  CITY_PRELUDE=<path>                   override the prelude path
)");
}

static bool ends_with(const string& s, const char* suffix)
{
	size_t n = strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		usage(stderr);
		return 1;
	}

	string cmd{argv[1]};

	if (cmd == "help" || cmd == "-h" || cmd == "--help")
	{
		usage(stdout);
		return 0;
	}

	int args_start = 2;
	if (cmd != "compile" && cmd != "run" && cmd != "exec" && ends_with(cmd, ".ss"))
	{
		cmd = "run";
		args_start = 1;
	}

	bool want_compile = (cmd == "compile" || cmd == "run" || cmd == "disasm");
	bool want_exec = (cmd == "exec" || cmd == "run");
	bool want_disasm = (cmd == "disasm");
	if (!want_compile && !want_exec)
	{
		fprintf(stderr, "error: unknown command '%s'\n", cmd.c_str());
		usage(stderr);
		return 1;
	}

	bool no_prelude = false;
	bool no_inline = false;
	string input_path;
	int script_arg_start = argc;
	for (int i = args_start; i < argc; ++i)
	{
		if (strcmp(argv[i], "--no-prelude") == 0)
		{
			no_prelude = true;
			continue;
		}
		if (strcmp(argv[i], "--no-inline") == 0)
		{
			no_inline = true;
			continue;
		}
		if (strcmp(argv[i], "--trace") == 0)
		{
#ifdef CITY_TRACE
			g_trace_enabled = true;
#else
			fprintf(stderr, "warning: --trace is only available in debug builds (build/city-debug)\n");
#endif
			continue;
		}
		input_path = argv[i];
		script_arg_start = i + 1;
		break;
	}

	CITY_DIE_WHEN(cmd == "run" && input_path.empty(), "error: 'city run' requires a source file");
	if (input_path.empty())
	{
		input_path = "-";
	}

	vector<uint8_t> bc;

	// disasm: if input is .bc, treat as compiled bytecode; otherwise compile.
	bool input_is_bc = want_disasm && ends_with(input_path, ".bc");

	if (want_compile && !input_is_bc)
	{
		string prelude = no_prelude ? string{} : find_prelude(argv[0]);
		if (compile_to_bytecode(input_path, prelude, no_inline, bc) != 0)
		{
			return 1;
		}
	}
	else
	{
		CITY_DIE_UNLESS(slurp_bytes(input_path, bc), "error: cannot read '%s'", input_path.c_str());
	}

	if (want_disasm)
	{
		disassemble(stdout, bc.data(), bc.size());
		return 0;
	}

	if (want_exec)
	{
		vector<char*> shim;
		shim.push_back(argv[0]);
		for (int i = script_arg_start; i < argc; ++i)
		{
			shim.push_back(argv[i]);
		}
		return execute_bytecode(bc, static_cast<int>(shim.size()), shim.data());
	}

	fwrite(bc.data(), 1, bc.size(), stdout);
	return 0;
}
