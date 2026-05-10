// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#pragma once

#include <cstdint>
#include <string>
#include <vector>

using Bytecode = std::vector<uint8_t>;

Bytecode compile(std::string source, std::string filename = "<stdin>", bool no_inline = false);

class Env;
void init_reader(Env& e);
