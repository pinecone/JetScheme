// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

// Opcode table generator. One declaration per emitted opcode; every trait is
// stated on the row itself. After parsing, one fixup pass resolves the three
// name references (i_of, float_of, fuses) into the base rows' metadata, and
// everything downstream (enum, operand structs, OpcodeInfo, handler and
// disassembly tables, pool-ref offsets) is a straight loop over the rows.

#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

template <typename... Args>
static void print(std::format_string<Args...> fmt, Args&&... args)
{
	std::string text = std::format(fmt, std::forward<Args>(args)...);
	fwrite(text.data(), 1, text.size(), stdout);
}

struct Field
{
	std::string type;
	std::string name;
	size_t offset{}; // byte offset within the packed operand block
};

static Field u16(const char* name)
{
	return {"uint16_t", name};
}

static Field u32(const char* name)
{
	return {"uint32_t", name};
}

static Field u64(const char* name)
{
	return {"uint64_t", name};
}

static Field ptr(const char* name)
{
	return {"Struct*", name};
}

static size_t field_size(const std::string& type)
{
	if (type == "uint8_t")
	{
		return sizeof(uint8_t);
	}
	if (type == "uint16_t")
	{
		return sizeof(uint16_t);
	}
	if (type == "uint32_t")
	{
		return sizeof(uint32_t);
	}
	if (type == "uint64_t")
	{
		return sizeof(uint64_t);
	}
	if (type == "Struct*")
	{
		return sizeof(void*);
	}
	print("unknown field type {}", type);
	std::exit(1);
}

struct Gen;

struct Op
{
	std::string name;        // emitted name (replica-suffixed)
	std::string struct_name; // replica rows alias the struct of their base name
	std::vector<Field> fields;
	std::vector<Field> ic_fields;
	bool is_iform{false};
	std::string i_base;      // iform rows: base op name
	std::string float_base;  // unboxed-float rows: base op name
	std::string float_kind;  // unboxed-float rows only
	std::string fuses_target;
	bool is_if_cmp{false};
	bool is_call_shaped{false};
	std::optional<size_t> pool_ref_index; // index into fields of the pool-ref operand
	size_t operand_size{};                // packed size of fields + ic
	int replica_count{1};   // >1: row expands into name0..name{n-1} sharing one struct
	std::string i_variant;                  // resolved op name (default: self)
	std::string unboxed_float_opcode{"std::nullopt"};
	std::string branch_fusion{"std::nullopt"};

	Op& args(Field field)
	{
		fields.push_back(std::move(field));
		return *this;
	}

	template <typename... Rest>
	Op& args(Field first, Rest... rest)
	{
		return args(std::move(first)).args(std::move(rest)...);
	}

	Op& ic(Field field)
	{
		ic_fields.push_back(std::move(field));
		return *this;
	}

	template <typename... Rest>
	Op& ic(Field first, Rest... rest)
	{
		return ic(std::move(first)).ic(std::move(rest)...);
	}

	// This row is the immediate-operand form of the base op named in i_base.
	Op& iform()
	{
		is_iform = true;
		return *this;
	}

	Op& i_of(const char* base)
	{
		i_base = base;
		return iform();
	}

	// This row is the unboxed-float form of the base op; it aliases the
	// hand-written OP_unboxed_float.
	Op& float_of(const char* base, const char* kind)
	{
		float_base = base;
		float_kind = kind;
		return *this;
	}

	Op& fuses(const char* target)
	{
		fuses_target = target;
		return *this;
	}

	// The named wire operand may hold a pool reference; load_program
	// substitutes the decoded atom.
	Op& pool_ref(const char* field_name)
	{
		for (size_t i = 0; i < fields.size(); ++i)
		{
			if (fields[i].name == field_name)
			{
				pool_ref_index = i;
				return *this;
			}
		}
		print("pool_ref: no field named {}", field_name);
		std::exit(1);
	}

	Op& if_cmp()
	{
		is_if_cmp = true;
		return *this;
	}

	Op& call_shaped()
	{
		is_call_shaped = true;
		return *this;
	}

	// Marks the row to expand into name0..name{n-1} sharing one operand
	// struct; expansion happens after parsing (see Gen::expand_replicas).
	Op& replicas(int n)
	{
		replica_count = n;
		return *this;
	}
};

struct Gen
{
	std::vector<Op> ops;

	Op& op(const char* name)
	{
		Op o;
		o.name = name;
		ops.push_back(std::move(o));
		return ops.back();
	}

