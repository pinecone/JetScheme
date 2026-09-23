#include <cstdio>
#include <string>
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

struct Opcode
{
  std::string name;
  std::string display;
  std::vector<Operand> operands;
  std::vector<IcField> icfields;
  bool op_variants[256]{};
  bool is_replicated{false};

  Opcode& shown_as(const char* text)
  {
    display = text;
    return *this;
  }

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
    return *this;
  }

  Opcode& replicated()
  {
    is_replicated = true;
    return *this;
  }

  template <typename Func>
  void apply(Func&& f)
  {
    const std::string& shown = display.empty() ? name : display;
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
        apply([&](const std::string&, const std::string& name, const std::string& shown)
              {
                fr(name, name + suffix, shown + suffix);
              });
      }
    }
  }
};

struct Gen
{
  std::vector<Opcode> opcodes;

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
    printf("#define JET_OPCODES(X) \\\n");
    for (size_t i = 0; i < opcodes.size(); ++i)
    {
      Opcode& op = opcodes[i];

      auto&& p = [&](const std::string& name, const std::string& shown)
      {
        printf("\tX(%s, \"%s\") \\\n", name.c_str(), shown.c_str());
      };

      op.foreach_variant([&](const std::string&, const std::string& name, const std::string& shown)
                         {
                           if (!op.is_replicated) { p(name, shown); }
                         },
                         [&](const std::string&, const std::string& name, const std::string& shown)
                         {
                           p(name, shown);
                         });
    }
    printf("\n\n");

    printf("#pragma pack(push, 1)\n");
    for (Opcode& op : opcodes)
    {
      print_struct(op);
    }
    printf("#pragma pack(pop)\n");

    printf("\n");
  };
};

int main()
{
  Gen g;

  g.opcode("halt");

  g.opcode("skip")
    .shown_as("b")
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
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("sub")
    .variants("kf")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("mul")
    .variants("kf")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("div")
    .variants("kf")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("numeq")
    .variants("kf")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("eq")
    .variants("k")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("lt")
    .variants("kf")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("le")
    .variants("f")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("gt")
    .variants("f")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("ge")
    .variants("f")
	  .operand("uint16_t", "dst")
	  .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("if_false")
    .shown_as("if")
	  .operand("uint16_t", "src")
    .operand("uint32_t", "size");

  g.opcode("if_numeq")
    .shown_as("ifnumeq")
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_eq")
    .shown_as("ifeq")
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_lt")
    .shown_as("iflt")
    .variants("k")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_le")
    .shown_as("ifle")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_gt")
    .shown_as("ifgt")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("if_ge")
    .shown_as("ifge")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b")
    .operand("uint32_t", "size");

  g.opcode("retv")
    .shown_as("ret")
    .operand("uint16_t", "src");

  g.opcode("call")
	  .operand("uint16_t", "w")
    .operand("uint16_t", "callee")
    .operand("uint16_t", "nargs");

  g.opcode("tcall")
	  .operand("uint16_t", "w")
    .operand("uint16_t", "callee")
    .operand("uint16_t", "nargs");

  g.opcode("call_self_tail")
    .shown_as("cselft")
	  .operand("uint16_t", "w")
    .operand("uint16_t", "nargs");

  g.opcode("call_self")
    .shown_as("cself")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "nargs");

  g.opcode("apply")
    .operand("uint16_t", "w");

  g.opcode("iter_next1")
    .shown_as("iter1")
	  .operand("uint16_t", "cursor")
    .operand("uint16_t", "dst")
    .operand("uint32_t", "size")
    .icfield("uint64_t", "dispatch_key");

  g.opcode("iter_next2")
    .shown_as("iter2")
	  .operand("uint16_t", "cursor")
    .operand("uint16_t", "dst0")
    .operand("uint16_t", "dst1")
    .operand("uint32_t", "size")
    .icfield("uint64_t", "dispatch_key");

  g.opcode("call_local")
    .shown_as("cl")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_local_tail")
    .shown_as("clt")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval")
    .shown_as("cu")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval_tail")
    .shown_as("cut")
    .replicated()
	  .operand("uint16_t", "w")
    .operand("uint16_t", "idx")
    .operand("uint16_t", "nargs")
    .icfield("uint16_t", "ic_n_locals")
    .icfield("uint32_t", "ic_epoch")
    .icfield("uint64_t", "ic_atom")
    .icfield("uint64_t", "ic_code");

  g.opcode("call_upval_slot")
    .shown_as("cus")
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

  g.opcode("call_upval_slot_tail")
    .shown_as("cust")
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
    .operand("uint16_t", "w");

  g.opcode("retk")
    .operand("Struct*", "escape");

  g.opcode("coro")
    .operand("uint16_t", "w");

  g.opcode("retc");
  g.opcode("retu");
  g.opcode("return_to_host").shown_as("rethost");

  g.opcode("trunc")
    .variants("f")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("sqrt")
    .variants("f")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("floor")
    .variants("f")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("round")
    .variants("f")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("ceil")
    .variants("f")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "src");

  g.opcode("min")
    .variants("kf")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b");

  g.opcode("max")
    .variants("kf")
	  .operand("uint16_t", "dst")
    .operand("uint16_t", "a")
    .operand("uint16_t", "b");
  
  g.print();

  return 0;
}
