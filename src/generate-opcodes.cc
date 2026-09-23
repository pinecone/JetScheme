#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static constexpr int N_REPLICAS = 4;

struct Operand
{
  const char* type;
  const char* name;
};

struct IcField
{
  const char* type;
  const char* name;
};

struct Metadata
{
  std::string type;
  std::string name;
  std::string value;
};

static std::string expand_name(std::string text, const std::string& name)
{
  size_t pos = 0;
  while ((pos = text.find("{name}", pos)) != std::string::npos)
  {
    text.replace(pos, 6, name);
    pos += name.size();
  }
  return text;
}

struct VariantMetadata
{
  std::string variant;
  Metadata field;
};

struct Opcode
{
  std::string name;
  std::vector<Operand> operands;
  std::vector<IcField> icfields;
  bool op_variants[256]{};
  bool is_replicated{false};
  std::vector<Metadata> metadata_values;
  std::vector<VariantMetadata> variant_metadata_values;

  Opcode& operand(const char* type, const char* name)
  {
    operands.push_back(Operand{type, name});
    return *this;
  }

  Opcode& icfield(const char* type, const char* name)
  {
    icfields.push_back(IcField{type, name});
    return *this;
  }

  Opcode& variants(const char* vs)
  {
    for (const char* v = vs; *v; ++v)
    {
      op_variants[static_cast<unsigned char>(*v)] = true;
    }
    if (op_variants['k'])
    {
      base_metadata("Opcode", "k_variant", "Opcode::{name}k");
      variant_metadata("k", "bool", "is_kform", "true");
      if (op_variants['h'])
      {
        variant_metadata("kh", "bool", "is_kform", "true");
      }
    }
    if (op_variants['f'])
    {
      std::string value{"Opcode::f"};
      value += name;
      metadata("std::optional<Opcode>", "unboxed_float_opcode", value.c_str());
    }
    return *this;
  }

  Opcode& replicated()
  {
    is_replicated = true;
    return *this;
  }

  Opcode& metadata(const char* type, const char* name, const char* value)
  {
    metadata_values.push_back(Metadata{type, name, value});
    return *this;
  }

  Opcode& variant_metadata(const char* variant, const char* type, const char* name, const char* value)
  {
    variant_metadata_values.push_back(VariantMetadata{variant, Metadata{type, name, value}});
    return *this;
  }

  Opcode& base_metadata(const char* type, const char* name, const char* value)
  {
    return variant_metadata("", type, name, value);
  }

  Opcode& if_comparison()
  {
    return metadata("bool", "is_if_cmp", "true");
  }

  Opcode& call_shaped()
  {
    return metadata("bool", "is_call_shaped", "true");
  }

  Opcode& branch_fusion()
  {
    base_metadata("std::optional<Opcode>", "branch_fusion", "Opcode::if_{name}");
    if (op_variants['k'])
    {
      variant_metadata("k", "std::optional<Opcode>", "branch_fusion", "Opcode::if_{name}");
    }
    return *this;
  }

  Opcode& unboxed_float_kind(const char* kind)
  {
    std::string value{"UnboxedFloatKind::"};
    value += kind;
    return variant_metadata("f", "UnboxedFloatKind", "unboxed_float_kind", value.c_str());
  }

  template <typename Func>
  void apply(Func&& f)
  {
    const std::string& shown = name;
    f("", name, shown);
    if (op_variants['h'])
    {
      f("h", name + "h", shown + "h");
    }
    if (op_variants['k'])
    {
      f("k", name + "k", shown + "k");
      if (op_variants['h'])
      {
        f("kh", name + "kh", shown + "kh");
      }
    }
    if (op_variants['f'])
    {
      f("f", "f" + name, "f" + shown);
    }
  }

  template <typename Func, typename FuncRep>
  void foreach_variant(Func&& f, FuncRep&& fr)
  {
    apply(f);

    if (is_replicated)
    {
      for (int i = 0; i < N_REPLICAS; ++i)
      {
        std::string suffix = "_" + std::to_string(i);
        apply([&](const std::string& variant, const std::string& name, const std::string&)
              {
                fr(name, name + suffix, variant);
              });
      }
    }
  }
};

struct Gen
{
  std::vector<Opcode> opcodes;
  std::vector<Metadata> metadata_fields;

  Gen& metadata(const char* type, const char* name, const char* default_value)
  {
    metadata_fields.push_back(Metadata{type, name, default_value});
    return *this;
  }

  Opcode& opcode(const char* name)
  {
    opcodes.emplace_back();
    opcodes.back().name = name;
    return opcodes.back();
  }