	Op& find(const std::string& name)
	{
		for (Op& o : ops)
		{
			if (o.name == name)
			{
				return o;
			}
		}
		print("no opcode named {}", name);
		std::exit(1);
	}

	void expand_replicas()
	{
		std::vector<Op> expanded;
		for (Op& o : ops)
		{
			if (o.replica_count <= 1)
			{
				expanded.push_back(std::move(o));
				continue;
			}
			for (int i = 0; i < o.replica_count; ++i)
			{
				Op replica = o;
				replica.name = o.name + std::to_string(i);
				replica.struct_name = o.name;
				expanded.push_back(std::move(replica));
			}
		}
		ops = std::move(expanded);
	}

	void fixup()
	{
		for (Op& o : ops)
		{
			o.i_variant = o.name;
			size_t offset = 0;
			for (Field& field : o.fields)
			{
				field.offset = offset;
				offset += field_size(field.type);
			}
			for (Field& field : o.ic_fields)
			{
				field.offset = offset;
				offset += field_size(field.type);
			}
			o.operand_size = offset;
		}
		for (Op& o : ops)
		{
			if (!o.i_base.empty())
			{
				find(o.i_base).i_variant = o.name;
			}
			if (!o.float_base.empty())
			{
				Op& base = find(o.float_base);
				base.unboxed_float_opcode = std::format("Opcode::{}", o.name);
				o.unboxed_float_opcode = base.unboxed_float_opcode;
			}
			if (!o.fuses_target.empty())
			{
				find(o.fuses_target);
				o.branch_fusion = std::format("Opcode::{}", o.fuses_target);
			}
		}
		// iform rows inherit their base's float variant, if it has one.
		for (Op& o : ops)
		{
			if (!o.i_base.empty())
			{
				o.unboxed_float_opcode = find(o.i_base).unboxed_float_opcode;
			}
		}
	}
};

static void print_enum(const Gen& gen)
{
	print("enum class Opcode : uint8_t\n{{\n");
	for (const Op& o : gen.ops)
	{
		print("\t{},\n", o.name);
	}
	print("}};\n\nconstexpr int OPCODE_COUNT = {};\n\n", gen.ops.size());
}

static void print_fields(const Op& o)
{
	for (const Field& field : o.fields)
	{
		print("\t{} {};\n", field.type, field.name);
	}
	for (const Field& field : o.ic_fields)
	{
		print("\t{} {};\n", field.type, field.name);
	}
}

static void print_structs(const Gen& gen)
{
	print("#pragma pack(push, 1)\n");
	for (const Op& o : gen.ops)
	{
		if (!o.float_base.empty())
		{
			print("using OP_{} = OP_unboxed_float;\n", o.name);
			continue;
		}
		if (o.struct_name.empty())
		{
			print("struct OP_{} {{\n", o.name);
			print_fields(o);
			print("}};\n");
			continue;
		}
		// Replica rows share their base name's struct.
		if (o.name == o.struct_name + "0")
		{
			print("struct OP_{} {{\n", o.struct_name);
			print_fields(o);
			print("}};\n");
		}
		print("using OP_{} = OP_{};\n", o.name, o.struct_name);
	}
	print("#pragma pack(pop)\n\n");
}

static void print_info(const Gen& gen)
{
	print("struct OpcodeInfo\n{{\n");
	print("\tconst char* name;\n");
	print("\tbool is_if_cmp;\n");
	print("\tbool is_call_shaped;\n");
	print("\tbool is_iform;\n");
	print("\tOpcode i_variant;\n");
	print("\tstd::optional<Opcode> unboxed_float_opcode;\n");
	print("\tstd::optional<Opcode> branch_fusion;\n");
	print("\tsize_t operand_size;\n");
	print("\tUnboxedFloatKind unboxed_float_kind;\n");
	print("}};\n\nconstexpr OpcodeInfo OPCODE_INFO[]{{\n");
	for (const Op& o : gen.ops)
	{
		std::string size_expr = o.float_base.empty()
		                    ? std::to_string(o.operand_size)
		                    : "sizeof(OP_unboxed_float) - std::is_empty_v<OP_unboxed_float>";
		print(
			"\t{{\"{}\", {}, {}, {}, Opcode::{}, {}, {}, {}, UnboxedFloatKind::{}}},\n",
			o.name,
			o.is_if_cmp,
			o.is_call_shaped,
			o.is_iform,
			o.i_variant,
			o.unboxed_float_opcode,
			o.branch_fusion,
			size_expr,
			o.float_kind.empty() ? "None" : o.float_kind);
	}
	print("}};\n\nconstexpr uint8_t OPCODE_POOL_REF_OFFSET[]{{\n");
	for (const Op& o : gen.ops)
	{
		unsigned offset = o.pool_ref_index
			? static_cast<unsigned>(o.fields[*o.pool_ref_index].offset)
			: 255;
		print("\t{},\n", offset);
	}
	print("}};\n\n");
}

