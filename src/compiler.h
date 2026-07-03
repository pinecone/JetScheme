// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#pragma once

#include <cstdint>
#include <string>
#include <vector>

using Bytecode = std::vector<uint8_t>;

struct CompileFlags
{
	bool inlining = true;
	bool stackify = true;
	bool specialize_ops = true;
};

Bytecode compile(std::string source, std::string filename = "<stdin>", CompileFlags flags = {});

class Env;
void init_reader(Env& e);