  void print_struct(Opcode& op)
  {
    auto&& p = [&](const std::string& variant, const std::string& name, const std::string&)
    {
      if (variant == "f")
      {
        printf("using OP_%s = OP_unboxed_float;\n", name.c_str());
        return;
      }

      printf("struct OP_%s {\n", name.c_str());
      for (Operand& operand : op.operands)
      {
        printf("\t%s %s;\n", operand.type, operand.name);
      }
      for (IcField& field : op.icfields)
      {
        printf("\t%s %s;\n", field.type, field.name);
      }
      printf("};\n");
    };

    auto&& pr = [&](const std::string& name, const std::string& rname, const std::string&)
    {
      printf("using OP_%s = OP_%s;\n", rname.c_str(), name.c_str());
    };

    op.foreach_variant(p, pr);
  }

  void print()
  {
    printf("enum class Opcode : uint8_t\n{\n");
    size_t count = 0;
    for (Opcode& op : opcodes)
    {
      auto&& emit = [&](const std::string& name)
      {
        printf("\t%s,\n", name.c_str());
        ++count;
      };
      op.foreach_variant([&](const std::string&, const std::string& name, const std::string&)
                         {
                           if (!op.is_replicated) { emit(name); }
                         },
                         [&](const std::string&, const std::string& name, const std::string&)
                         {
                           emit(name);
                         });
    }
    printf("};\n\nconstexpr int OPCODE_COUNT = %zu;\n\n", count);
    printf("#pragma pack(push, 1)\n");
    for (Opcode& op : opcodes)
    {
      print_struct(op);
    }
    printf("#pragma pack(pop)\n\n");

    printf("struct OpcodeInfo\n{\n");
    for (const Metadata& field : metadata_fields)
    {
      printf("\t%s %s;\n", field.type.c_str(), field.name.c_str());
    }
    printf("};\n\nconstexpr OpcodeInfo OPCODE_INFO[]{\n");
    for (Opcode& op : opcodes)
    {
      auto&& emit = [&](const std::string& variant, const std::string& name)
      {
        printf("\t{");
        for (size_t i = 0; i < metadata_fields.size(); ++i)
        {
          const Metadata& field = metadata_fields[i];
          std::string value = field.value;
          for (const Metadata& override : op.metadata_values)
          {
            if (override.type == field.type && override.name == field.name)
            {
              value = override.value;
            }
          }
          for (const VariantMetadata& override : op.variant_metadata_values)
          {
            if (override.variant == variant && override.field.type == field.type && override.field.name == field.name)
            {
              value = override.field.value;
            }
          }
          printf("%s%s", i ? ", " : "", expand_name(value, name).c_str());
        }
        printf("},\n");
      };
      op.foreach_variant([&](const std::string& variant, const std::string& name, const std::string&)
                         {
                           if (!op.is_replicated) { emit(variant, name); }
                         },
                         [&](const std::string&, const std::string& name, const std::string& variant)
                         {
                           emit(variant, name);
                         });
    }
    printf("};\n\n");
  }

  void print_disasm()
  {
    for (Opcode& op : opcodes)
    {
      auto&& emit = [&](const std::string& name)
      {
        printf("\tdisasm_fields<OP_%s>,\n", name.c_str());
      };
      op.foreach_variant([&](const std::string&, const std::string& name, const std::string&)
                         {
                           if (!op.is_replicated) { emit(name); }
                         },
                         [&](const std::string&, const std::string& name, const std::string&)
                         {
                           emit(name);
                         });
    }
  }

  void print_handlers()
  {
    for (Opcode& op : opcodes)
    {
      auto&& emit = [&](const std::string& name)
      {
        printf("\top_%s,\n", name.c_str());
      };
      op.foreach_variant([&](const std::string&, const std::string& name, const std::string&)
                         {
                           if (!op.is_replicated) { emit(name); }
                         },
                         [&](const std::string&, const std::string& name, const std::string&)
                         {
                           emit(name);
                         });
    }
  }
};