static void print_handlers(const Gen& gen)
{
	for (const Op& o : gen.ops)
	{
		print("\top_{},\n", o.name);
	}
}

static void print_disasm(const Gen& gen)
{
	for (const Op& o : gen.ops)
	{
		print("\tdisasm_fields<OP_{}>,\n", o.name);
	}
}

int main(int argc, char* argv[])
{
	Gen gen;

	gen.op("hlt");
	gen.op("skp").args(u32("size"));
	gen.op("loc");
	gen.op("mov").args(u16("dst"), u16("src"));
	gen.op("mov2").args(u16("dst0"), u16("src0"), u16("dst1"), u16("src1"));
	gen.op("ldi").args(u16("dst"), u64("imm")).pool_ref("imm");
	gen.op("ldc").args(u16("dst"), u16("idx"));
	gen.op("ldcb").args(u16("dst"), u16("idx"));
	gen.op("stcb").args(u16("idx"), u16("src"));
	gen.op("ldb").args(u16("dst"), u16("idx"));
	gen.op("stb").args(u16("idx"), u16("src"));
	gen.op("box").args(u16("reg"));
	gen.op("clo").args(u16("dst"), u64("tmpl"), u16("n_captures")).pool_ref("tmpl");

	gen.op("add").args(u16("dst"), u16("a"), u16("b"));
	gen.op("addi").args(u16("dst"), u16("a"), u64("imm")).i_of("add");
	gen.op("addf").float_of("add", "Binary");
	gen.op("sub").args(u16("dst"), u16("a"), u16("b"));
	gen.op("subi").args(u16("dst"), u16("a"), u64("imm")).i_of("sub");
	gen.op("subf").float_of("sub", "Binary");
	gen.op("mul").args(u16("dst"), u16("a"), u16("b"));
	gen.op("muli").args(u16("dst"), u16("a"), u64("imm")).i_of("mul");
	gen.op("mulf").float_of("mul", "Binary");
	gen.op("div").args(u16("dst"), u16("a"), u16("b"));
	gen.op("divi").args(u16("dst"), u16("a"), u64("imm")).i_of("div");
	gen.op("divf").float_of("div", "Binary");

	gen.op("cmp").args(u16("dst"), u16("a"), u16("b")).fuses("bcmp");
	gen.op("cmpi").args(u16("dst"), u16("a"), u64("imm")).i_of("cmp").fuses("bcmpi");
	gen.op("cmpf").float_of("cmp", "Comparison");
	gen.op("eq").args(u16("dst"), u16("a"), u16("b")).fuses("beq");
	gen.op("eqi").args(u16("dst"), u16("a"), u64("imm")).i_of("eq").pool_ref("imm").fuses("beqi");
	gen.op("lt").args(u16("dst"), u16("a"), u16("b")).fuses("blt");
	gen.op("lti").args(u16("dst"), u16("a"), u64("imm")).i_of("lt").fuses("blti");
	gen.op("ltf").float_of("lt", "Comparison");
	gen.op("le").args(u16("dst"), u16("a"), u16("b")).fuses("ble");
	gen.op("lef").float_of("le", "Comparison");
	gen.op("gt").args(u16("dst"), u16("a"), u16("b")).fuses("bgt");
	gen.op("gtf").float_of("gt", "Comparison");
	gen.op("ge").args(u16("dst"), u16("a"), u16("b")).fuses("bge");
	gen.op("gef").float_of("ge", "Comparison");

	gen.op("bn").args(u16("src"), u32("size"));
	gen.op("bcmp").if_cmp().args(u16("a"), u16("b"), u32("size"));
	gen.op("bcmpi").if_cmp().args(u16("a"), u64("imm"), u32("size")).i_of("bcmp").pool_ref("imm");
	gen.op("beq").if_cmp().args(u16("a"), u16("b"), u32("size"));
	gen.op("beqi").if_cmp().args(u16("a"), u64("imm"), u32("size")).i_of("beq").pool_ref("imm");
	gen.op("blt").if_cmp().args(u16("a"), u16("b"), u32("size"));
	gen.op("blti").if_cmp().args(u16("a"), u64("imm"), u32("size")).i_of("blt").pool_ref("imm");
	gen.op("ble").if_cmp().args(u16("a"), u16("b"), u32("size"));
	gen.op("bgt").if_cmp().args(u16("a"), u16("b"), u32("size"));
	gen.op("bge").if_cmp().args(u16("a"), u16("b"), u32("size"));

	gen.op("ret").args(u16("src"));
	gen.op("c").call_shaped().args(u16("w"), u16("callee"), u16("nargs"));
	gen.op("ct").args(u16("w"), u16("callee"), u16("nargs"));
	gen.op("cst").call_shaped().args(u16("w"), u16("nargs"));
	gen.op("cs").call_shaped().args(u16("w"), u16("nargs")).replicas(4);
	gen.op("app").args(u16("w"));
	gen.op("itn1").args(u16("cursor"), u16("dst"), u32("size")).ic(u64("dispatch_key"));
	gen.op("itn2").args(u16("cursor"), u16("dst0"), u16("dst1"), u32("size")).ic(u64("dispatch_key"));
	gen.op("cl").call_shaped()
		.args(u16("w"), u16("idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_atom"), u64("ic_code"))
		.replicas(4);
	gen.op("clt")
		.args(u16("w"), u16("idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_atom"), u64("ic_code"))
		.replicas(4);
	gen.op("cc").call_shaped()
		.args(u16("w"), u16("idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_atom"), u64("ic_code"))
		.replicas(4);
	gen.op("cct")
		.args(u16("w"), u16("idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_atom"), u64("ic_code"))
		.replicas(4);
	gen.op("ccb").call_shaped()
		.args(u16("w"), u16("upvalue_idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_slot"), u64("ic_atom"), u64("ic_code"), u64("ic_version"))
		.replicas(4);
	gen.op("ccbt")
		.args(u16("w"), u16("upvalue_idx"), u16("nargs"))
		.ic(u16("ic_n_locals"), u32("ic_epoch"), u64("ic_slot"), u64("ic_atom"), u64("ic_code"), u64("ic_version"))
		.replicas(4);
	gen.op("ldk").args(u16("dst"), u16("obj"), u16("key"))
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("ldkm").args(u16("dst"), u16("obj"), u16("key"))
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("ldki").args(u16("dst"), u16("obj"), u64("key")).i_of("ldk").pool_ref("key")
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("ldkmi").args(u16("dst"), u16("obj"), u64("key")).i_of("ldkm").pool_ref("key")
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("stk").args(u16("obj"), u16("key"), u16("val"))
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("stki").args(u16("obj"), u64("key"), u16("val")).i_of("stk").pool_ref("key")
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("ldkd").args(u16("dst"), u16("obj"), u16("key"), u16("dfl"))
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));
	gen.op("ldkdi").args(u16("dst"), u16("obj"), u64("key"), u16("dfl")).i_of("ldkd").pool_ref("key")
		.ic(u64("dispatch_key"), u64("cached_index"), u64("cached_key"));

	gen.op("rst").call_shaped().args(u16("w"));
	gen.op("retx").args(ptr("escape"));
	gen.op("coro").call_shaped().args(u16("w"));
	gen.op("retr");
	gen.op("retn");
	gen.op("reth");

	gen.op("trunc").args(u16("dst"), u16("src"));
	gen.op("truncf").float_of("trunc", "Unary");
	gen.op("sqrt").args(u16("dst"), u16("src"));
	gen.op("sqrtf").float_of("sqrt", "Unary");
	gen.op("floor").args(u16("dst"), u16("src"));
	gen.op("floorf").float_of("floor", "Unary");
	gen.op("round").args(u16("dst"), u16("src"));
	gen.op("roundf").float_of("round", "Unary");
	gen.op("ceil").args(u16("dst"), u16("src"));
	gen.op("ceilf").float_of("ceil", "Unary");

	gen.op("min").args(u16("dst"), u16("a"), u16("b"));
	gen.op("mini").args(u16("dst"), u16("a"), u64("imm")).i_of("min");
	gen.op("minf").float_of("min", "Binary");
	gen.op("max").args(u16("dst"), u16("a"), u16("b"));
	gen.op("maxi").args(u16("dst"), u16("a"), u64("imm")).i_of("max");
	gen.op("maxf").float_of("max", "Binary");

	gen.expand_replicas();
	gen.fixup();

	if (argc == 2 && std::string_view{argv[1]} == "handlers")
	{
		print_handlers(gen);
	}
	else if (argc == 2 && std::string_view{argv[1]} == "disasm")
	{
		print_disasm(gen);
	}
	else
	{
		print_enum(gen);
		print_structs(gen);
		print_info(gen);
	}

	return 0;
}