int main(int argc, char* argv[])
{
  Gen g;
  g.metadata("const char*", "name", "\"{name}\"")
    .metadata("bool", "is_if_cmp", "false")
    .metadata("bool", "is_call_shaped", "false")
    .metadata("bool", "is_kform", "false")
    .metadata("Opcode", "k_variant", "Opcode::{name}")
    .metadata("std::optional<Opcode>", "unboxed_float_opcode", "std::nullopt")
    .metadata("std::optional<Opcode>", "branch_fusion", "std::nullopt")
    .metadata("size_t", "operand_size", "sizeof(OP_{name}) - std::is_empty_v<OP_{name}>")
    .metadata("UnboxedFloatKind", "unboxed_float_kind", "UnboxedFloatKind::None");

  g.opcode("halt");

  g.opcode("skip")
    .operand("size_t", "size");

  g.opcode("label");

  g.opcode("mov")
    .operand("uint16_t", "dst")
    .operand("uint16_t", "src");
  g.opcode("mov2")
    .operand("uint16_t", "dst0")
    .operand("uint16_t", "src0")
    .operand("uint16_t", "dst1")
    .operand("uint16_t", "src1");

  g.opcode("ldk")
    .operand("uint16_t", "dst")
    .operand("uint16_t", "idx");
  g.opcode("ldu")
    .operand("uint16_t", "dst")
    .operand("uint16_t", "idx");
  g.opcode("ldus")
    .operand("uint16_t", "dst")
    .operand("uint16_t", "idx");
  g.opcode("stu")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "src");

  g.opcode("ldd")
    .operand("uint16_t", "dst")
    .operand("uint16_t", "idx");
  g.opcode("std")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "src");

  g.opcode("box")
    .operand("uint16_t", "reg");

  g.opcode("clos")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "pool_idx")
    .operand("uint16_t", "n_captures");

  g.opcode("add")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("sub")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("mul")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("div")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("numeq")
    .variants("kf")
    .branch_fusion()
    .unboxed_float_kind("Comparison")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("eq")
    .variants("k")
    .branch_fusion()
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("lt")
    .variants("kf")
    .branch_fusion()
    .unboxed_float_kind("Comparison")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("le")
    .variants("f")
    .branch_fusion()
    .unboxed_float_kind("Comparison")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("gt")
    .variants("f")
    .branch_fusion()
    .unboxed_float_kind("Comparison")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("ge")
    .variants("f")
    .branch_fusion()
    .unboxed_float_kind("Comparison")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("if_false")
	  .operand("uint16_t", "src")
    .operand("uint32_t", "size");

  g.opcode("if_numeq")
    .if_comparison()
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_eq")
    .if_comparison()
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_lt")
    .if_comparison()
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_le")
    .if_comparison()
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_gt")
    .if_comparison()
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_ge")
    .if_comparison()
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("retv")
    .operand("uint16_t", "src");

  g.opcode("call")
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "callee")
    .operand("uint16_t", "nargs");

  g.opcode("tcall")
	  .operand("uint16_t", "w")
    .operand("uint16_t", "callee")
    .operand("uint16_t", "nargs");

  g.opcode("call_self_tail")
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "nargs");

  g.opcode("call_self")
    .replicated()
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "nargs");

  g.opcode("apply")
    .operand("uint16_t", "w");

  g.opcode("iter_next1")
	  .operand("uint16_t", "cursor")
    .operand("uint16_t", "dst")
    .operand("uint32_t", "size")
    .icfield("uint64_t", "dispatch_key");

  g.opcode("iter_next2")
	  .operand("uint16_t", "cursor")
    .operand("uint16_t", "dst0")
    .operand("uint16_t", "dst1")
    .operand("uint32_t", "size")
    .icfield("uint64_t", "dispatch_key");

  g.opcode("call_local")
    .replicated()
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_local_tail")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval")
    .replicated()
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval_tail")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval_slot")
    .replicated()
    .call_shaped()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "upvalue_idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_slot")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code")
    .icfield("uint64_t", "ic_version");

  g.opcode("call_upval_slot_tail")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "upvalue_idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_slot")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code")
    .icfield("uint64_t", "ic_version");

  g.opcode("ldf")
    .variants("kh")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "obj")
    .operand("uint16_t", "key")
	  .icfield("uint64_t", "dispatch_key")
    .icfield("uint64_t", "cached_index")
    .icfield("uint64_t", "cached_key");

  g.opcode("stf")
    .variants("k")
	  .operand("uint16_t", "obj")
	  .operand("uint16_t", "key")
    .operand("uint16_t", "val")
	  .icfield("uint64_t", "dispatch_key")
    .icfield("uint64_t", "cached_index")
    .icfield("uint64_t", "cached_key");

  g.opcode("ldfo")
    .variants("k")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "obj")
    .operand("uint16_t", "key")
    .operand("uint16_t", "dfl")
	  .icfield("uint64_t", "dispatch_key")
    .icfield("uint64_t", "cached_index")
    .icfield("uint64_t", "cached_key");

  g.opcode("reset")
    .call_shaped()
    .operand("uint16_t", "w");

  g.opcode("retk")
    .operand("Struct*", "escape");

  g.opcode("coro")
    .call_shaped()
    .operand("uint16_t", "w");

  g.opcode("retc");
  g.opcode("retu");
  g.opcode("return_to_host");

  g.opcode("trunc")
    .variants("f")
    .unboxed_float_kind("Unary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("sqrt")
    .variants("f")
    .unboxed_float_kind("Unary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("floor")
    .variants("f")
    .unboxed_float_kind("Unary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("round")
    .variants("f")
    .unboxed_float_kind("Unary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("ceil")
    .variants("f")
    .unboxed_float_kind("Unary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("min")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("max")
    .variants("kf")
    .unboxed_float_kind("Binary")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b");
  
  if (argc == 2 && std::string_view{argv[1]} == "handlers")
  {
    g.print_handlers();
  }
  else if (argc == 2 && std::string_view{argv[1]} == "disasm")
  {
    g.print_disasm();
  }
  else
  {
    g.print();
  }

  return 0;
}
