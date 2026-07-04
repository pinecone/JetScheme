// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "error.h"
#include "runtime.h"
#include "vm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef __APPLE__
#include <xlocale.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <locale.h>
#include <stdlib.h>
#endif

struct SourceLoc
{
	uint32_t file_id = 0;
	int line = 0;
	int col = 0;
};

enum class TokenKind : uint8_t
{
	LParen,
	RParen,
	Quote,
	Hash,
	Number,
	String,
	Boolean,
	Character,
	Variable,
	Lambda,
	Define,
	If,
	Set,
	Setf,
	QuoteWord,
	Apply,
	Let,
	LetStar,
	Letrec,
	Begin,
	When,
	Unless,
	Cond,
	And,
	Or,
	Include,
	Dot,
	Eof,
};

struct Token
{
	TokenKind kind;
	std::string_view text;
	SourceLoc loc;
};

template <class T>
struct Slice
{
	T* data = nullptr;
	uint32_t count = 0;

	T* begin() { return data; }
	T* end() { return data + count; }
	uint32_t size() { return count; }
	bool empty() { return count == 0; }
	T& operator[](uint32_t i) { return data[i]; }
	T& back() { return data[count - 1]; }
};

struct Arena
{
	static constexpr size_t BLOCK_SIZE = 16384;

	struct Block
	{
		char* mem;
		size_t size;
	};

	std::vector<Block> blocks;
	char* ptr = nullptr;
	char* end = nullptr;

	Arena() = default;
	Arena(Arena&) = delete;
	Arena& operator=(Arena&) = delete;

	~Arena()
	{
		for (Block& b : blocks)
		{
			delete[] b.mem;
		}
	}

	void* alloc_raw(size_t size, size_t align)
	{
		uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
		p = (p + align - 1) & ~(align - 1);
		char* aligned = reinterpret_cast<char*>(p);

		if (ptr == nullptr || aligned + size > end)
		{
			size_t block_size = size + align > BLOCK_SIZE ? size + align : BLOCK_SIZE;
			char* mem = new char[block_size];
			blocks.push_back({mem, block_size});
			aligned = mem;
			end = mem + block_size;
		}

		ptr = aligned + size;
		return aligned;
	}

	template <class T, class... Args>
	T* alloc(Args&&... args)
	{
		T* p = static_cast<T*>(alloc_raw(sizeof(T), alignof(T)));
		return new (p) T{std::forward<Args>(args)...};
	}

	template <class T>
	T* alloc_array(size_t count)
	{
		T* p = static_cast<T*>(alloc_raw(sizeof(T) * count, alignof(T)));
		for (size_t i = 0; i < count; ++i)
		{
			new (&p[i]) T{};
		}
		return p;
	}

	std::string_view copy_string(std::string_view s)
	{
		char* p = static_cast<char*>(alloc_raw(s.size() + 1, 1));
		memcpy(p, s.data(), s.size());
		p[s.size()] = '\0';
		return {p, s.size()};
	}

	template <class T>
	Slice<T> copy_slice(const T* src, uint32_t count)
	{
		T* data = alloc_array<T>(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			data[i] = src[i];
		}
		return {data, count};
	}

	template <class T>
	Slice<T> copy_slice(const std::vector<T>& src)
	{
		return copy_slice(src.data(), static_cast<uint32_t>(src.size()));
	}

	template <class T>
	Slice<T> copy_slice(std::initializer_list<T> src)
	{
		return copy_slice(src.begin(), static_cast<uint32_t>(src.size()));
	}
};

struct Expr;

struct UpvalueRef
{
	Expr* owner;
	uint32_t breadth;
};

using ExprId = uint32_t;
constexpr ExprId NO_EXPR = ~uint32_t{0};

enum class ExprKind : uint8_t
{
	NumberLit,
	StringLit,
	BooleanLit,
	CharacterLit,
	SymbolLit,
	UnknownLit,
	VarRef,
	Call,
	Apply,
	Lambda,
	Define,
	PrimRef,
	SetBang,
	SetRef,
	If,
	Let,
	Letrec,
	Begin,
	When,
	Unless,
	Cond,
	And,
	Or,
};

struct Expr
{
	ExprKind kind;
	ExprId id;
	SourceLoc loc;

	union
	{
		struct
		{
			std::string_view text;
		} number_lit;
		struct
		{
			std::string_view value;
		} string_lit; // escape-processed
		struct
		{
			bool value;
		} boolean_lit;
		struct
		{
			char value;
		} character_lit;
		struct
		{
			std::string_view name;
		} symbol_lit;
		struct
		{
		} unknown_lit;
		struct
		{
			std::string_view name;
		} var_ref;
		struct
		{
			Expr* proc;
			Slice<Expr*> args;
		} call;
		struct
		{
			Expr* proc;
			Expr* args;
		} apply;
		struct
		{
			Slice<std::string_view> params;
			bool is_variadic;
			Slice<Expr*> body;
			Slice<std::string_view> names;
			Slice<bool> captured_locals;
			Slice<bool> mutated_locals;
			Slice<bool> reassigned_after_init_locals;
			Slice<UpvalueRef> upvalues;
		} lambda;
		struct
		{
			std::string_view name;
			Expr* value;
		} define;
		struct
		{
			std::string_view name;
		} prim_ref;
		struct
		{
			std::string_view name;
			Expr* value;
			bool is_init;
		} set_bang;
		struct
		{
			Expr* obj;
			Expr* key;
			Expr* value;
		} set_ref;
		struct
		{
			Expr* test;
			Expr* consequent;
			Expr* alternate;
		} if_;
		struct
		{
			Slice<std::string_view> names;
			Slice<Expr*> vals;
			Slice<Expr*> body;
			uint32_t slot_base;
			Expr* owner;
		} let;
		struct
		{
			Slice<Expr*> body;
		} begin;
		struct
		{
			Expr* test;
			Slice<Expr*> body;
		} when;
		struct
		{
			Expr* test;
			Slice<Expr*> body;
		} unless;
		// clauses are flat (test, expr) pairs.
		struct
		{
			Slice<Expr*> clauses;
		} cond;
		struct
		{
			Slice<Expr*> exprs;
		} and_;
		struct
		{
			Slice<Expr*> exprs;
		} or_;
	};

	Expr() : kind{ExprKind::UnknownLit}, id{NO_EXPR}, loc{}, unknown_lit{} {}
};

struct Program
{
	Slice<Expr*> forms;
};

inline bool is_delimiter(char c)
{
	return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == '"' ||
		   c == ';';
}

inline char decode_char_literal(std::string_view body, SourceLoc loc)
{
	if (body.size() == 1)
	{
		return body[0];
	}
	struct Named
	{
		std::string_view name;
		char value;
	};
	static constexpr Named names[] = {
		{"alarm", 0x07},     {"backspace", 0x08}, {"delete", 0x7F}, {"escape", 0x1B}, {"newline", 0x0A},
		{"null", 0x00},      {"return", 0x0D},    {"space", 0x20},  {"tab", 0x09},
	};
	for (Named n : names)
	{
		if (n.name == body)
		{
			return n.value;
		}
	}
	JET_DIE("%d:%d: unknown character name '#\\%.*s'", loc.line, loc.col,
			 static_cast<int>(body.size()), body.data());
}

inline bool is_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '!' || c == '$' || c == '%' || c == '&' ||
		   c == '*' || c == '/' || c == ':' || c == '<' || c == '=' || c == '>' || c == '?' || c == '^' ||
		   c == '_' || c == '~' || c == '-' || c == '.';
}

inline bool is_ident_cont(char c)
{
	return is_ident_start(c) || (c >= '0' && c <= '9') || c == '+' || c == '@';
}

static const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));

static double number_lit_value(std::string_view text)
{
	// NumberLit text is arena-interned by the lexer/parser (copy_string), so
	// text.data() is NUL-terminated.
	return strtod_l(text.data(), nullptr, c_locale);
}

struct ResolvedBinding
{
	Expr* lambda;
	size_t breadth;
};

template <class T>
typename std::vector<T>::reference get(std::vector<T>& v, size_t i)
{
	if (v.size() <= i)
	{
		v.resize(i + 1);
	}
	return v[i];
}

struct OrderedNameSet
{
	std::vector<std::string_view> ordered;
	std::unordered_set<std::string_view> seen;

	bool insert(std::string_view name)
	{
		if (!seen.insert(name).second)
		{
			return false;
		}
		ordered.push_back(name);
		return true;
	}
};

struct Compiler
{
	std::string source;
	std::string filename;
	std::vector<std::string> file_table;
	Arena arena;
	CompileFlags flags_;

	std::optional<std::vector<Token>> tokens_;
	std::optional<Program> ast_;
	// Counter used by both parse() and expand-time desugaring so synthesized
	// Expr nodes get unique IDs the binding/tail caches can index by.
	uint32_t next_expr_id_ = 0;
	std::vector<Token>& tokens();
	Program& ast();

	Expr* make_expr(ExprKind kind, SourceLoc loc);
	Expr* make_boolean_lit(bool value, SourceLoc loc);
	Expr* expand(Expr* expr);
	Expr* expand_let(Expr* expr);
	Expr* expand_letrec(Expr* expr);
	Expr* expand_begin(Expr* expr);
	Slice<Expr*> hoist_defines_in_body(Slice<Expr*> body, SourceLoc loc);
	Expr* rewrite_define_in(Expr* expr, OrderedNameSet& names);
	OrderedNameSet toplevel_names_;

	using AnfBindings = std::vector<std::pair<std::string_view, Expr*>>;
	Expr* compute_anf(Expr* expr);
	Expr* anf_atomize(Expr* expr, AnfBindings& bindings);
	Expr* anf_wrap(AnfBindings& bindings, Expr* body);
	std::string_view gensym();
	void verify_anf(Expr* expr);
	uint32_t gensym_counter_ = 0;

	std::vector<ResolvedBinding> bindings_;
	std::vector<bool> tail_cache_;

	// Compile-time chain of enclosing lambdas during binding resolution.
	// Index 0 is a synthesized toplevel lambda whose names are the program's
	// top-level defines, so binding lookup and codegen can treat the program
	// uniformly as one big nested lambda.
	std::vector<Expr*> lambdas_;
	std::vector<std::unordered_map<std::string_view, size_t>> lambda_name_index_;
	std::vector<std::vector<std::string_view>> frame_names_;
	Expr* toplevel_lambda_ = nullptr;

	std::vector<Expr*> all_lambdas_;
	std::vector<bool> intrinsic_callee_;

	struct LambdaBindings
	{
		std::vector<UpvalueRef> upvalues;
		std::unordered_set<uint64_t> upvalue_keys;
		std::vector<bool> captured;
		std::vector<bool> mutated;
		std::vector<bool> reassigned_after_init;
		std::vector<Expr*> bound_init;
	};
	std::unordered_map<Expr*, LambdaBindings> lambda_bindings_;

	ResolvedBinding binding(Expr* expr);
	bool is_tail(Expr* expr);
	bool is_self_tail_call(Expr* call, Expr* current);
	bool is_intrinsic_callee(Expr* call, Expr* current);
	void collect_intrinsic_callees(Expr* expr, Expr* current);
	std::optional<ResolvedBinding> lookup_name(std::string_view name);
	void push_lambda_scope(Expr* lambda);
	void pop_lambda_scope();
	bool prim_binding_lowerable(ResolvedBinding b, std::string_view prim);
	void record_ref(ResolvedBinding b);
	void record_set(ResolvedBinding b, bool is_init, Expr* value);
	void resolve_bindings(Program& program);
	void run_optimization_passes(Program& program);
	void compute_binding_addresses(Program& program);
	void compute_binding_addresses_in(Expr* expr);
	void recompute_lambda_bindings(Program& program);
	void recompute_lambda_bindings_in(Expr* expr);
	void freeze_lambda(Expr* lambda);
	void collect_tail_calls(Program& program);
	void collect_tail_calls(Expr* expr, bool in_tail);

	struct OpSelection
	{
		Opcode op;
		union
		{
			struct { uint16_t addr; } var;     // register / upvalue idx of ref/set
			struct { uint16_t upvalue_idx; } call_ic_slot;
			struct { uint8_t src; uint16_t idx; } call_ic_direct;
		} u;
	};
	std::vector<std::optional<OpSelection>> selected_ops_;
	void run_op_selection(Program& program);
	void select_ops_in(Expr* expr, Expr* current);
	void select_call_op(Expr* expr, Expr* current);
	void select_if_cmp(Expr* expr);
	void select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, bool is_set);
	void select_var_op(Expr* expr, Expr* current, bool is_set);

	void run_anf_inline(Program& program);
	void run_binarize_arith(Program& program);
	void run_lambda_lift(Program& program);

	Bytecode compile();
};

static std::vector<Token> lex(IPort* port, Arena& arena, uint32_t file_id);

namespace
{

	struct LexState
	{
		IPort* port;
		Arena& arena;
		uint32_t file_id = 0;
		int line = 1;
		int col = 1;
		bool in_quote = false;
		bool read_mode = false;
		std::vector<Token> tokens{};
		Token pending_{};

		LexState(IPort* p, Arena& a, uint32_t fid) : port{p}, arena{a}, file_id{fid} {}

		TokenKind classify_token_text(std::string_view text)
		{
			return (in_quote || read_mode) ? TokenKind::Variable : classify_ident(text);
		}

		bool at_end() { return port->eof(); }
		char peek() { return at_end() ? '\0' : port->peek_byte(); }

		char advance()
		{
			char c = port->read_byte();
			if (c == '\n')
			{
				++line;
				col = 1;
			}
			else
			{
				++col;
			}
			return c;
		}

		SourceLoc loc() { return {file_id, line, col}; }

		void skip_whitespace_and_comments()
		{
			while (!at_end())
			{
				char c = peek();
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				{
					advance();
				}
				else if (c == ';')
				{
					while (!at_end() && peek() != '\n')
					{
						advance();
					}
				}
				else
				{
					break;
				}
			}
		}

		void emit(TokenKind kind, std::string_view text, SourceLoc l) { pending_ = {kind, text, l}; }

		std::string_view intern(const std::string& buf) { return arena.copy_string(buf); }

		void lex_string()
		{
			SourceLoc start = loc();
			std::string buf;
			buf += advance();
			while (!at_end() && peek() != '"')
			{
				if (peek() == '\\')
				{
					buf += advance();
				}
				if (!at_end())
				{
					buf += advance();
				}
			}
			if (!at_end())
			{
				buf += advance();
			}
			emit(TokenKind::String, intern(buf), start);
		}

		void lex_hash()
		{
			SourceLoc start = loc();
			std::string buf;
			buf += advance();

			if (at_end())
			{
				emit(TokenKind::Hash, intern(buf), start);
				return;
			}

			switch (peek())
			{
				case 't':
				case 'f':
					buf += advance();
					if (is_delimiter(peek()))
					{
						emit(TokenKind::Boolean, intern(buf), start);
					}
					else
					{
						JET_DIE("%d:%d: invalid boolean", line, col);
					}
					break;

				case '\\':
					buf += advance();
					if (!at_end())
					{
						char first = advance();
						buf += first;
						if (std::isalpha(static_cast<unsigned char>(first)))
						{
							while (std::isalpha(static_cast<unsigned char>(peek())))
							{
								buf += advance();
							}
						}
					}
					emit(TokenKind::Character, intern(buf), start);
					break;

				case '(':
					// Leave the '(' in the stream; it lexes as LParen next.
					emit(TokenKind::Hash, intern(buf), start);
					break;

				default:
					JET_DIE("%d:%d: invalid # syntax", line, col);
			}
		}

		void finish_ident(std::string& buf, SourceLoc start)
		{
			while (!at_end() && is_ident_cont(peek()))
			{
				buf += advance();
			}
			std::string_view text = intern(buf);
			emit(classify_token_text(text), text, start);
		}

		void finish_number(std::string& buf, SourceLoc start)
		{
			// peek() is on the first digit; buf may already hold a sign.
			char first = advance();
			buf += first;

			if (first == '0' && !at_end() && peek() == 'x')
			{
				buf += advance();
				while (!at_end() && ((peek() >= '0' && peek() <= '9') ||
									 (peek() >= 'a' && peek() <= 'f') ||
									 (peek() >= 'A' && peek() <= 'F')))
				{
					buf += advance();
				}
				emit(TokenKind::Number, intern(buf), start);
				return;
			}

			while (!at_end() && peek() >= '0' && peek() <= '9')
			{
				buf += advance();
			}
			if (!at_end() && peek() == '.')
			{
				buf += advance();
				while (!at_end() && peek() >= '0' && peek() <= '9')
				{
					buf += advance();
				}
			}
			if (!at_end() && (peek() == 'e' || peek() == 'E'))
			{
				buf += advance();
				if (!at_end() && (peek() == '+' || peek() == '-'))
				{
					buf += advance();
				}
				while (!at_end() && peek() >= '0' && peek() <= '9')
				{
					buf += advance();
				}
			}
			emit(TokenKind::Number, intern(buf), start);
		}

		void lex_number_or_ident()
		{
			SourceLoc start = loc();
			std::string buf;
			char c = peek();

			if (c == '+' || c == '-')
			{
				buf += advance();
				if (at_end() || is_delimiter(peek()))
				{
					std::string_view text = intern(buf);
					emit(classify_token_text(text), text, start);
					return;
				}
				if (peek() >= '0' && peek() <= '9')
				{
					finish_number(buf, start);
					return;
				}
				finish_ident(buf, start);
				return;
			}

			if (c >= '0' && c <= '9')
			{
				finish_number(buf, start);
				return;
			}

			if (is_ident_start(c))
			{
				finish_ident(buf, start);
				return;
			}

			JET_DIE("%d:%d: unexpected character '%c'", line, col, c);
		}

		TokenKind classify_ident(std::string_view text)
		{
			struct Keyword
			{
				std::string_view name;
				TokenKind kind;
			};
			static constexpr Keyword keywords[] = {
				{"lambda", TokenKind::Lambda},  {"define", TokenKind::Define}, {"if", TokenKind::If},
				{"set!", TokenKind::Set},       {"setf!", TokenKind::Setf},    {"quote", TokenKind::QuoteWord},
				{"apply", TokenKind::Apply},    {"let", TokenKind::Let},       {"let*", TokenKind::LetStar},
				{"letrec", TokenKind::Letrec},  {"letrec*", TokenKind::Letrec}, {"begin", TokenKind::Begin},
				{"when", TokenKind::When},      {"unless", TokenKind::Unless}, {"cond", TokenKind::Cond},
				{"and", TokenKind::And},        {"or", TokenKind::Or},         {"include", TokenKind::Include},
			};
			for (Keyword k : keywords)
			{
				if (k.name == text)
				{
					return k.kind;
				}
			}
			return TokenKind::Variable;
		}

		Token next_token()
		{
			skip_whitespace_and_comments();
			if (at_end())
			{
				return {TokenKind::Eof, {}, loc()};
			}

			char c = peek();
			SourceLoc l = loc();

			switch (c)
			{
				case '(':
					advance();
					emit(TokenKind::LParen, intern(std::string{c}), l);
					break;

				case ')':
					advance();
					in_quote = false;
					emit(TokenKind::RParen, intern(std::string{c}), l);
					break;

				case '\'':
					advance();
					in_quote = true;
					emit(TokenKind::Quote, intern(std::string{c}), l);
					break;

				case '"':
					lex_string();
					break;

				case '#':
					lex_hash();
					break;

				case '.':
					advance();
					if (at_end() || is_delimiter(peek()))
					{
						emit(TokenKind::Dot, intern(std::string{c}), l);
					}
					else
					{
						std::string buf{c};
						finish_ident(buf, l);
					}
					break;

				default:
					lex_number_or_ident();
					// Quote mode wraps a single Variable token; for lists, in_quote exits via ')'.
					if (in_quote && pending_.kind == TokenKind::Variable)
					{
						in_quote = false;
					}
					break;
			}

			return pending_;
		}

		void lex_all()
		{
			while (true)
			{
				Token t = next_token();
				tokens.push_back(t);
				if (t.kind == TokenKind::Eof)
				{
					break;
				}
			}
		}
	};

	struct ParseState
	{
		std::span<Token> tokens;
		size_t pos = 0;
		LexState* stream_lex = nullptr;
		Token la_{};
		bool la_valid_ = false;
		Token current_{};

		Arena& arena;
		std::vector<std::string>& file_table;
		uint32_t& next_id;
		std::vector<Expr*> scratch{};

		Token& peek()
		{
			if (stream_lex)
			{
				if (!la_valid_)
				{
					la_ = stream_lex->next_token();
					la_valid_ = true;
				}
				return la_;
			}
			return tokens[pos];
		}

		bool at_end() { return peek().kind == TokenKind::Eof; }

		Token& advance()
		{
			if (stream_lex)
			{
				peek();
				current_ = la_;
				la_valid_ = false;
				return current_;
			}
			Token& t = tokens[pos];
			if (t.kind != TokenKind::Eof)
			{
				++pos;
			}
			return t;
		}

		void expect(TokenKind kind)
		{
			if (peek().kind != kind)
			{
				JET_DIE("%d:%d: expected token %d, got %d", peek().loc.line, peek().loc.col,
						 static_cast<int>(kind), static_cast<int>(peek().kind));
			}
			advance();
		}

		std::string_view expect_identifier(const char* what)
		{
			JET_DIE_UNLESS(peek().kind == TokenKind::Variable,
							"%d:%d: expected identifier for %s", peek().loc.line, peek().loc.col, what);
			return advance().text;
		}

		Expr* make_expr(ExprKind kind, SourceLoc loc)
		{
			Expr* e = arena.alloc<Expr>();
			e->kind = kind;
			e->id = next_id++;
			e->loc = loc;
			return e;
		}

		Slice<Expr*> make_slice(std::vector<Expr*>& vec) { return arena.copy_slice(vec); }

		Slice<std::string_view> make_string_slice(std::vector<std::string_view>& vec)
		{
			return arena.copy_slice(vec);
		}

		std::string_view process_string_escapes(std::string_view raw)
		{
			// `raw` is the lexer slice including surrounding quotes.
			std::string_view inner = raw.substr(1, raw.size() - 2);

			if (inner.find('\\') == std::string_view::npos)
			{
				return inner;
			}

			std::string result;
			result.reserve(inner.size());

			for (size_t i = 0; i < inner.size(); ++i)
			{
				if (inner[i] == '\\' && i + 1 < inner.size())
				{
					++i;
					switch (inner[i])
					{
						case '\\':
							result += '\\';
							break;
						case '"':
							result += '"';
							break;
						case 'n':
							result += '\n';
							break;
						case 't':
							result += '\t';
							break;
						default:
							result += inner[i];
							break;
					}
				}
				else
				{
					result += inner[i];
				}
			}

			return arena.copy_string(result);
		}

		Expr* parse_expr()
		{
			Token& tok = peek();

			switch (tok.kind)
			{
				case TokenKind::Number:
				{
					Expr* e = make_expr(ExprKind::NumberLit, tok.loc);
					e->number_lit.text = advance().text;
					return e;
				}
				case TokenKind::String:
				{
					Expr* e = make_expr(ExprKind::StringLit, tok.loc);
					e->string_lit.value = process_string_escapes(advance().text);
					return e;
				}
				case TokenKind::Boolean:
				{
					Expr* e = make_expr(ExprKind::BooleanLit, tok.loc);
					e->boolean_lit.value = (advance().text.back() == 't');
					return e;
				}
				case TokenKind::Character:
				{
					Expr* e = make_expr(ExprKind::CharacterLit, tok.loc);
					std::string text = std::string{advance().text};
					std::string_view body{text.data() + 2, text.size() - 2};
					e->character_lit.value = decode_char_literal(body, tok.loc);
					return e;
				}
				case TokenKind::Variable:
				{
					Expr* e = make_expr(ExprKind::VarRef, tok.loc);
					e->var_ref.name = advance().text;
					return e;
				}
				case TokenKind::Quote:
				{
					advance();
					return parse_datum();
				}
				case TokenKind::Hash:
					return parse_quoted_vector();
				case TokenKind::LParen:
					return parse_paren_form();
				default:
					JET_DIE("%d:%d: unexpected token", tok.loc.line, tok.loc.col);
			}
		}

		Expr* parse_paren_form()
		{
			SourceLoc loc = peek().loc;
			expect(TokenKind::LParen);
			Token& head = peek();

			if (head.kind == TokenKind::Variable)
			{
				if (head.text == "%prim")
				{
					return parse_prim(loc);
				}
				if (head.text == "$file")
				{
					return parse_dollar_file(loc);
				}
				if (head.text == "$line")
				{
					return parse_dollar_loc_field(loc, loc.line);
				}
				if (head.text == "$col")
				{
					return parse_dollar_loc_field(loc, loc.col);
				}
				if (head.text == "$check")
				{
					return parse_dollar_check(loc);
				}
			}

			switch (head.kind)
			{
				case TokenKind::Lambda:
					return parse_lambda(loc);
				case TokenKind::Define:
					return parse_define(loc);
				case TokenKind::If:
					return parse_if(loc);
				case TokenKind::Set:
					return parse_set_bang(loc);
				case TokenKind::Setf:
					return parse_setf_bang(loc);
				case TokenKind::Apply:
					return parse_apply(loc);
				case TokenKind::QuoteWord:
					return parse_quote_form(loc);
				case TokenKind::Let:
					return parse_let_form(loc, ExprKind::Let);
				case TokenKind::LetStar:
					return parse_let_star(loc);
				case TokenKind::Letrec:
					return parse_let_form(loc, ExprKind::Letrec);
				case TokenKind::Begin:
					return parse_begin(loc);
				case TokenKind::When:
					return parse_when(loc);
				case TokenKind::Unless:
					return parse_unless(loc);
				case TokenKind::Cond:
					return parse_cond(loc);
				case TokenKind::And:
					return parse_and(loc);
				case TokenKind::Or:
					return parse_or(loc);
				case TokenKind::Include:
					return parse_include(loc);
				default:
					return parse_call(loc);
			}
		}

		Expr* parse_lambda(SourceLoc loc)
		{
			advance();

			std::vector<std::string_view> params;
			bool is_variadic = false;

			if (peek().kind == TokenKind::LParen)
			{
				advance();
				while (peek().kind == TokenKind::Variable)
				{
					params.push_back(advance().text);
				}
				expect(TokenKind::RParen);
			}
			else if (peek().kind == TokenKind::Variable)
			{
				params.push_back(advance().text);
				is_variadic = true;
			}
			else
			{
				JET_DIE("%d:%d: expected formals", loc.line, loc.col);
			}

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Lambda, loc);
			e->lambda.params = make_string_slice(params);
			e->lambda.is_variadic = is_variadic;
			e->lambda.body = make_slice(body);
			return e;
		}

		Expr* parse_define(SourceLoc loc)
		{
			advance();

			if (peek().kind == TokenKind::LParen)
			{
				// Shorthand: (define (f args...) body...) → (define f (lambda (args...) body...)).
				advance();
				std::string_view name = expect_identifier("define");

				std::vector<std::string_view> params;
				while (peek().kind == TokenKind::Variable)
				{
					params.push_back(advance().text);
				}
				expect(TokenKind::RParen);

				std::vector<Expr*> body;
				while (peek().kind != TokenKind::RParen)
				{
					body.push_back(parse_expr());
				}
				expect(TokenKind::RParen);

				Expr* lam = make_expr(ExprKind::Lambda, loc);
				lam->lambda.params = make_string_slice(params);
				lam->lambda.is_variadic = false;
				lam->lambda.body = make_slice(body);

				Expr* e = make_expr(ExprKind::Define, loc);
				e->define.name = name;
				e->define.value = lam;
				return e;
			}

			std::string_view name = expect_identifier("define");
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Define, loc);
			e->define.name = name;
			e->define.value = value;
			return e;
		}

		Expr* parse_prim(SourceLoc loc)
		{
			advance();
			if (peek().kind != TokenKind::String)
			{
				JET_DIE("%d:%d: %%prim expects a string literal", loc.line, loc.col);
			}
			std::string_view name = process_string_escapes(advance().text);
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::PrimRef, loc);
			e->prim_ref.name = name;
			return e;
		}

		Expr* parse_if(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			Expr* consequent = parse_expr();
			Expr* alternate = nullptr;
			if (peek().kind != TokenKind::RParen)
			{
				alternate = parse_expr();
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::If, loc);
			e->if_.test = test;
			e->if_.consequent = consequent;
			e->if_.alternate = alternate;
			return e;
		}

		Expr* parse_set_bang(SourceLoc loc)
		{
			advance();

			std::string_view name = expect_identifier("set!");
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::SetBang, loc);
			e->set_bang.name = name;
			e->set_bang.value = value;
			e->set_bang.is_init = false;
			return e;
		}

		Expr* parse_setf_bang(SourceLoc loc)
		{
			advance();

			SourceLoc place_loc = peek().loc;
			expect(TokenKind::LParen);
			if (peek().kind != TokenKind::Variable || peek().text != "ref")
			{
				JET_DIE("%d:%d: setf! place form must be (ref obj key)", place_loc.line, place_loc.col);
			}
			advance();
			Expr* obj = parse_expr();
			Expr* key = parse_expr();
			expect(TokenKind::RParen);
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::SetRef, loc);
			e->set_ref.obj = obj;
			e->set_ref.key = key;
			e->set_ref.value = value;
			return e;
		}

		Expr* parse_apply(SourceLoc loc)
		{
			advance();
			Expr* proc = parse_expr();
			Expr* args = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Apply, loc);
			e->apply.proc = proc;
			e->apply.args = args;
			return e;
		}

		Expr* parse_quote_form(SourceLoc loc)
		{
			advance();
			Expr* datum = parse_datum();
			expect(TokenKind::RParen);
			return datum;
		}

		Expr* parse_call(SourceLoc loc)
		{
			Expr* proc = parse_expr();
			std::vector<Expr*> args;
			while (peek().kind != TokenKind::RParen)
			{
				args.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Call, loc);
			e->call.proc = proc;
			e->call.args = make_slice(args);
			return e;
		}

		Expr* parse_let_form(SourceLoc loc, ExprKind kind)
		{
			advance();

			std::string_view loop_name;
			if (kind == ExprKind::Let && peek().kind == TokenKind::Variable)
			{
				loop_name = advance().text;
			}

			expect(TokenKind::LParen);

			std::vector<std::string_view> names;
			std::vector<Expr*> vals;
			while (peek().kind != TokenKind::RParen)
			{
				expect(TokenKind::LParen);
				names.push_back(expect_identifier("let binding"));
				vals.push_back(parse_expr());
				expect(TokenKind::RParen);
			}
			expect(TokenKind::RParen);

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			if (loop_name.empty())
			{
				Expr* e = make_expr(kind, loc);
				e->let.names = make_string_slice(names);
				e->let.vals = make_slice(vals);
				e->let.body = make_slice(body);
				return e;
			}

			// Named let: (let f ((x v) ...) body...)
			//   ==>  (letrec ((f (lambda (x ...) body...))) (f v ...))
			Expr* lam = make_expr(ExprKind::Lambda, loc);
			lam->lambda.params = make_string_slice(names);
			lam->lambda.is_variadic = false;
			lam->lambda.body = make_slice(body);

			Expr* var = make_expr(ExprKind::VarRef, loc);
			var->var_ref.name = loop_name;

			Expr* call = make_expr(ExprKind::Call, loc);
			call->call.proc = var;
			call->call.args = make_slice(vals);

			std::vector<std::string_view> rec_names{loop_name};
			std::vector<Expr*> rec_vals{lam};
			std::vector<Expr*> rec_body{call};

			Expr* e = make_expr(ExprKind::Letrec, loc);
			e->let.names = make_string_slice(rec_names);
			e->let.vals = make_slice(rec_vals);
			e->let.body = make_slice(rec_body);
			return e;
		}

		Expr* parse_let_star(SourceLoc loc)
		{
			// (let* ((a v1) (b v2) ...) body...) →
			//   (let ((a v1)) (let ((b v2)) ... body ...))
			advance();
			expect(TokenKind::LParen);

			std::vector<std::string_view> names;
			std::vector<Expr*> vals;
			while (peek().kind != TokenKind::RParen)
			{
				expect(TokenKind::LParen);
				names.push_back(expect_identifier("let* binding"));
				vals.push_back(parse_expr());
				expect(TokenKind::RParen);
			}
			expect(TokenKind::RParen);

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			if (names.empty())
			{
				Expr* e = make_expr(ExprKind::Let, loc);
				e->let.names = make_string_slice(names);
				e->let.vals = make_slice(vals);
				e->let.body = make_slice(body);
				return e;
			}

			Expr* inner = nullptr;
			for (size_t i = names.size(); i-- > 0;)
			{
				Expr* let_expr = make_expr(ExprKind::Let, loc);
				std::vector<std::string_view> n{names[i]};
				std::vector<Expr*> v{vals[i]};
				let_expr->let.names = make_string_slice(n);
				let_expr->let.vals = make_slice(v);
				if (inner == nullptr)
				{
					let_expr->let.body = make_slice(body);
				}
				else
				{
					std::vector<Expr*> b{inner};
					let_expr->let.body = make_slice(b);
				}
				inner = let_expr;
			}
			return inner;
		}

		Expr* parse_begin(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Begin, loc);
			e->begin.body = make_slice(body);
			return e;
		}

		Expr* parse_when(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::When, loc);
			e->when.test = test;
			e->when.body = make_slice(body);
			return e;
		}

		Expr* parse_unless(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Unless, loc);
			e->unless.test = test;
			e->unless.body = make_slice(body);
			return e;
		}

		Expr* parse_cond(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> clauses;
			while (peek().kind != TokenKind::RParen)
			{
				SourceLoc clause_loc = peek().loc;
				expect(TokenKind::LParen);
				clauses.push_back(parse_expr());
				std::vector<Expr*> body;
				while (peek().kind != TokenKind::RParen)
				{
					body.push_back(parse_expr());
				}
				expect(TokenKind::RParen);
				if (body.size() == 1)
				{
					clauses.push_back(body[0]);
				}
				else
				{
					Expr* begin_e = make_expr(ExprKind::Begin, clause_loc);
					begin_e->begin.body = make_slice(body);
					clauses.push_back(begin_e);
				}
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Cond, loc);
			e->cond.clauses = make_slice(clauses);
			return e;
		}

		Expr* parse_and(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> exprs;
			while (peek().kind != TokenKind::RParen)
			{
				exprs.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::And, loc);
			e->and_.exprs = make_slice(exprs);
			return e;
		}

		Expr* parse_or(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> exprs;
			while (peek().kind != TokenKind::RParen)
			{
				exprs.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(ExprKind::Or, loc);
			e->or_.exprs = make_slice(exprs);
			return e;
		}

		Expr* parse_include(SourceLoc loc)
		{
			advance();
			if (peek().kind != TokenKind::String)
			{
				JET_DIE("%d:%d: include expects a string path", loc.line, loc.col);
			}
			std::string_view raw_path = advance().text;
			expect(TokenKind::RParen);

			std::string_view raw_inc_path = process_string_escapes(raw_path);

			std::string path{raw_inc_path};
			if (path[0] != '/')
			{
				std::string& parent = file_table[loc.file_id];
				size_t slash = parent.rfind('/');
				if (slash != std::string::npos)
				{
					path = parent.substr(0, slash + 1) + path;
				}
			}

			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
			{
				JET_DIE("%d:%d: cannot open '%.*s'", loc.line, loc.col,
						 static_cast<int>(path.size()), path.data());
			}
			std::string source;
			char read_buf[4096];
			size_t n;
			while ((n = fread(read_buf, 1, sizeof(read_buf), f)) > 0)
			{
				source.append(read_buf, n);
			}
			fclose(f);

			uint32_t file_id = static_cast<uint32_t>(file_table.size());
			file_table.push_back(std::string{path});
			char* source_copy = static_cast<char*>(arena.alloc_raw(source.size() + 1, 1));
			memcpy(source_copy, source.data(), source.size());
			source_copy[source.size()] = '\0';
			std::string_view source_view{source_copy, source.size()};

			IPortMem inc_port{source_view};
			std::vector<Token> inc_tokens = lex(&inc_port, arena, file_id);

			ParseState inc_state{
				.tokens = inc_tokens, .arena = arena, .file_table = file_table, .next_id = next_id};
			std::vector<Expr*> forms;
			while (!inc_state.at_end())
			{
				forms.push_back(inc_state.parse_expr());
			}
			next_id = inc_state.next_id;

			Expr* e = make_expr(ExprKind::Begin, loc);
			e->begin.body = make_slice(forms);
			return e;
		}

		Expr* parse_dollar_file(SourceLoc loc)
		{
			advance();
			expect(TokenKind::RParen);
			Expr* e = make_expr(ExprKind::StringLit, loc);
			e->string_lit.value = arena.copy_string(file_table[loc.file_id]);
			return e;
		}

		Expr* make_number_lit(int value, SourceLoc loc)
		{
			Expr* e = make_expr(ExprKind::NumberLit, loc);
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", value);
			e->number_lit.text = arena.copy_string(buf);
			return e;
		}

		Expr* parse_dollar_loc_field(SourceLoc loc, int value)
		{
			advance();
			expect(TokenKind::RParen);
			return make_number_lit(value, loc);
		}

		Expr* parse_dollar_check(SourceLoc loc)
		{
			// ($check expr) → (%check expr "file" line col).
			advance();
			Expr* test = parse_expr();
			expect(TokenKind::RParen);

			Expr* check_ref = make_expr(ExprKind::VarRef, loc);
			check_ref->var_ref.name = arena.copy_string("%check");

			Expr* file_lit = make_expr(ExprKind::StringLit, loc);
			file_lit->string_lit.value = arena.copy_string(file_table[loc.file_id]);

			std::vector<Expr*> args{test, file_lit, make_number_lit(loc.line, loc),
									make_number_lit(loc.col, loc)};

			Expr* e = make_expr(ExprKind::Call, loc);
			e->call.proc = check_ref;
			e->call.args = make_slice(args);
			return e;
		}

		Expr* parse_datum()
		{
			Token& tok = peek();

			switch (tok.kind)
			{
				case TokenKind::Number:
				case TokenKind::String:
				case TokenKind::Boolean:
				case TokenKind::Character:
					return parse_expr();

				case TokenKind::Variable:
				{
					Expr* e = make_expr(ExprKind::SymbolLit, tok.loc);
					e->symbol_lit.name = advance().text;
					return e;
				}

				case TokenKind::Quote:
				{
					SourceLoc qloc = peek().loc;
					advance();
					Expr* inner = parse_datum();

					Expr* list_ref = make_expr(ExprKind::VarRef, qloc);
					list_ref->var_ref.name = arena.copy_string("list");

					Expr* quote_sym = make_expr(ExprKind::SymbolLit, qloc);
					quote_sym->symbol_lit.name = arena.copy_string("quote");

					std::vector<Expr*> args{quote_sym, inner};
					Expr* call = make_expr(ExprKind::Call, qloc);
					call->call.proc = list_ref;
					call->call.args = make_slice(args);
					return call;
				}

				case TokenKind::LParen:
					return parse_quoted_list();

				case TokenKind::Hash:
					return parse_quoted_vector();

				default:
					JET_DIE("%d:%d: unexpected token in datum", tok.loc.line, tok.loc.col);
			}
		}

		Expr* parse_quoted_list()
		{
			// '(a b c) → Call{VarRef{"list"}, [a, b, c]}.
			SourceLoc loc = peek().loc;
			expect(TokenKind::LParen);

			std::vector<Expr*> elems;
			Expr* tail = nullptr;
			while (peek().kind != TokenKind::RParen)
			{
				if (peek().kind == TokenKind::Dot)
				{
					advance();
					JET_DIE_UNLESS(!elems.empty(), "%d:%d: dotted pair needs a head", peek().loc.line,
								peek().loc.col);
					tail = parse_datum();
					JET_DIE_UNLESS(peek().kind == TokenKind::RParen,
									"%d:%d: extra tokens after dot in dotted pair", peek().loc.line,
								peek().loc.col);
					break;
				}
				elems.push_back(parse_datum());
			}
			expect(TokenKind::RParen);

			if (tail)
			{
				Expr* result = tail;
				for (size_t i = elems.size(); i-- > 0;)
				{
					Expr* cons_ref = make_expr(ExprKind::VarRef, loc);
					cons_ref->var_ref.name = arena.copy_string("cons");
					std::vector<Expr*> args{elems[i], result};
					Expr* call = make_expr(ExprKind::Call, loc);
					call->call.proc = cons_ref;
					call->call.args = make_slice(args);
					result = call;
				}
				return result;
			}

			Expr* list_ref = make_expr(ExprKind::VarRef, loc);
			list_ref->var_ref.name = arena.copy_string("list");

			Expr* e = make_expr(ExprKind::Call, loc);
			e->call.proc = list_ref;
			e->call.args = make_slice(elems);
			return e;
		}

		Expr* parse_quoted_vector()
		{
			// #(a b c) → Call{VarRef{"vector"}, [a, b, c]}.
			SourceLoc loc = peek().loc;
			advance();
			expect(TokenKind::LParen);

			std::vector<Expr*> elems;
			while (peek().kind != TokenKind::RParen)
			{
				elems.push_back(parse_datum());
			}
			expect(TokenKind::RParen);

			Expr* vec_ref = make_expr(ExprKind::VarRef, loc);
			vec_ref->var_ref.name = arena.copy_string("vector");

			Expr* e = make_expr(ExprKind::Call, loc);
			e->call.proc = vec_ref;
			e->call.args = make_slice(elems);
			return e;
		}
	};

} // namespace

static std::vector<Token> lex(IPort* port, Arena& arena, uint32_t file_id)
{
	LexState state{port, arena, file_id};
	state.lex_all();
	return std::move(state.tokens);
}

static Program parse(std::span<Token> tokens, Arena& arena, std::vector<std::string>& file_table,
					 uint32_t& next_id)
{
	ParseState state{.tokens = tokens, .arena = arena, .file_table = file_table, .next_id = next_id};

	std::vector<Expr*> forms;
	while (!state.at_end())
	{
		forms.push_back(state.parse_expr());
	}

	return {state.make_slice(forms)};
}

std::vector<Token>& Compiler::tokens()
{
	if (!tokens_)
	{
		IPortMem port{source};
		tokens_ = lex(&port, arena, 0);
	}
	return *tokens_;
}

Program& Compiler::ast()
{
	if (!ast_)
	{
		if (file_table.empty())
		{
			file_table.push_back(filename);
		}
		std::vector<Token>& toks = tokens();
		ast_ = parse(toks, arena, file_table, next_expr_id_);
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = expand(ast_->forms[i]);
		}
		// The loader pre-allocates one slot per collected toplevel name from the
		// bytecode header before any code runs.
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = rewrite_define_in(ast_->forms[i], toplevel_names_);
		}
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = compute_anf(ast_->forms[i]);
			verify_anf(ast_->forms[i]);
		}
		toplevel_lambda_ = make_expr(ExprKind::Lambda, {});
		toplevel_lambda_->lambda.params = {};
		toplevel_lambda_->lambda.is_variadic = false;
		toplevel_lambda_->lambda.body = {};
		toplevel_lambda_->lambda.names = arena.copy_slice(toplevel_names_.ordered);

		resolve_bindings(*ast_);
		run_optimization_passes(*ast_);
		collect_tail_calls(*ast_);
		run_op_selection(*ast_);
	}
	return *ast_;
}

ResolvedBinding Compiler::binding(Expr* expr)
{
	ast();

	if (bindings_[expr->id].lambda != nullptr)
	{
		return bindings_[expr->id];
	}

	JET_DIE("%d:%d: unresolved binding", expr->loc.line, expr->loc.col);
}

bool Compiler::is_tail(Expr* expr)
{
	ast();
	return tail_cache_[expr->id];
}

Expr* Compiler::make_expr(ExprKind kind, SourceLoc loc)
{
	Expr* e = arena.alloc<Expr>();
	e->kind = kind;
	e->id = next_expr_id_++;
	e->loc = loc;
	return e;
}

Expr* Compiler::make_boolean_lit(bool value, SourceLoc loc)
{
	Expr* e = make_expr(ExprKind::BooleanLit, loc);
	e->boolean_lit.value = value;
	return e;
}

template <typename F>
static void walk_children(Expr* expr, F&& f)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			break;

		case ExprKind::Call:
			f(expr->call.proc);
			for (Expr*& arg : expr->call.args)
			{
				f(arg);
			}
			break;

		case ExprKind::Apply:
			f(expr->apply.proc);
			f(expr->apply.args);
			break;

		case ExprKind::Lambda:
			for (Expr*& form : expr->lambda.body)
			{
				f(form);
			}
			break;

		case ExprKind::Let:
			for (Expr*& val : expr->let.vals)
			{
				f(val);
			}
			for (Expr*& form : expr->let.body)
			{
				f(form);
			}
			break;

		case ExprKind::SetBang:
			f(expr->set_bang.value);
			break;

		case ExprKind::SetRef:
			f(expr->set_ref.obj);
			f(expr->set_ref.key);
			f(expr->set_ref.value);
			break;

		case ExprKind::If:
			f(expr->if_.test);
			f(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				f(expr->if_.alternate);
			}
			break;

		default:
			JET_DIE("%d:%d: walk_children: unhandled ExprKind %d (not ANF?)",
					 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
	}
}

Expr* Compiler::expand(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::Let:
			return expand_let(expr);

		case ExprKind::Letrec:
			return expand_letrec(expr);

		case ExprKind::Begin:
			return expand_begin(expr);

		case ExprKind::When:
		case ExprKind::Unless:
		{
			// (when test body...) -> (if test (begin body...))
			// (unless test body...) -> (if test <void> (begin body...))
			bool is_when = expr->kind == ExprKind::When;
			Expr* test = expand(is_when ? expr->when.test : expr->unless.test);
			Expr* begin_e = make_expr(ExprKind::Begin, expr->loc);
			begin_e->begin.body = is_when ? expr->when.body : expr->unless.body;
			begin_e = expand_begin(begin_e);

			Expr* if_e = make_expr(ExprKind::If, expr->loc);
			if_e->if_.test = test;
			if_e->if_.consequent = is_when ? begin_e : make_expr(ExprKind::UnknownLit, expr->loc);
			if_e->if_.alternate = is_when ? nullptr : begin_e;
			return if_e;
		}

		case ExprKind::And:
		{
			Slice<Expr*>& exprs = expr->and_.exprs;
			if (exprs.empty())
			{
				return make_boolean_lit(true, expr->loc);
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				Expr* if_e = make_expr(ExprKind::If, exprs[i]->loc);
				if_e->if_.test = expand(exprs[i]);
				if_e->if_.consequent = result;
				if_e->if_.alternate = make_boolean_lit(false, expr->loc);
				result = if_e;
			}
			return result;
		}

		case ExprKind::Or:
		{
			Slice<Expr*>& exprs = expr->or_.exprs;
			if (exprs.empty())
			{
				return make_boolean_lit(false, expr->loc);
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			// Build right-to-left: (let ((t expr[i])) (if t t rest))
			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				std::string_view tmp_name = gensym();
				Expr* ref1 = make_expr(ExprKind::VarRef, expr->loc);
				ref1->var_ref.name = tmp_name;
				Expr* ref2 = make_expr(ExprKind::VarRef, expr->loc);
				ref2->var_ref.name = tmp_name;

				Expr* if_e = make_expr(ExprKind::If, exprs[i]->loc);
				if_e->if_.test = ref1;
				if_e->if_.consequent = ref2;
				if_e->if_.alternate = result;

				Expr* let_e = make_expr(ExprKind::Let, expr->loc);
				let_e->let.names = arena.copy_slice({tmp_name});
				let_e->let.vals = arena.copy_slice({expand(exprs[i])});
				let_e->let.body = arena.copy_slice({if_e});
				result = let_e;
			}
			return result;
		}

		case ExprKind::Cond:
		{
			Slice<Expr*>& clauses = expr->cond.clauses;
			if (clauses.empty())
			{
				return make_expr(ExprKind::UnknownLit, expr->loc);
			}
			Expr* result = nullptr;
			for (int i = clauses.size() - 2; i >= 0; i -= 2)
			{
				Expr* test = expand(clauses[i]);
				Expr* body = expand(clauses[i + 1]);
				if (test->kind == ExprKind::VarRef && test->var_ref.name == "else")
				{
					result = body;
					continue;
				}
				Expr* if_e = make_expr(ExprKind::If, test->loc);
				if_e->if_.test = test;
				if_e->if_.consequent = body;
				if_e->if_.alternate = result;
				result = if_e;
			}
			return result ? result : make_expr(ExprKind::UnknownLit, expr->loc);
		}

		case ExprKind::Call:
		case ExprKind::Apply:
		case ExprKind::SetBang:
		case ExprKind::SetRef:
		case ExprKind::If:
			walk_children(expr, [&](Expr*& c) { c = expand(c); });
			return expr;

		case ExprKind::Lambda:
			walk_children(expr, [&](Expr*& c) { c = expand(c); });
			expr->lambda.body = hoist_defines_in_body(expr->lambda.body, expr->loc);
			return expr;

		case ExprKind::Define:
			expr->define.value = expand(expr->define.value);
			return expr;
		default:
			return expr;
	}
}

Expr* Compiler::expand_let(Expr* expr)
{
	walk_children(expr, [&](Expr*& c) { c = expand(c); });
	expr->let.body = hoist_defines_in_body(expr->let.body, expr->loc);
	return expr;
}

Expr* Compiler::expand_begin(Expr* expr)
{
	for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
	{
		expr->begin.body[i] = expand(expr->begin.body[i]);
	}
	return expr;
}

Expr* Compiler::rewrite_define_in(Expr* expr, OrderedNameSet& names)
{
	if (expr->kind == ExprKind::Define)
	{
		bool inserted = names.insert(expr->define.name);
		Expr* set_e = make_expr(ExprKind::SetBang, expr->loc);
		set_e->set_bang.name = expr->define.name;
		set_e->set_bang.value = expr->define.value;
		set_e->set_bang.is_init = inserted;
		return set_e;
	}
	// Begin is the only transparent sequence sugar sharing the enclosing scope,
	// so every other kind is treated as opaque.
	if (expr->kind == ExprKind::Begin)
	{
		for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
		{
			expr->begin.body[i] = rewrite_define_in(expr->begin.body[i], names);
		}
	}
	return expr;
}

Slice<Expr*> Compiler::hoist_defines_in_body(Slice<Expr*> body, SourceLoc loc)
{
	OrderedNameSet names;
	for (uint32_t i = 0; i < body.size(); ++i)
	{
		body[i] = rewrite_define_in(body[i], names);
	}
	if (names.ordered.empty())
	{
		return body;
	}

	uint32_t n = static_cast<uint32_t>(names.ordered.size());

	// #f sentinels + the in-place set!s left by rewrite_define_in give the body
	// letrec* semantics.
	Expr** vals = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		vals[i] = make_boolean_lit(false, loc);
	}

	Expr* let_e = make_expr(ExprKind::Let, loc);
	let_e->let.names = arena.copy_slice(names.ordered);
	let_e->let.vals = {vals, n};
	let_e->let.body = body;

	return arena.copy_slice({let_e});
}

Expr* Compiler::expand_letrec(Expr* expr)
{
	// (letrec ((x1 e1) ... (xn en)) body...)
	//   ==>
	// (let ((x1 #f) ... (xn #f))
	//   (set! x1 e1) ... (set! xn en)
	//   body...)
	// Sequential set!s give letrec* semantics, which is what almost all uses
	// of letrec actually want and matches what we accept for both keywords.
	uint32_t n = expr->let.names.size();

	Expr** sentinels = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		sentinels[i] = make_boolean_lit(false, expr->loc);
	}

	uint32_t body_n = n + expr->let.body.size();
	Expr** new_body = arena.alloc_array<Expr*>(body_n);
	for (uint32_t i = 0; i < n; ++i)
	{
		Expr* set_e = make_expr(ExprKind::SetBang, expr->let.vals[i]->loc);
		set_e->set_bang.name = expr->let.names[i];
		set_e->set_bang.value = expr->let.vals[i];
		set_e->set_bang.is_init = true;
		new_body[i] = set_e;
	}
	for (uint32_t i = 0; i < expr->let.body.size(); ++i)
	{
		new_body[n + i] = expr->let.body[i];
	}

	Expr* let_e = make_expr(ExprKind::Let, expr->loc);
	let_e->let.names = expr->let.names;
	let_e->let.vals = {sentinels, n};
	let_e->let.body = {new_body, body_n};

	return expand_let(let_e);
}

std::string_view Compiler::gensym()
{
	constexpr size_t max =
		sizeof("%t ") + std::numeric_limits<decltype(gensym_counter_)>::digits10 + 1;
	char* buf = static_cast<char*>(arena.alloc_raw(max, 1));
	// The embedded space cannot appear inside a lexed identifier, so a user
	// binding can never collide with a temp.
	int n = snprintf(buf, max, "%%t %u", gensym_counter_++);
	return {buf, static_cast<size_t>(n)};
}

// ANF hoist temps are single-use by construction.
static bool is_anf_temp(std::string_view name)
{
	return name.starts_with("%t ");
}

Expr* Compiler::anf_wrap(AnfBindings& bindings, Expr* body)
{
	// bindings[0] becomes the outermost Let: vals evaluate in the order they
	// were collected.
	for (size_t i = bindings.size(); i-- > 0;)
	{
		Expr* let_e = make_expr(ExprKind::Let, bindings[i].second->loc);
		let_e->let.names = arena.copy_slice({bindings[i].first});
		let_e->let.vals = arena.copy_slice({bindings[i].second});
		let_e->let.body = arena.copy_slice({body});
		body = let_e;
	}
	return body;
}

Expr* Compiler::anf_atomize(Expr* expr, AnfBindings& bindings)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			return expr;

		case ExprKind::Lambda:
			return compute_anf(expr);

		default:
		{
			std::string_view tmp = gensym();
			bindings.push_back({tmp, compute_anf(expr)});
			Expr* ref = make_expr(ExprKind::VarRef, expr->loc);
			ref->var_ref.name = tmp;
			return ref;
		}
	}
}

Expr* Compiler::compute_anf(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			return expr;

		case ExprKind::Lambda:
		case ExprKind::Let:
			walk_children(expr, [&](Expr*& c) { c = compute_anf(c); });
			return expr;

		case ExprKind::If:
		{
			AnfBindings bindings;
			expr->if_.test = anf_atomize(expr->if_.test, bindings);
			expr->if_.consequent = compute_anf(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				expr->if_.alternate = compute_anf(expr->if_.alternate);
			}
			return anf_wrap(bindings, expr);
		}

		case ExprKind::Begin:
		{
			// (begin e1 e2 e3) -> (let ((t1 e1)) (let ((t2 e2)) e3))
			Slice<Expr*>& body = expr->begin.body;
			if (body.empty())
			{
				return make_expr(ExprKind::UnknownLit, expr->loc);
			}
			AnfBindings bindings;
			for (uint32_t i = 0; i + 1 < body.size(); ++i)
			{
				bindings.push_back({gensym(), compute_anf(body[i])});
			}
			return anf_wrap(bindings, compute_anf(body.back()));
		}

		case ExprKind::Call:
		{
			Expr* proc = expr->call.proc;
			if (proc->kind == ExprKind::Lambda
				&& !proc->lambda.is_variadic
				&& proc->lambda.params.size() == expr->call.args.size())
			{
				// ((lambda (x ...) body ...) e ...) -> (let ((x e) ...) body ...)
				Expr* let_e = make_expr(ExprKind::Let, expr->loc);
				let_e->let.names = proc->lambda.params;
				let_e->let.vals = expr->call.args;
				let_e->let.body = proc->lambda.body;
				return compute_anf(let_e);
			}
			AnfBindings bindings;
			expr->call.proc = anf_atomize(proc, bindings);
			for (uint32_t i = 0; i < expr->call.args.size(); ++i)
			{
				expr->call.args[i] = anf_atomize(expr->call.args[i], bindings);
			}
			return anf_wrap(bindings, expr);
		}

		case ExprKind::Apply:
		{
			AnfBindings bindings;
			expr->apply.proc = anf_atomize(expr->apply.proc, bindings);
			expr->apply.args = anf_atomize(expr->apply.args, bindings);
			return anf_wrap(bindings, expr);
		}

		case ExprKind::SetBang:
		{
			AnfBindings bindings;
			expr->set_bang.value = anf_atomize(expr->set_bang.value, bindings);
			return anf_wrap(bindings, expr);
		}

		case ExprKind::SetRef:
		{
			AnfBindings bindings;
			expr->set_ref.obj = anf_atomize(expr->set_ref.obj, bindings);
			expr->set_ref.key = anf_atomize(expr->set_ref.key, bindings);
			expr->set_ref.value = anf_atomize(expr->set_ref.value, bindings);
			return anf_wrap(bindings, expr);
		}

		default:
			JET_DIE("%d:%d: anf: unhandled ExprKind %d (surface form not expanded?)",
					 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
	}
}

static bool is_anf_atom(Expr* e)
{
	switch (e->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
		case ExprKind::Lambda:
			return true;
		default:
			return false;
	}
}

void Compiler::verify_anf(Expr* expr)
{
	auto check_atom = [&](Expr* e) {
		JET_DIE_UNLESS(is_anf_atom(e), "%d:%d: anf: non-atomic operand (kind %d)", e->loc.line,
						e->loc.col, static_cast<int>(e->kind));
		verify_anf(e);
	};

	switch (expr->kind)
	{
		case ExprKind::Call:
		case ExprKind::Apply:
		case ExprKind::SetBang:
		case ExprKind::SetRef:
			walk_children(expr, check_atom);
			break;

		case ExprKind::If:
			check_atom(expr->if_.test);
			verify_anf(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				verify_anf(expr->if_.alternate);
			}
			break;

		default:
			walk_children(expr, [&](Expr* e) { verify_anf(e); });
			break;
	}
}

bool Compiler::prim_binding_lowerable(ResolvedBinding b, std::string_view prim)
{
	if (b.lambda != toplevel_lambda_)
	{
		return false;
	}
	LambdaBindings& tl = lambda_bindings_[toplevel_lambda_];
	if (get(tl.reassigned_after_init, b.breadth))
	{
		return false;
	}
	Expr* init = get(tl.bound_init, b.breadth);
	return init && init->kind == ExprKind::PrimRef && init->prim_ref.name == prim;
}

void Compiler::record_ref(ResolvedBinding b)
{
	// Codegen wires each transit lambda's clos to forward the Slot, so
	// every lambda between owner and the current scope needs an upvalue entry.
	if (lambdas_.back() == b.lambda)
	{
		return;
	}

	get(lambda_bindings_[b.lambda].captured, b.breadth) = true;

	uint32_t bw = static_cast<uint32_t>(b.breadth);
	uint64_t key = (static_cast<uint64_t>(b.lambda->id) << 32) | bw;
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		Expr* lam = lambdas_[i];
		if (lam == b.lambda)
		{
			return;
		}
		LambdaBindings& lb = lambda_bindings_[lam];
		if (lb.upvalue_keys.insert(key).second)
		{
			lb.upvalues.push_back({b.lambda, bw});
		}
	}
	JET_DIE("record_ref: owner not in lambdas_");
}

void Compiler::record_set(ResolvedBinding b, bool is_init, Expr* value)
{
	LambdaBindings& lb = lambda_bindings_[b.lambda];
	get(lb.mutated, b.breadth) = true;
	if (!is_init)
	{
		get(lb.reassigned_after_init, b.breadth) = true;
	}
	if (is_init && value)
	{
		get(lb.bound_init, b.breadth) = value;
	}
}

std::optional<ResolvedBinding> Compiler::lookup_name(std::string_view name)
{
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_[i];
		auto it = idx.find(name);
		if (it != idx.end())
		{
			return ResolvedBinding{.lambda = lambdas_[i], .breadth = it->second};
		}
	}
	return std::nullopt;
}

void Compiler::push_lambda_scope(Expr* lambda)
{
	lambdas_.push_back(lambda);
	std::unordered_map<std::string_view, size_t> idx;
	Slice<std::string_view>& names = lambda->lambda.names;
	idx.reserve(names.size());
	for (size_t b = 0; b < names.size(); ++b)
	{
		idx[names[b]] = b;
	}
	lambda_name_index_.push_back(std::move(idx));
}

void Compiler::pop_lambda_scope()
{
	lambdas_.pop_back();
	lambda_name_index_.pop_back();
}

void Compiler::freeze_lambda(Expr* lambda)
{
	LambdaBindings& lb = lambda_bindings_[lambda];

	lambda->lambda.upvalues = arena.copy_slice(lb.upvalues);

	uint32_t n = lambda->lambda.names.size();
	bool* captured_data = arena.alloc_array<bool>(n);
	bool* mutated_data = arena.alloc_array<bool>(n);
	bool* reassigned_data = arena.alloc_array<bool>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		captured_data[i] = get(lb.captured, i);
		mutated_data[i] = get(lb.mutated, i);
		reassigned_data[i] = get(lb.reassigned_after_init, i);
	}
	lambda->lambda.captured_locals = {captured_data, n};
	lambda->lambda.mutated_locals = {mutated_data, n};
	lambda->lambda.reassigned_after_init_locals = {reassigned_data, n};
}

void Compiler::compute_binding_addresses(Program& program)
{
	bindings_.assign(next_expr_id_ + 1, ResolvedBinding{});

	push_lambda_scope(toplevel_lambda_);
	frame_names_.push_back({toplevel_lambda_->lambda.names.begin(), toplevel_lambda_->lambda.names.end()});
	for (Expr* form : program.forms)
	{
		compute_binding_addresses_in(form);
	}
	toplevel_lambda_->lambda.names = arena.copy_slice(frame_names_.back());
	frame_names_.pop_back();
	pop_lambda_scope();
}

void Compiler::recompute_lambda_bindings(Program& program)
{
	lambda_bindings_.clear();
	all_lambdas_.clear();

	push_lambda_scope(toplevel_lambda_);
	for (Expr* form : program.forms)
	{
		recompute_lambda_bindings_in(form);
	}
	pop_lambda_scope();
	all_lambdas_.push_back(toplevel_lambda_);
}

void Compiler::resolve_bindings(Program& program)
{
	compute_binding_addresses(program);
	recompute_lambda_bindings(program);
}

void Compiler::run_optimization_passes(Program& program)
{
	if (flags_.inlining)
	{
		run_anf_inline(program);
		recompute_lambda_bindings(program);
	}

	if (flags_.specialize_ops)
	{
		run_binarize_arith(program);
		recompute_lambda_bindings(program);
	}

	if (flags_.lift_lambdas)
	{
		run_lambda_lift(program);
		resolve_bindings(program);
	}
}

void Compiler::compute_binding_addresses_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
		{
			std::optional<ResolvedBinding> found = lookup_name(expr->var_ref.name);
			if (!found)
			{
				JET_DIE("%d:%d: unresolved variable '%.*s'", expr->loc.line, expr->loc.col,
						 static_cast<int>(expr->var_ref.name.size()), expr->var_ref.name.data());
			}
			bindings_[expr->id] = *found;
			break;
		}

		case ExprKind::Lambda:
		{
			std::vector<std::string_view> names;
			for (std::string_view param : expr->lambda.params)
			{
				names.push_back(param);
			}
			expr->lambda.names = arena.copy_slice(names);

			push_lambda_scope(expr);
			frame_names_.push_back(std::move(names));
			walk_children(expr, [&](Expr* e) { compute_binding_addresses_in(e); });
			expr->lambda.names = arena.copy_slice(frame_names_.back());
			frame_names_.pop_back();
			pop_lambda_scope();
			break;
		}

		case ExprKind::Let:
		{
			for (Expr* val : expr->let.vals)
			{
				compute_binding_addresses_in(val);
			}

			std::vector<std::pair<std::string_view, std::optional<size_t>>> shadowed;
			{
				std::vector<std::string_view>& frame = frame_names_.back();
				std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
				expr->let.slot_base = static_cast<uint32_t>(frame.size());
				expr->let.owner = lambdas_.back();

				for (std::string_view name : expr->let.names)
				{
					size_t breadth = frame.size();
					frame.push_back(name);
					auto it = idx.find(name);
					shadowed.push_back({name, it == idx.end() ? std::nullopt
															  : std::optional<size_t>(it->second)});
					idx[name] = breadth;
				}
			}

			for (Expr* form : expr->let.body)
			{
				compute_binding_addresses_in(form);
			}

			// A lambda in the body pushes lambda_name_index_, which can
			// reallocate it: no reference to it may survive the body walk.
			std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
			for (auto it = shadowed.rbegin(); it != shadowed.rend(); ++it)
			{
				if (it->second)
				{
					idx[it->first] = *it->second;
				}
				else
				{
					idx.erase(it->first);
				}
			}
			break;
		}

		case ExprKind::SetBang:
		{
			compute_binding_addresses_in(expr->set_bang.value);
			std::optional<ResolvedBinding> found = lookup_name(expr->set_bang.name);
			if (!found)
			{
				JET_DIE("%d:%d: unresolved variable '%.*s' in set!", expr->loc.line, expr->loc.col,
						 static_cast<int>(expr->set_bang.name.size()), expr->set_bang.name.data());
			}
			bindings_[expr->id] = *found;
			break;
		}

		default:
			walk_children(expr, [&](Expr* e) { compute_binding_addresses_in(e); });
			break;
	}
}

void Compiler::recompute_lambda_bindings_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
			if (!get(intrinsic_callee_, expr->id))
			{
				record_ref(bindings_[expr->id]);
			}
			break;

		case ExprKind::Lambda:
			push_lambda_scope(expr);
			walk_children(expr, [&](Expr* e) { recompute_lambda_bindings_in(e); });
			pop_lambda_scope();
			all_lambdas_.push_back(expr);
			break;

		case ExprKind::SetBang:
		{
			recompute_lambda_bindings_in(expr->set_bang.value);
			ResolvedBinding b = bindings_[expr->id];
			record_ref(b);
			record_set(b, expr->set_bang.is_init, expr->set_bang.value);
			break;
		}

		default:
			walk_children(expr, [&](Expr* e) { recompute_lambda_bindings_in(e); });
			break;
	}
}

void Compiler::collect_tail_calls(Program& program)
{
	tail_cache_.assign(next_expr_id_ + 1, false);
	for (Expr* form : program.forms)
	{
		collect_tail_calls(form, false);
	}
}

void Compiler::collect_tail_calls(Expr* expr, bool in_tail)
{
	switch (expr->kind)
	{
		case ExprKind::Call:
			if (in_tail)
			{
				tail_cache_[expr->id] = true;
			}
			walk_children(expr, [&](Expr* e) { collect_tail_calls(e, false); });
			break;

		case ExprKind::Lambda:
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				bool is_last = (i == expr->lambda.body.size() - 1);
				collect_tail_calls(expr->lambda.body[i], is_last);
			}
			break;

		case ExprKind::If:
			collect_tail_calls(expr->if_.test, false);
			collect_tail_calls(expr->if_.consequent, in_tail);
			if (expr->if_.alternate)
			{
				collect_tail_calls(expr->if_.alternate, in_tail);
			}
			break;

		case ExprKind::Let:
			for (Expr* val : expr->let.vals)
			{
				collect_tail_calls(val, false);
			}
			for (uint32_t i = 0; i < expr->let.body.size(); ++i)
			{
				bool is_last = (i == expr->let.body.size() - 1);
				collect_tail_calls(expr->let.body[i], is_last && in_tail);
			}
			break;

		default:
			walk_children(expr, [&](Expr* e) { collect_tail_calls(e, false); });
			break;
	}
}

namespace
{

	bool needs_slot(Expr* owner, uint32_t breadth)
	{
		return owner->lambda.captured_locals[breadth] && owner->lambda.mutated_locals[breadth];
	}

	std::optional<uint16_t> find_upvalue(Expr* current, Expr* owner, uint32_t breadth)
	{
		Slice<UpvalueRef>& ups = current->lambda.upvalues;
		for (uint32_t i = 0; i < ups.size(); ++i)
		{
			if (ups[i].owner == owner && ups[i].breadth == breadth)
			{
				return static_cast<uint16_t>(i);
			}
		}
		return std::nullopt;
	}

	bool is_literal_key(Expr* e)
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			case ExprKind::SymbolLit:
			case ExprKind::CharacterLit:
			case ExprKind::BooleanLit:
			case ExprKind::StringLit:
				return true;
			default:
				return false;
		}
	}

	uint16_t narrow_off(size_t v)
	{
		JET_DIE_WHEN(v > UINT16_MAX, "codegen: frame offset %zu exceeds uint16_t", v);
		return static_cast<uint16_t>(v);
	}

	std::optional<Opcode> binary_arith_opcode(std::string_view name)
	{
		if (name == "-")
		{
			return Opcode::sub;
		}
		if (name == "+")
		{
			return Opcode::add;
		}
		if (name == "*")
		{
			return Opcode::mul;
		}
		if (name == "/")
		{
			return Opcode::div;
		}
		if (name == "=")
		{
			return Opcode::eq;
		}
		if (name == "<")
		{
			return Opcode::lt;
		}
		if (name == "<=")
		{
			return Opcode::le;
		}
		if (name == ">")
		{
			return Opcode::gt;
		}
		if (name == ">=")
		{
			return Opcode::ge;
		}
		return std::nullopt;
	}

} // namespace

void Compiler::run_op_selection(Program& program)
{
	// Remove references to callees that will be lowered to intrinsics to avoid classifying them
	// as upvalues during instruction selection.
	intrinsic_callee_.assign(next_expr_id_ + 1, false);
	for (Expr* form : program.forms)
	{
		collect_intrinsic_callees(form, toplevel_lambda_);
	}
	recompute_lambda_bindings(program);

	for (Expr* L : all_lambdas_)
	{
		freeze_lambda(L);
	}

	selected_ops_.assign(next_expr_id_ + 1, std::nullopt);
	for (Expr* form : program.forms)
	{
		select_ops_in(form, toplevel_lambda_);
	}
}

void Compiler::select_ops_in(Expr* expr, Expr* current)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
			select_var_op(expr, current, false);
			break;

		case ExprKind::Call:
			if (get(intrinsic_callee_, expr->call.proc->id))
			{
				for (Expr* arg : expr->call.args)
				{
					select_ops_in(arg, current);
				}
			}
			else
			{
				walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
			}
			select_call_op(expr, current);
			break;

		case ExprKind::Lambda:
			walk_children(expr, [&](Expr* e) { select_ops_in(e, expr); });
			break;

		case ExprKind::SetBang:
			select_ops_in(expr->set_bang.value, current);
			select_var_op(expr, current, true);
			break;

		case ExprKind::SetRef:
			walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
			select_field_op(expr, current, expr->set_ref.obj, expr->set_ref.key, true);
			break;

		case ExprKind::Let:
			walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
			select_if_cmp(expr);
			break;

		default:
			walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
			break;
	}
}

bool Compiler::is_self_tail_call(Expr* expr, Expr* current)
{
	if (expr->kind != ExprKind::Call)
	{
		return false;
	}
	Expr* proc = expr->call.proc;
	if (proc->kind != ExprKind::VarRef)
	{
		return false;
	}
	if (!is_tail(expr))
	{
		return false;
	}
	if (current->lambda.is_variadic)
	{
		return false;
	}
	if (current->lambda.params.size() != expr->call.args.size())
	{
		return false;
	}
	ResolvedBinding proc_binding = binding(proc);
	LambdaBindings& lb = lambda_bindings_[proc_binding.lambda];
	return !get(lb.reassigned_after_init, proc_binding.breadth)
		   && get(lb.bound_init, proc_binding.breadth) == current;
}

bool Compiler::is_intrinsic_callee(Expr* expr, Expr* current)
{
	if (!flags_.specialize_ops)
	{
		return false;
	}
	if (is_self_tail_call(expr, current))
	{
		return true;
	}
	Expr* proc = expr->call.proc;
	if (proc->kind != ExprKind::VarRef || expr->call.args.size() != 2)
	{
		return false;
	}
	std::string_view name = proc->var_ref.name;
	if (!binary_arith_opcode(name) && name != "ref")
	{
		return false;
	}
	return prim_binding_lowerable(binding(proc), name);
}

void Compiler::collect_intrinsic_callees(Expr* expr, Expr* current)
{
	if (expr->kind == ExprKind::Call && is_intrinsic_callee(expr, current))
	{
		get(intrinsic_callee_, expr->call.proc->id) = true;
	}
	if (expr->kind == ExprKind::Lambda)
	{
		current = expr;
	}
	walk_children(expr, [&](Expr* e) { collect_intrinsic_callees(e, current); });
}

void Compiler::select_call_op(Expr* expr, Expr* current)
{
	Expr* proc = expr->call.proc;
	OpSelection& sel = selected_ops_[expr->id].emplace();
	sel.op = Opcode::callw;

	if (!flags_.specialize_ops)
	{
		return;
	}

	// Self-tail-call: recur.
	if (is_self_tail_call(expr, current))
	{
		sel.op = Opcode::recurw;
		return;
	}

	if (proc->kind != ExprKind::VarRef)
	{
		return;
	}

	ResolvedBinding proc_binding = binding(proc);

	// Two-arg arithmetic: rr, or rk when the rhs is a number literal.
	if (expr->call.args.size() == 2)
	{
		std::string_view name = proc->var_ref.name;
		std::optional<Opcode> arith = binary_arith_opcode(name);
		if (arith && prim_binding_lowerable(proc_binding, name))
		{
			Opcode op = *arith;
			if (expr->call.args[1]->kind == ExprKind::NumberLit)
			{
				switch (op)
				{
					case Opcode::sub: op = Opcode::subk; break;
					case Opcode::add: op = Opcode::addk; break;
					case Opcode::mul: op = Opcode::mulk; break;
					case Opcode::div: op = Opcode::divk; break;
					case Opcode::eq:  op = Opcode::eqk;  break;
					case Opcode::lt:  op = Opcode::ltk;  break;
					default:          break;   // no rk form for le/gt/ge
				}
			}
			sel.op = op;
			return;
		}
	}

	if (proc->var_ref.name == "ref"
		&& expr->call.args.size() == 2
		&& prim_binding_lowerable(proc_binding, "ref"))
	{
		select_field_op(expr, current, expr->call.args[0], expr->call.args[1], false);
		return;
	}

	bool slot = needs_slot(proc_binding.lambda, static_cast<uint32_t>(proc_binding.breadth));

	// Callee in a boxed binding: slot IC.
	if (proc_binding.lambda != current && slot)
	{
		std::optional<uint16_t> found = find_upvalue(current, proc_binding.lambda,
													 static_cast<uint32_t>(proc_binding.breadth));
		JET_DIE_UNLESS(found, "codegen: cacheable call missing upvalue entry");
		sel.op = Opcode::cs_0;
		sel.u.call_ic_slot.upvalue_idx = *found;
		return;
	}

	// Callee in an unboxed binding: direct IC.
	if (!slot)
	{
		sel.op = Opcode::cd_0;
		if (proc_binding.lambda == current)
		{
			sel.u.call_ic_direct.src = 0;
			sel.u.call_ic_direct.idx = static_cast<uint16_t>(proc_binding.breadth);
		}
		else
		{
			std::optional<uint16_t> found = find_upvalue(current, proc_binding.lambda,
														 static_cast<uint32_t>(proc_binding.breadth));
			JET_DIE_UNLESS(found, "codegen: cacheable call missing upvalue entry");
			sel.u.call_ic_direct.src = 1;
			sel.u.call_ic_direct.idx = *found;
		}
		return;
	}
}

// ANF spells a branch on a compare as (let ((%t (< a b))) (if %t C A)): reselect the compare
// as the fused branch op. The fold drops the write to %t, so any second ref to the temp in a
// branch arm -- (or x y) expands to (if %t %t y) -- keeps the compare unfused.
void Compiler::select_if_cmp(Expr* expr)
{
	if (expr->let.names.size() != 1 || expr->let.body.size() != 1 || !is_anf_temp(expr->let.names[0]))
	{
		return;
	}
	Expr* val = expr->let.vals[0];
	Expr* if_e = expr->let.body[0];
	if (val->kind != ExprKind::Call || if_e->kind != ExprKind::If
		|| if_e->if_.test->kind != ExprKind::VarRef || !selected_ops_[val->id])
	{
		return;
	}
	Opcode fused;
	switch (selected_ops_[val->id]->op)
	{
		case Opcode::eq:  fused = Opcode::if_eq;  break;
		case Opcode::lt:  fused = Opcode::if_lt;  break;
		case Opcode::le:  fused = Opcode::if_le;  break;
		case Opcode::gt:  fused = Opcode::if_gt;  break;
		case Opcode::ge:  fused = Opcode::if_ge;  break;
		case Opcode::eqk: fused = Opcode::if_eqk; break;
		case Opcode::ltk: fused = Opcode::if_ltk; break;
		default:          return;
	}
	ResolvedBinding temp = binding(if_e->if_.test);
	if (temp.lambda != expr->let.owner || temp.breadth != expr->let.slot_base)
	{
		return;
	}
	bool reread = false;
	auto&& scan = [&](Expr* e, auto&& self) -> void
	{
		if (e->kind == ExprKind::VarRef || e->kind == ExprKind::SetBang)
		{
			ResolvedBinding b = binding(e);
			reread = reread || (b.lambda == temp.lambda && b.breadth == temp.breadth);
		}
		walk_children(e, [&](Expr* c) { self(c, self); });
	};
	scan(if_e->if_.consequent, scan);
	if (if_e->if_.alternate)
	{
		scan(if_e->if_.alternate, scan);
	}
	if (reread)
	{
		return;
	}
	selected_ops_[val->id]->op = fused;
}

void Compiler::select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, bool is_set)
{
	OpSelection& sel = selected_ops_[expr->id].emplace();

	if (!flags_.specialize_ops)
	{
		sel.op = is_set ? Opcode::stf : Opcode::ldf;
		return;
	}

	bool ck = is_literal_key(key);
	sel.op = is_set ? (ck ? Opcode::stfk : Opcode::stf) : (ck ? Opcode::ldfk : Opcode::ldf);
}

void Compiler::select_var_op(Expr* expr, Expr* current, bool is_set)
{
	ResolvedBinding b = binding(expr);
	bool slot = needs_slot(b.lambda, static_cast<uint32_t>(b.breadth));
	OpSelection& sel = selected_ops_[expr->id].emplace();
	if (b.lambda == current)
	{
		// mov marks a plain register access: refs read the register directly
		// (no code), sets write it.
		sel.op = slot ? (is_set ? Opcode::std : Opcode::ldd) : Opcode::mov;
		sel.u.var.addr = narrow_off(b.breadth);
		return;
	}
	std::optional<uint16_t> found = find_upvalue(current, b.lambda, static_cast<uint32_t>(b.breadth));
	std::string_view name = expr->kind == ExprKind::SetBang ? expr->set_bang.name : expr->var_ref.name;
	JET_DIE_UNLESS(found, "select-pass: ref to non-local without upvalue entry: '%.*s'",
			   static_cast<int>(name.size()), name.data());
	sel.op = is_set ? Opcode::stu : (slot ? Opcode::ldus : Opcode::ldu);
	sel.u.var.addr = *found;
}

namespace
{

	// Counts post-ANF nodes: every non-atomic operand costs an extra
	// Let + VarRef pair on top of the expression itself.
	constexpr uint32_t INLINE_BUDGET = 32;

	uint32_t count_exprs(Expr* e);

	uint32_t count_exprs_slice(Slice<Expr*> body)
	{
		uint32_t n = 0;
		for (uint32_t i = 0; i < body.size(); ++i)
		{
			n += count_exprs(body[i]);
		}
		return n;
	}

	uint32_t count_exprs(Expr* e)
	{
		if (!e)
		{
			return 0;
		}
		uint32_t n = 1;
		switch (e->kind)
		{
			case ExprKind::Call:
				n += count_exprs(e->call.proc);
				n += count_exprs_slice(e->call.args);
				break;
			case ExprKind::Apply:
				n += count_exprs(e->apply.proc);
				n += count_exprs(e->apply.args);
				break;
			case ExprKind::If:
				n += count_exprs(e->if_.test);
				n += count_exprs(e->if_.consequent);
				n += count_exprs(e->if_.alternate);
				break;
			case ExprKind::Lambda:
				n += count_exprs_slice(e->lambda.body);
				break;
			case ExprKind::SetBang:
				n += count_exprs(e->set_bang.value);
				break;
			case ExprKind::SetRef:
				n += count_exprs(e->set_ref.obj);
				n += count_exprs(e->set_ref.key);
				n += count_exprs(e->set_ref.value);
				break;
			case ExprKind::Let:
				n += count_exprs_slice(e->let.vals);
				n += count_exprs_slice(e->let.body);
				break;
			default:
				break;
		}
		return n;
	}

	uint64_t binding_key(ResolvedBinding b)
	{
		return (static_cast<uint64_t>(b.lambda->id) << 32) | static_cast<uint32_t>(b.breadth);
	}

	struct AnfInline
	{
		Compiler& db;
		// A hit means the binding provably holds this init wherever it is
		// referenced, so a call may splice it and a reference may become it.
		std::unordered_map<uint64_t, Expr*> lambda_cands{};
		std::unordered_map<uint64_t, Expr*> const_cands{};
		std::unordered_set<Expr*> candidate_lambdas{};
		// Candidates being spliced or walked at their own definition: calls
		// to them stay calls, so splicing terminates and a recursive body is
		// never unrolled into itself.
		std::unordered_set<Expr*> active{};
		std::vector<Expr*> hosts{};
		// Ids at or above this are clones this pass made. A cloned Let has no
		// row in the pre-pass analysis, so its slots' mutated flags are
		// unknowable and it never registers candidates.
		uint32_t first_clone_id;

		void consider(Expr* owner, size_t breadth, Expr* init)
		{
			if (init->kind == ExprKind::Lambda)
			{
				if (init->lambda.is_variadic || count_exprs_slice(init->lambda.body) > INLINE_BUDGET)
				{
					return;
				}
				lambda_cands[binding_key({owner, breadth})] = init;
				candidate_lambdas.insert(init);
			}
			else if (init->kind == ExprKind::NumberLit || init->kind == ExprKind::BooleanLit ||
					 init->kind == ExprKind::CharacterLit)
			{
				const_cands[binding_key({owner, breadth})] = init;
			}
		}

		struct CloneCtx
		{
			Expr* callee;
			Expr* host;
			uint32_t base;
			std::vector<std::pair<Expr*, Expr*>> lambda_stack;
		};

		ResolvedBinding translate(ResolvedBinding rb, CloneCtx& ctx)
		{
			if (rb.lambda == ctx.callee)
			{
				return {ctx.host, ctx.base + rb.breadth};
			}
			for (auto it = ctx.lambda_stack.rbegin(); it != ctx.lambda_stack.rend(); ++it)
			{
				if (it->first == rb.lambda)
				{
					rb.lambda = it->second;
					return rb;
				}
			}
			// A free variable of the whole spliced body: its scope encloses
			// the callee's definition and therefore every call site that can
			// reference the callee, so the binding is valid unchanged.
			return rb;
		}

		Slice<Expr*> clone_slice(Slice<Expr*> src, CloneCtx& ctx)
		{
			Expr** data = db.arena.alloc_array<Expr*>(src.size());
			for (uint32_t i = 0; i < src.size(); ++i)
			{
				data[i] = clone(src[i], ctx);
			}
			return {data, src.size()};
		}

		Expr* clone(Expr* orig, CloneCtx& ctx)
		{
			Expr* e = db.make_expr(orig->kind, orig->loc);
			switch (orig->kind)
			{
				case ExprKind::NumberLit:
					e->number_lit = orig->number_lit;
					break;
				case ExprKind::StringLit:
					e->string_lit = orig->string_lit;
					break;
				case ExprKind::BooleanLit:
					e->boolean_lit = orig->boolean_lit;
					break;
				case ExprKind::CharacterLit:
					e->character_lit = orig->character_lit;
					break;
				case ExprKind::SymbolLit:
					e->symbol_lit = orig->symbol_lit;
					break;
				case ExprKind::UnknownLit:
					e->unknown_lit = orig->unknown_lit;
					break;
				case ExprKind::PrimRef:
					e->prim_ref = orig->prim_ref;
					break;
				case ExprKind::VarRef:
					e->var_ref = orig->var_ref;
					get(db.bindings_, e->id) = translate(get(db.bindings_, orig->id), ctx);
					break;
				case ExprKind::Call:
					e->call.proc = clone(orig->call.proc, ctx);
					e->call.args = clone_slice(orig->call.args, ctx);
					break;
				case ExprKind::Apply:
					e->apply.proc = clone(orig->apply.proc, ctx);
					e->apply.args = clone(orig->apply.args, ctx);
					break;
				case ExprKind::Lambda:
					e->lambda.params = orig->lambda.params;
					e->lambda.is_variadic = orig->lambda.is_variadic;
					e->lambda.names = orig->lambda.names;
					e->lambda.captured_locals = {};
					e->lambda.mutated_locals = {};
					e->lambda.reassigned_after_init_locals = {};
					e->lambda.upvalues = {};
					ctx.lambda_stack.push_back({orig, e});
					e->lambda.body = clone_slice(orig->lambda.body, ctx);
					ctx.lambda_stack.pop_back();
					break;
				case ExprKind::SetBang:
					e->set_bang.name = orig->set_bang.name;
					e->set_bang.is_init = orig->set_bang.is_init;
					e->set_bang.value = clone(orig->set_bang.value, ctx);
					get(db.bindings_, e->id) = translate(get(db.bindings_, orig->id), ctx);
					break;
				case ExprKind::SetRef:
					e->set_ref.obj = clone(orig->set_ref.obj, ctx);
					e->set_ref.key = clone(orig->set_ref.key, ctx);
					e->set_ref.value = clone(orig->set_ref.value, ctx);
					break;
				case ExprKind::If:
					e->if_.test = clone(orig->if_.test, ctx);
					e->if_.consequent = clone(orig->if_.consequent, ctx);
					e->if_.alternate = orig->if_.alternate ? clone(orig->if_.alternate, ctx) : nullptr;
					break;
				case ExprKind::Let:
				{
					e->let.names = orig->let.names;
					if (orig->let.owner == ctx.callee)
					{
						e->let.owner = ctx.host;
						e->let.slot_base = ctx.base + orig->let.slot_base;
					}
					else
					{
						Expr* owner = nullptr;
						for (auto it = ctx.lambda_stack.rbegin(); it != ctx.lambda_stack.rend(); ++it)
						{
							if (it->first == orig->let.owner)
							{
								owner = it->second;
								break;
							}
						}
						JET_DIE_UNLESS(owner, "anf-inline: cloned let owned by a lambda outside the clone");
						e->let.owner = owner;
						e->let.slot_base = orig->let.slot_base;
					}
					e->let.vals = clone_slice(orig->let.vals, ctx);
					e->let.body = clone_slice(orig->let.body, ctx);
					break;
				}
				default:
					JET_DIE("%d:%d: anf-inline: unhandled ExprKind %d in clone", orig->loc.line,
							 orig->loc.col, static_cast<int>(orig->kind));
			}
			return e;
		}

		Expr* splice(Expr* call, Expr* callee)
		{
			Expr* host = hosts.back();
			uint32_t base = host->lambda.names.size();

			// The callee's whole frame (params + let slots) is appended to
			// the host frame, so {callee, i} translates to {host, base + i}.
			uint32_t n_callee = callee->lambda.names.size();
			std::string_view* names = db.arena.alloc_array<std::string_view>(base + n_callee);
			for (uint32_t i = 0; i < base; ++i)
			{
				names[i] = host->lambda.names[i];
			}
			for (uint32_t i = 0; i < n_callee; ++i)
			{
				names[base + i] = callee->lambda.names[i];
			}
			host->lambda.names = {names, base + n_callee};

			CloneCtx ctx{callee, host, base, {}};
			Expr* e = db.make_expr(ExprKind::Let, call->loc);
			e->let.names = callee->lambda.params;
			e->let.vals = call->call.args;
			e->let.slot_base = base;
			e->let.owner = host;
			e->let.body = clone_slice(callee->lambda.body, ctx);
			return e;
		}

		Expr* walk(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				case ExprKind::StringLit:
				case ExprKind::BooleanLit:
				case ExprKind::CharacterLit:
				case ExprKind::SymbolLit:
				case ExprKind::UnknownLit:
				case ExprKind::PrimRef:
					return expr;

				case ExprKind::VarRef:
				{
					auto it = const_cands.find(binding_key(get(db.bindings_, expr->id)));
					if (it == const_cands.end())
					{
						return expr;
					}
					Expr* lit = it->second;
					Expr* e = db.make_expr(lit->kind, expr->loc);
					switch (lit->kind)
					{
						case ExprKind::NumberLit:
							e->number_lit = lit->number_lit;
							break;
						case ExprKind::BooleanLit:
							e->boolean_lit = lit->boolean_lit;
							break;
						case ExprKind::CharacterLit:
							e->character_lit = lit->character_lit;
							break;
						default:
							JET_DIE("anf-inline: non-literal const candidate");
					}
					return e;
				}

				case ExprKind::Call:
				{
					Expr* proc = expr->call.proc;
					if (proc->kind == ExprKind::VarRef)
					{
						auto it = lambda_cands.find(binding_key(get(db.bindings_, proc->id)));
						if (it != lambda_cands.end())
						{
							Expr* callee = it->second;
							if (callee->lambda.params.size() == expr->call.args.size() &&
								!active.count(callee))
							{
								Expr* let = splice(expr, callee);
								active.insert(callee);
								let = walk(let);
								active.erase(callee);
								return let;
							}
						}
					}
					walk_children(expr, [&](Expr*& c) { c = walk(c); });
					return expr;
				}

				case ExprKind::Apply:
				case ExprKind::SetBang:
				case ExprKind::SetRef:
				case ExprKind::If:
					walk_children(expr, [&](Expr*& c) { c = walk(c); });
					return expr;

				case ExprKind::Lambda:
				{
					bool guard = candidate_lambdas.count(expr) && active.insert(expr).second;
					hosts.push_back(expr);
					walk_children(expr, [&](Expr*& c) { c = walk(c); });
					hosts.pop_back();
					if (guard)
					{
						active.erase(expr);
					}
					return expr;
				}

				case ExprKind::Let:
				{
					if (expr->id < first_clone_id)
					{
						Compiler::LambdaBindings& ob = db.lambda_bindings_[expr->let.owner];
						for (uint32_t i = 0; i < expr->let.names.size(); ++i)
						{
							// A never-written slot's val is its value for the
							// whole scope; a written one (incl. letrec-style
							// is_init set!s over #f sentinels) is covered by
							// bound_init candidacy instead.
							if (!get(ob.mutated, expr->let.slot_base + i))
							{
								consider(expr->let.owner, expr->let.slot_base + i, expr->let.vals[i]);
							}
						}
					}
					walk_children(expr, [&](Expr*& c) { c = walk(c); });
					return expr;
				}

				default:
					JET_DIE("%d:%d: anf-inline: unhandled ExprKind %d", expr->loc.line,
							 expr->loc.col, static_cast<int>(expr->kind));
			}
		}
	};

} // namespace

void Compiler::run_anf_inline(Program& program)
{
	// Needs current recompute_lambda_bindings results, and the caller must rerun
	// that pass afterwards: splices change captures and upvalues.
	AnfInline pass{.db = *this, .first_clone_id = next_expr_id_};
	pass.hosts.push_back(toplevel_lambda_);

	for (Expr* L : all_lambdas_)
	{
		LambdaBindings& lb = lambda_bindings_[L];
		for (size_t i = 0; i < lb.bound_init.size(); ++i)
		{
			Expr* init = lb.bound_init[i];
			if (init && !get(lb.reassigned_after_init, i))
			{
				pass.consider(L, i, init);
			}
		}
	}

	for (uint32_t i = 0; i < program.forms.size(); ++i)
	{
		program.forms[i] = pass.walk(program.forms[i]);
		verify_anf(program.forms[i]);
	}
}

namespace
{

	bool is_nary_arith(std::string_view name)
	{
		// Must stay a subset of select_call_op's fused names: a binarized chain
		// of calls selection cannot fuse is strictly worse than one n-ary call.
		return name == "+" || name == "-" || name == "*" || name == "/";
	}

	struct BinarizeArith
	{
		Compiler& db;

		bool lowerable(Expr* e)
		{
			if (e->call.args.size() < 3 || e->call.proc->kind != ExprKind::VarRef)
			{
				return false;
			}
			std::string_view name = e->call.proc->var_ref.name;
			return is_nary_arith(name) && db.prim_binding_lowerable(db.binding(e->call.proc), name);
		}

		Expr* make_binary(Expr* proc, Expr* lhs, Expr* rhs, SourceLoc loc)
		{
			Expr* e = db.make_expr(ExprKind::Call, loc);
			e->call.proc = proc;
			e->call.args = db.arena.copy_slice({lhs, rhs});
			return e;
		}

		Expr* clone_proc(Expr* proc)
		{
			Expr* e = db.make_expr(ExprKind::VarRef, proc->loc);
			e->var_ref.name = proc->var_ref.name;
			get(db.bindings_, e->id) = get(db.bindings_, proc->id);
			return e;
		}

		void walk(Expr* e)
		{
			walk_children(e, [&](Expr* c) { walk(c); });
			if (e->kind != ExprKind::Call || !lowerable(e))
			{
				return;
			}
			// (op a b c) -> (op (op a b) c), leftward like the prim's fold, so
			// the lowering is bit-identical for doubles.
			Slice<Expr*> args = e->call.args;
			Expr* acc = args[0];
			for (uint32_t i = 1; i + 1 < args.size(); ++i)
			{
				acc = make_binary(clone_proc(e->call.proc), acc, args[i], e->loc);
			}
			e->call.args = db.arena.copy_slice({acc, args.back()});
		}
	};

} // namespace

void Compiler::run_binarize_arith(Program& program)
{
	BinarizeArith pass{.db = *this};
	for (Expr* form : program.forms)
	{
		pass.walk(form);
	}
}

namespace
{

	struct LambdaLift
	{
		Compiler& db;

		static bool is_false_lit(Expr* e)
		{
			return e->kind == ExprKind::BooleanLit && !e->boolean_lit.value;
		}

		static bool name_used_as_value(Expr* expr, std::string_view name)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
					return expr->var_ref.name == name;
				case ExprKind::Call:
				{
					bool proc_is_name = expr->call.proc->kind == ExprKind::VarRef
										&& expr->call.proc->var_ref.name == name;
					if (!proc_is_name && name_used_as_value(expr->call.proc, name))
					{
						return true;
					}
					for (Expr* arg : expr->call.args)
					{
						if (name_used_as_value(arg, name))
						{
							return true;
						}
					}
					return false;
				}
				case ExprKind::SetBang:
					return expr->set_bang.name == name
						   || name_used_as_value(expr->set_bang.value, name);
				default:
				{
					bool found = false;
					walk_children(expr, [&](Expr*& c) { found = found || name_used_as_value(c, name); });
					return found;
				}
			}
		}

		static bool self_calls_all_tail(Expr* expr, std::string_view name, bool in_tail)
		{
			switch (expr->kind)
			{
				case ExprKind::Call:
				{
					bool is_self = expr->call.proc->kind == ExprKind::VarRef
								   && expr->call.proc->var_ref.name == name;
					if (is_self && !in_tail)
					{
						return false;
					}
					if (!self_calls_all_tail(expr->call.proc, name, false))
					{
						return false;
					}
					for (Expr* arg : expr->call.args)
					{
						if (!self_calls_all_tail(arg, name, false))
						{
							return false;
						}
					}
					return true;
				}
				case ExprKind::If:
					return self_calls_all_tail(expr->if_.test, name, false)
						   && self_calls_all_tail(expr->if_.consequent, name, in_tail)
						   && (!expr->if_.alternate
							   || self_calls_all_tail(expr->if_.alternate, name, in_tail));
				case ExprKind::Let:
				{
					for (Expr* val : expr->let.vals)
					{
						if (!self_calls_all_tail(val, name, false))
						{
							return false;
						}
					}
					for (uint32_t i = 0; i < expr->let.body.size(); ++i)
					{
						bool last = (i == expr->let.body.size() - 1);
						if (!self_calls_all_tail(expr->let.body[i], name, last && in_tail))
						{
							return false;
						}
					}
					return true;
				}
				case ExprKind::Lambda:
					for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
					{
						bool last = (i == expr->lambda.body.size() - 1);
						if (!self_calls_all_tail(expr->lambda.body[i], name, last))
						{
							return false;
						}
					}
					return true;
				default:
				{
					bool ok = true;
					walk_children(expr, [&](Expr*& c) { ok = ok && self_calls_all_tail(c, name, false); });
					return ok;
				}
			}
		}

		struct Capture
		{
			std::string_view name;
			ResolvedBinding binding;
		};

		void collect_captures_in(Expr* expr, Expr* lambda, std::string_view self_name,
			std::vector<Capture>& out, std::unordered_set<uint64_t>& seen)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
				{
					if (expr->var_ref.name == self_name)
					{
						return;
					}
					ResolvedBinding b = db.binding(expr);
					if (b.lambda == lambda || b.lambda == db.toplevel_lambda_)
					{
						return;
					}
					uint64_t key = (static_cast<uint64_t>(b.lambda->id) << 32) | b.breadth;
					if (seen.insert(key).second)
					{
						out.push_back({expr->var_ref.name, b});
					}
					return;
				}
				case ExprKind::Lambda:
					// Do not descend into nested lambdas: their captures reach the
					// binding through the closure chain and stay captures.
					return;
				default:
					walk_children(expr, [&](Expr* c) {
						collect_captures_in(c, lambda, self_name, out, seen);
					});
					return;
			}
		}

		std::vector<Capture> collect_captures(Expr* lambda, std::string_view self_name)
		{
			std::vector<Capture> captures;
			std::unordered_set<uint64_t> seen;
			for (Expr* form : lambda->lambda.body)
			{
				collect_captures_in(form, lambda, self_name, captures, seen);
			}
			return captures;
		}

		Expr* make_resolved_ref(const Capture& cap, SourceLoc loc)
		{
			// Record the binding immediately so lifts of enclosing lets can resolve
			// this ref before resolve_bindings reruns.
			Expr* e = db.make_expr(ExprKind::VarRef, loc);
			e->var_ref.name = cap.name;
			get(db.bindings_, e->id) = cap.binding;
			return e;
		}

		void prepend_capture_args(Expr* expr, std::string_view name, const std::vector<Capture>& captures)
		{
			if (expr->kind == ExprKind::Call
				&& expr->call.proc->kind == ExprKind::VarRef
				&& expr->call.proc->var_ref.name == name)
			{
				uint32_t n = static_cast<uint32_t>(expr->call.args.size());
				Expr** new_args = db.arena.alloc_array<Expr*>(n + captures.size());
				for (uint32_t i = 0; i < captures.size(); ++i)
				{
					new_args[i] = make_resolved_ref(captures[i], expr->loc);
				}
				for (uint32_t i = 0; i < n; ++i)
				{
					new_args[captures.size() + i] = expr->call.args[i];
				}
				expr->call.args = {new_args, static_cast<uint32_t>(n + captures.size())};
				for (Expr* arg : expr->call.args)
				{
					prepend_capture_args(arg, name, captures);
				}
				return;
			}
			walk_children(expr, [&](Expr*& c) { prepend_capture_args(c, name, captures); });
		}

		Expr* try_lift(Expr* let_expr)
		{
			if (let_expr->kind != ExprKind::Let || let_expr->let.names.size() != 1)
			{
				return let_expr;
			}

			std::string_view name = let_expr->let.names[0];
			Expr* init_val = let_expr->let.vals[0];
			if (!is_false_lit(init_val))
			{
				return let_expr;
			}

			Expr* lambda = nullptr;
			size_t setbang_idx = 0;
			for (size_t i = 0; i < let_expr->let.body.size(); ++i)
			{
				Expr* form = let_expr->let.body[i];
				if (form->kind == ExprKind::SetBang
					&& form->set_bang.name == name
					&& form->set_bang.is_init
					&& form->set_bang.value->kind == ExprKind::Lambda)
				{
					if (lambda)
					{
						return let_expr;
					}
					lambda = form->set_bang.value;
					setbang_idx = i;
				}
			}
			if (!lambda)
			{
				return let_expr;
			}

			for (Expr* form : let_expr->let.body)
			{
				if (form->kind == ExprKind::SetBang
					&& form->set_bang.name == name
					&& !form->set_bang.is_init)
				{
					return let_expr;
				}
			}

			if (lambda->lambda.is_variadic)
			{
				return let_expr;
			}

			for (size_t i = setbang_idx + 1; i < let_expr->let.body.size(); ++i)
			{
				if (name_used_as_value(let_expr->let.body[i], name))
				{
					return let_expr;
				}
			}

			for (Expr* form : lambda->lambda.body)
			{
				if (name_used_as_value(form, name))
				{
					return let_expr;
				}
			}

			std::vector<Capture> captures = collect_captures(lambda, name);
			if (captures.empty())
			{
				return let_expr;
			}

			// A parameter is a copy: a capture whose binding is written after init must
			// stay a capture or writes through one copy are lost to the others.
			for (Capture& cap : captures)
			{
				Compiler::LambdaBindings& owner = db.lambda_bindings_[cap.binding.lambda];
				if (get(owner.reassigned_after_init, cap.binding.breadth))
				{
					return let_expr;
				}
			}

			if (!self_calls_all_tail(lambda, name, false))
			{
				return let_expr;
			}

			uint32_t n_captures = static_cast<uint32_t>(captures.size());
			uint32_t n_params = static_cast<uint32_t>(lambda->lambda.params.size());
			std::string_view* new_params = db.arena.alloc_array<std::string_view>(n_captures + n_params);
			for (uint32_t i = 0; i < n_captures; ++i)
			{
				new_params[i] = captures[i].name;
			}
			for (uint32_t i = 0; i < n_params; ++i)
			{
				new_params[n_captures + i] = lambda->lambda.params[i];
			}
			lambda->lambda.params = {new_params, n_captures + n_params};

			for (size_t i = setbang_idx + 1; i < let_expr->let.body.size(); ++i)
			{
				prepend_capture_args(let_expr->let.body[i], name, captures);
			}
			for (Expr* form : lambda->lambda.body)
			{
				prepend_capture_args(form, name, captures);
			}

			return let_expr;
		}

		Expr* walk(Expr* expr)
		{
			walk_children(expr, [&](Expr*& c) { c = walk(c); });
			if (expr->kind == ExprKind::Let)
			{
				return try_lift(expr);
			}
			return expr;
		}
	};

} // namespace

void Compiler::run_lambda_lift(Program& program)
{
	LambdaLift pass{.db = *this};
	for (Expr* form : program.forms)
	{
		pass.walk(form);
	}
}

namespace
{

	struct LirInst
	{
		Opcode op;   // base opcode only: no _1.._7 replicas; label is IR-only
		union
		{
			struct { uint16_t dst; uint16_t src; } mov;              // mov
			struct { uint16_t dst; uint16_t idx; } load;             // ldk ldu ldus ldd
			struct { uint16_t idx; uint16_t src; } store;            // stu std
			struct { uint16_t reg; } box;                            // box
			struct { uint16_t src; } ret;                            // retv
			struct { uint32_t id; uint16_t src; } label;             // label; if_false/skip target
			struct { uint32_t id; uint16_t a; uint16_t b; } if_cmp;  // if_eq..if_ltk; rk holds the pool idx in b
			// One payload for every call op: callw/tcall read callee, cs/cst
			// read upvalue_idx, cd/cdt read src+idx, recurw/applyw only w+nargs.
			struct { uint16_t w; uint16_t nargs; uint16_t callee; uint16_t upvalue_idx; uint16_t idx;
					 uint8_t src; } call;
			struct { uint16_t dst; uint16_t pool_idx; uint16_t first_capture; uint16_t n_captures; } closure;
			struct { uint16_t dst; uint16_t a; uint16_t b; } arith;  // rr; rk holds the pool idx in b
			struct { uint16_t dst; uint16_t obj; uint16_t key; uint16_t val; } field;  // ldf stf; *k holds the pool idx in key
		} u;
	};

	struct LirLambda
	{
		std::vector<LirInst> code;
		// make_closure payload, never code: closure insts hold (first, n)
		// into this sidecar.
		std::vector<OP_make_closure_capture> captures;
		bool is_variadic = false;
		uint32_t n_params = 0;
		// Bump allocator for temps and call windows; starts at the named-register
		// high water (static per lambda) and never shrinks.
		uint32_t n_regs = 0;
		uint16_t pool_slot = 0;   // unused for the toplevel (index 0)
		// Coalesced bindings: named register -> the call-window register the value
		// actually lives in. Write-once per register (the name frame never shrinks).
		std::unordered_map<uint16_t, uint16_t> reg_alias;
	};

	struct LirProgram
	{
		std::vector<LirLambda> lambdas;    // [0] = toplevel
		std::vector<std::string> pool;     // lambda entries stay empty until emit
		std::vector<int32_t> pool_to_lambda;  // pool slot -> lambdas index, -1 = constant
		uint32_t next_label = 0;
	};

	struct LirEmitter
	{
		Compiler& db;
		LirProgram& prog;
		std::vector<Expr*> outer_lambdas{db.toplevel_lambda_};
		std::vector<uint32_t> lambda_stack{0};

		// Lambda entries bypass the dedup: one pool slot per Lambda expr.
		std::unordered_map<std::string, uint16_t> pool_idx{};

		// Windows allocated ahead of their call site by argument sinking, keyed by
		// the Call expr id.
		std::unordered_map<uint32_t, uint16_t> call_windows{};

		// Compares reselected as fused branch ops: the let binding skips them and
		// the consuming If emits them, keyed by the If expr id.
		std::unordered_map<uint32_t, Expr*> fused_tests{};

		LirLambda& current_lambda()
		{
			// prog.lambdas reallocates as nested lambdas append; never hold the
			// returned reference across a recursive walk.
			return prog.lambdas[lambda_stack.back()];
		}

		static LirInst inst(Opcode op)
		{
			LirInst i{};
			i.op = op;
			return i;
		}

		void emit(const LirInst& i) { current_lambda().code.push_back(i); }

		uint16_t alloc_reg() { return alloc_window(1); }

		uint16_t alloc_window(size_t n)
		{
			// A window holds at least the result register: a nullary call still
			// writes its result at w.
			if (n == 0)
			{
				n = 1;
			}
			LirLambda& L = current_lambda();
			JET_DIE_WHEN(L.n_regs + n > UINT16_MAX, "codegen: frame exceeds %d registers", UINT16_MAX);
			uint16_t w = static_cast<uint16_t>(L.n_regs);
			L.n_regs += n;
			return w;
		}

		uint16_t alias(uint16_t r)
		{
			std::unordered_map<uint16_t, uint16_t>& m = current_lambda().reg_alias;
			std::unordered_map<uint16_t, uint16_t>::iterator it = m.find(r);
			return it == m.end() ? r : it->second;
		}

		// Find-or-allocate the window for a call site; argument sinking claims it
		// ahead of emit_call reaching the site.
		uint16_t claim_call_window(Expr* call_expr, size_t nargs)
		{
			std::unordered_map<uint32_t, uint16_t>::iterator it = call_windows.find(call_expr->id);
			if (it == call_windows.end())
			{
				it = call_windows.emplace(call_expr->id, alloc_window(nargs)).first;
			}
			return it->second;
		}

		void emit_mov(uint16_t dst, uint16_t src)
		{
			if (dst == src)
			{
				return;
			}
			LirInst i = inst(Opcode::mov);
			i.u.mov.dst = dst;
			i.u.mov.src = src;
			emit(i);
		}

		void emit_load(Opcode op, uint16_t dst, uint16_t idx)
		{
			LirInst i = inst(op);
			i.u.load.dst = dst;
			i.u.load.idx = idx;
			emit(i);
		}

		void emit_store(Opcode op, uint16_t idx, uint16_t src)
		{
			LirInst i = inst(op);
			i.u.store.idx = idx;
			i.u.store.src = src;
			emit(i);
		}

		void emit_box(uint16_t reg)
		{
			LirInst i = inst(Opcode::box);
			i.u.box.reg = reg;
			emit(i);
		}

		void emit_label(Opcode op, uint32_t id, uint16_t src = 0)
		{
			LirInst i = inst(op);
			i.u.label.id = id;
			i.u.label.src = src;
			emit(i);
		}

		void emit_ldk(uint16_t dst, uint16_t idx)
		{
			emit_load(Opcode::ldk, dst, idx);
		}

		uint16_t intern_constant(std::string& serialized)
		{
			auto it = pool_idx.find(serialized);
			if (it != pool_idx.end())
			{
				return it->second;
			}
			uint16_t idx = static_cast<uint16_t>(prog.pool.size());
			prog.pool.push_back(serialized);
			prog.pool_to_lambda.push_back(-1);
			pool_idx[serialized] = idx;
			return idx;
		}

		template <class T>
		uint16_t intern_typed(ConstTag t, T& payload)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			s.append(reinterpret_cast<char*>(&payload), sizeof(T));
			return intern_constant(s);
		}

		uint16_t intern_string(ConstTag t, std::string_view payload)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			s.append(payload.data(), payload.size());
			s.push_back(0);
			return intern_constant(s);
		}

		uint16_t intern_empty(ConstTag t)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			return intern_constant(s);
		}

		uint16_t intern_global_name(std::string_view name)
		{
			return intern_string(ConstTag::GlobalName, name);
		}

		uint16_t intern_literal_key(Expr* e)
		{
			switch (e->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = number_lit_value(e->number_lit.text);
					Number n = static_cast<Number>(val);
					return intern_typed(ConstTag::Number, n);
				}
				case ExprKind::SymbolLit:
					return intern_string(ConstTag::Symbol, e->symbol_lit.name);
				case ExprKind::CharacterLit:
				{
					Character c = static_cast<Character>(e->character_lit.value);
					return intern_typed(ConstTag::Character, c);
				}
				case ExprKind::BooleanLit:
				{
					bool v = e->boolean_lit.value;
					return intern_typed(ConstTag::Boolean, v);
				}
				case ExprKind::StringLit:
					return intern_string(ConstTag::String, e->string_lit.value);
				default:
					JET_DIE("intern_literal_key: not a literal Expr (kind %d)", static_cast<int>(e->kind));
			}
		}

		void emit_ret(uint16_t src)
		{
			LirInst i = inst(Opcode::retv);
			i.u.ret.src = src;
			emit(i);
		}

		uint16_t emit_sequence_value(Slice<Expr*>& forms)
		{
			uint32_t n = forms.size();
			if (n == 0)
			{
				uint16_t t = alloc_reg();
				emit_ldk(t, intern_empty(ConstTag::Unknown));
				return t;
			}
			for (uint32_t i = 0; i + 1 < n; ++i)
			{
				emit_to_any_reg(forms[i]);
			}
			return emit_to_any_reg(forms[n - 1]);
		}

		void emit_sequence_to(Slice<Expr*>& forms, uint16_t dst)
		{
			uint32_t n = forms.size();
			if (n == 0)
			{
				emit_ldk(dst, intern_empty(ConstTag::Unknown));
				return;
			}
			for (uint32_t i = 0; i + 1 < n; ++i)
			{
				emit_to_any_reg(forms[i]);
			}
			emit_to_reg(forms[n - 1], dst);
		}

		void emit_prologue(Expr* lambda)
		{
			Slice<bool>& captured = lambda->lambda.captured_locals;
			Slice<bool>& mutated = lambda->lambda.mutated_locals;
			for (uint32_t i = 0; i < captured.size(); ++i)
			{
				if (captured[i] && mutated[i])
				{
					emit_box(narrow_off(i));
				}
			}
		}

		uint16_t emit_lifted_lambda(Expr* expr)
		{
			uint32_t idx = static_cast<uint32_t>(prog.lambdas.size());
			prog.lambdas.emplace_back();
			prog.lambdas[idx].is_variadic = expr->lambda.is_variadic;
			prog.lambdas[idx].n_params = expr->lambda.params.size();
			// names is the named-register high water (the resolver's name frame
			// only grows); the variadic formula can exceed it by the rest slot.
			uint32_t n_named = static_cast<uint32_t>(
				expr->lambda.params.size() + (expr->lambda.is_variadic ? 1 : 0));
			uint32_t n_names = static_cast<uint32_t>(expr->lambda.names.size());
			prog.lambdas[idx].n_regs = n_named > n_names ? n_named : n_names;

			outer_lambdas.push_back(expr);
			lambda_stack.push_back(idx);
			emit_prologue(expr);
			uint16_t r = emit_sequence_value(expr->lambda.body);
			emit_ret(r);
			lambda_stack.pop_back();
			outer_lambdas.pop_back();

			// The slot must be reserved after the body completes: the body's
			// constants and nested lambda entries take lower pool indices.
			uint16_t slot = static_cast<uint16_t>(prog.pool.size());
			prog.pool.emplace_back();
			prog.pool_to_lambda.push_back(static_cast<int32_t>(idx));
			prog.lambdas[idx].pool_slot = slot;
			return slot;
		}

		void emit_capture_recipe(Expr* inner)
		{
			Expr* current = outer_lambdas.back();
			Slice<UpvalueRef>& ups = inner->lambda.upvalues;
			for (uint32_t i = 0; i < ups.size(); ++i)
			{
				UpvalueRef u = ups[i];
				ResolvedBinding rb{.lambda = u.owner, .breadth = u.breadth};
				OP_make_closure_capture cap{};
				if (rb.lambda == current)
				{
					cap.src = static_cast<uint8_t>(CaptureSource::Local);
					cap.idx = alias(static_cast<uint16_t>(rb.breadth));
				}
				else
				{
					std::optional<uint16_t> found = find_upvalue(current, rb.lambda, static_cast<uint32_t>(rb.breadth));
					JET_DIE_UNLESS(found, "codegen: upvalue not in parent's upvalue list");
					cap.src = static_cast<uint8_t>(CaptureSource::Upvalue);
					cap.idx = *found;
				}
				current_lambda().captures.push_back(cap);
			}
		}

		void emit_lambda_value(Expr* expr, uint16_t dst)
		{
			uint16_t pool_index = emit_lifted_lambda(expr);
			if (expr->lambda.upvalues.empty())
			{
				emit_ldk(dst, pool_index);
				return;
			}
			LirInst i = inst(Opcode::clos);
			i.u.closure.dst = dst;
			i.u.closure.pool_idx = pool_index;
			i.u.closure.first_capture = static_cast<uint16_t>(current_lambda().captures.size());
			i.u.closure.n_captures = static_cast<uint16_t>(expr->lambda.upvalues.size());
			emit_capture_recipe(expr);
			emit(i);
		}

		Compiler::OpSelection selection(Expr* expr, const char* what)
		{
			JET_DIE_WHEN(!db.selected_ops_[expr->id], "%d:%d: codegen: %s without selection",
						  expr->loc.line, expr->loc.col, what);
			Compiler::OpSelection sel = *db.selected_ops_[expr->id];
			// Coalesced homes resolve here, the single read point for selections;
			// only these two payloads name an unboxed local register (for ldu/stu/
			// ldus the same field holds an upvalue index).
			if (sel.op == Opcode::mov)
			{
				sel.u.var.addr = alias(sel.u.var.addr);
			}
			else if (sel.op == Opcode::cd_0 && sel.u.call_ic_direct.src == 0)
			{
				sel.u.call_ic_direct.idx = alias(sel.u.call_ic_direct.idx);
			}
			return sel;
		}

		uint16_t emit_set_bang(Expr* expr)
		{
			Compiler::OpSelection sel = selection(expr, "set!");
			switch (sel.op)
			{
				case Opcode::mov:
				{
					emit_to_reg(expr->set_bang.value, sel.u.var.addr);
					return sel.u.var.addr;
				}
				case Opcode::std:
				case Opcode::stu:
				{
					uint16_t v = emit_to_any_reg(expr->set_bang.value);
					emit_store(sel.op, sel.u.var.addr, v);
					return v;
				}
				default:
					JET_DIE("%d:%d: codegen: unexpected set! selection %d",
							 expr->loc.line, expr->loc.col, static_cast<int>(sel.op));
			}
		}

		uint16_t emit_set_ref(Expr* expr)
		{
			Compiler::OpSelection sel = selection(expr, "SetRef");
			LirInst i = inst(sel.op);
			i.u.field.obj = emit_to_any_reg(expr->set_ref.obj);
			i.u.field.key = sel.op == Opcode::stfk ? intern_literal_key(expr->set_ref.key)
												   : emit_to_any_reg(expr->set_ref.key);
			uint16_t v = emit_to_any_reg(expr->set_ref.value);
			i.u.field.val = v;
			emit(i);
			return v;
		}

		void emit_field_get(Compiler::OpSelection sel, Expr* receiver, Expr* key, uint16_t dst)
		{
			LirInst i = inst(sel.op);
			i.u.field.dst = dst;
			i.u.field.obj = emit_to_any_reg(receiver);
			i.u.field.key = sel.op == Opcode::ldfk ? intern_literal_key(key) : emit_to_any_reg(key);
			emit(i);
		}

		void emit_window_args(Slice<Expr*>& args, uint16_t w)
		{
			for (uint32_t i = 0; i < args.size(); ++i)
			{
				emit_to_reg(args[i], static_cast<uint16_t>(w + i));
			}
		}

		bool is_plain_reg_ref(Expr* e, uint16_t r)
		{
			if (e->kind != ExprKind::VarRef)
			{
				return false;
			}
			Compiler::OpSelection sel = selection(e, "var access");
			return sel.op == Opcode::mov && sel.u.var.addr == r;
		}

		std::optional<uint16_t> window_slot(Expr* e, uint16_t home)
		{
			if (e->kind != ExprKind::Call || !is_call_shaped(selection(e, "Call").op))
			{
				return std::nullopt;
			}
			for (uint32_t i = 0; i < e->call.args.size(); ++i)
			{
				if (is_plain_reg_ref(e->call.args[i], home))
				{
					return static_cast<uint16_t>(claim_call_window(e, e->call.args.size()) + i);
				}
			}
			return std::nullopt;
		}

		void emit_let_bindings(Expr* expr)
		{
			uint32_t sb = expr->let.slot_base;
			uint32_t n = expr->let.names.size();
			for (uint32_t i = 0; i < n; ++i)
			{
				uint16_t home = narrow_off(sb + i);
				Expr* val = expr->let.vals[i];
				if (needs_slot(expr->let.owner, sb + i))
				{
					emit_to_reg(val, home);
					continue;
				}
				// ANF nests hoists, so a value is often a let-chain around the
				// expression that produces it: emit the inner bindings here and
				// coalesce home with the terminal itself.
				while (val->kind == ExprKind::Let && val->let.body.size() == 1)
				{
					emit_let_bindings(val);
					val = val->let.body[0];
				}
				if (val->kind == ExprKind::Call)
				{
					Compiler::OpSelection sel = selection(val, "Call");
					if (is_if_cmp(sel.op))
					{
						fused_tests[expr->let.body[0]->id] = val;
						continue;
					}
					if (is_call_shaped(sel.op) && sel.op != Opcode::recurw)
					{
						current_lambda().reg_alias[home] = emit_call(val, sel);
						continue;
					}
				}
				if (is_anf_temp(expr->let.names[i]))
				{
					std::optional<uint16_t> target = sink_target(expr, home);
					if (target)
					{
						emit_to_reg(val, *target);
						current_lambda().reg_alias[home] = *target;
						continue;
					}
				}
				emit_to_reg(val, home);
			}
			for (uint32_t i = 0; i < n; ++i)
			{
				if (needs_slot(expr->let.owner, sb + i))
				{
					emit_box(narrow_off(sb + i));
				}
			}
		}

		// A single-use temp consumed as a window-call argument is computed straight
		// into the argument register; the consumer's window is allocated early so
		// the register is known at the temp's definition. Safe because everything
		// emitted between the definition and the call writes only registers above
		// its own (later-allocated, hence higher) window.
		std::optional<uint16_t> sink_target(Expr* let_expr, uint16_t home)
		{
			Expr* cur = let_expr;
			while (cur->let.body.size() == 1)
			{
				cur = cur->let.body[0];
				if (cur->kind != ExprKind::Let)
				{
					return window_slot(cur, home);
				}
				for (uint32_t i = 0; i < cur->let.vals.size(); ++i)
				{
					std::optional<uint16_t> w = window_slot(cur->let.vals[i], home);
					if (w)
					{
						return w;
					}
				}
			}
			return std::nullopt;
		}

		uint16_t emit_call(Expr* expr, Compiler::OpSelection sel)
		{
			bool tail = db.is_tail(expr);
			uint16_t nargs = static_cast<uint16_t>(expr->call.args.size());

			LirInst i = inst(sel.op);
			switch (sel.op)
			{
				case Opcode::recurw:
					break;
				case Opcode::cs_0:
					i.op = tail ? Opcode::cst_0 : Opcode::cs_0;
					i.u.call.upvalue_idx = sel.u.call_ic_slot.upvalue_idx;
					break;
				case Opcode::cd_0:
					i.op = tail ? Opcode::cdt_0 : Opcode::cd_0;
					i.u.call.src = sel.u.call_ic_direct.src;
					i.u.call.idx = sel.u.call_ic_direct.idx;
					break;
				case Opcode::callw:
					i.op = tail ? Opcode::tcall : Opcode::callw;
					i.u.call.callee = emit_to_any_reg(expr->call.proc);
					break;
				default:
					JET_DIE("%d:%d: codegen: unexpected Call selection %d",
							 expr->loc.line, expr->loc.col, static_cast<int>(sel.op));
			}

			uint16_t w = claim_call_window(expr, nargs);
			emit_window_args(expr->call.args, w);
			i.u.call.w = w;
			i.u.call.nargs = nargs;
			emit(i);
			return w;
		}

		uint16_t emit_apply(Expr* expr)
		{
			uint16_t w = alloc_window(2);
			emit_to_reg(expr->apply.proc, w);
			emit_to_reg(expr->apply.args, static_cast<uint16_t>(w + 1));
			LirInst i = inst(Opcode::applyw);
			i.u.call.w = w;
			emit(i);
			return w;
		}

		void emit_program(Program& program)
		{
			prog.lambdas.emplace_back();
			prog.lambdas[0].n_regs = static_cast<uint32_t>(db.toplevel_lambda_->lambda.names.size());
			emit_prologue(db.toplevel_lambda_);
			uint16_t r = emit_sequence_value(program.forms);
			emit_ret(r);
		}

		bool is_call_shaped(Opcode op)
		{
			switch (op)
			{
				case Opcode::callw:
				case Opcode::cs_0:
				case Opcode::cd_0:
				case Opcode::recurw:
					return true;
				default:
					return false;
			}
		}

		bool is_if_cmp(Opcode op)
		{
			switch (op)
			{
				case Opcode::if_eq:
				case Opcode::if_lt:
				case Opcode::if_le:
				case Opcode::if_gt:
				case Opcode::if_ge:
				case Opcode::if_eqk:
				case Opcode::if_ltk:
					return true;
				default:
					return false;
			}
		}

		uint16_t emit_to_any_reg(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
				{
					Compiler::OpSelection sel = selection(expr, "var access");
					if (sel.op == Opcode::mov)
					{
						return sel.u.var.addr;
					}
					break;
				}

				case ExprKind::Call:
				{
					Compiler::OpSelection sel = selection(expr, "Call");
					if (is_call_shaped(sel.op))
					{
						return emit_call(expr, sel);
					}
					break;
				}

				case ExprKind::Apply:
					return emit_apply(expr);

				case ExprKind::SetBang:
					return emit_set_bang(expr);

				case ExprKind::SetRef:
					return emit_set_ref(expr);

				default:
					break;
			}
			uint16_t t = alloc_reg();
			emit_to_reg(expr, t);
			return t;
		}

		void emit_to_reg(Expr* expr, uint16_t dst)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = number_lit_value(expr->number_lit.text);
					Number n = static_cast<Number>(val);
					emit_ldk(dst, intern_typed(ConstTag::Number, n));
					break;
				}

				case ExprKind::BooleanLit:
				{
					bool v = expr->boolean_lit.value;
					emit_ldk(dst, intern_typed(ConstTag::Boolean, v));
					break;
				}

				case ExprKind::CharacterLit:
				{
					Character c = static_cast<Character>(expr->character_lit.value);
					emit_ldk(dst, intern_typed(ConstTag::Character, c));
					break;
				}

				case ExprKind::StringLit:
					emit_ldk(dst, intern_string(ConstTag::String, expr->string_lit.value));
					break;

				case ExprKind::SymbolLit:
					emit_ldk(dst, intern_string(ConstTag::Symbol, expr->symbol_lit.name));
					break;

				case ExprKind::UnknownLit:
					emit_ldk(dst, intern_empty(ConstTag::Unknown));
					break;

				case ExprKind::PrimRef:
					emit_ldk(dst, intern_global_name(expr->prim_ref.name));
					break;

				case ExprKind::VarRef:
				{
					Compiler::OpSelection sel = selection(expr, "var access");
					switch (sel.op)
					{
						case Opcode::mov:
							emit_mov(dst, sel.u.var.addr);
							break;
						case Opcode::ldd:
						case Opcode::ldu:
						case Opcode::ldus:
							emit_load(sel.op, dst, sel.u.var.addr);
							break;
						default:
							JET_DIE("%d:%d: codegen: unexpected var selection %d",
									 expr->loc.line, expr->loc.col, static_cast<int>(sel.op));
					}
					break;
				}

				case ExprKind::Call:
				{
					Compiler::OpSelection sel = selection(expr, "Call");
					switch (sel.op)
					{
						case Opcode::add:
						case Opcode::sub:
						case Opcode::mul:
						case Opcode::div:
						case Opcode::eq:
						case Opcode::lt:
						case Opcode::le:
						case Opcode::gt:
						case Opcode::ge:
						case Opcode::addk:
						case Opcode::subk:
						case Opcode::mulk:
						case Opcode::divk:
						case Opcode::eqk:
						case Opcode::ltk:
						{
							bool k = sel.op == Opcode::addk || sel.op == Opcode::subk
									 || sel.op == Opcode::mulk || sel.op == Opcode::divk
									 || sel.op == Opcode::eqk || sel.op == Opcode::ltk;
							LirInst i = inst(sel.op);
							i.u.arith.dst = dst;
							i.u.arith.a = emit_to_any_reg(expr->call.args[0]);
							i.u.arith.b = k ? intern_literal_key(expr->call.args[1])
											: emit_to_any_reg(expr->call.args[1]);
							emit(i);
							break;
						}

						case Opcode::ldf:
						case Opcode::ldfk:
							emit_field_get(sel, expr->call.args[0], expr->call.args[1], dst);
							break;

						default:
							emit_mov(dst, emit_call(expr, sel));
							break;
					}
					break;
				}

				case ExprKind::Apply:
					emit_mov(dst, emit_apply(expr));
					break;

				case ExprKind::Lambda:
					emit_lambda_value(expr, dst);
					break;

				case ExprKind::SetBang:
					emit_mov(dst, emit_set_bang(expr));
					break;

				case ExprKind::SetRef:
					emit_mov(dst, emit_set_ref(expr));
					break;

				case ExprKind::Let:
					emit_let_bindings(expr);
					emit_sequence_to(expr->let.body, dst);
					break;

				case ExprKind::If:
				{
					uint32_t l_alt = prog.next_label++;
					uint32_t l_end = prog.next_label++;
					std::unordered_map<uint32_t, Expr*>::iterator fused = fused_tests.find(expr->id);
					if (fused != fused_tests.end())
					{
						Expr* cmp = fused->second;
						Compiler::OpSelection sel = selection(cmp, "fused test");
						LirInst i = inst(sel.op);
						i.u.if_cmp.id = l_alt;
						i.u.if_cmp.a = emit_to_any_reg(cmp->call.args[0]);
						i.u.if_cmp.b = sel.op == Opcode::if_eqk || sel.op == Opcode::if_ltk
										   ? intern_literal_key(cmp->call.args[1])
										   : emit_to_any_reg(cmp->call.args[1]);
						emit(i);
					}
					else
					{
						uint16_t test = emit_to_any_reg(expr->if_.test);
						emit_label(Opcode::if_false, l_alt, test);
					}
					emit_to_reg(expr->if_.consequent, dst);
					emit_label(Opcode::skip, l_end);
					emit_label(Opcode::label, l_alt);
					if (expr->if_.alternate)
					{
						emit_to_reg(expr->if_.alternate, dst);
					}
					else
					{
						emit_ldk(dst, intern_empty(ConstTag::Unknown));
					}
					emit_label(Opcode::label, l_end);
					break;
				}

				default:
					JET_DIE("%d:%d: codegen: unhandled ExprKind %d (not ANF?)",
							 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
			}
		}
	};

	struct BytecodeEmitter
	{
		LirProgram& prog;

		// Rotate among JET_REPLICATE_N variants so distinct call sites
		// land on distinct asm dispatch tails (Ertl & Gregg 2003).
		size_t v_cs = 0;
		size_t v_cst = 0;
		size_t v_cd = 0;
		size_t v_cdt = 0;

		static size_t encoded_size(const LirInst& i)
		{
			if (i.op == Opcode::label)
			{
				return 0;
			}
			if (i.op == Opcode::clos)
			{
				return OPCODE_SIZE + sizeof(OP_clos) +
					   i.u.closure.n_captures * sizeof(OP_make_closure_capture);
			}
			return opcode_step(static_cast<uint8_t>(i.op), nullptr);
		}

		void emit_raw(Bytecode& bc, const void* data, size_t size)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			bc.insert(bc.end(), p, p + size);
		}

		template <class T>
		void emit_operand(Bytecode& bc, T& val)
		{
			emit_raw(bc, &val, sizeof(T));
		}

		void emit_opcode(Bytecode& bc, Opcode op)
		{
			size_t at = bc.size();
			bc.resize(at + OPCODE_SIZE);
			bc[at + VM_OP_SLOT_SIZE] = static_cast<uint8_t>(op);
		}

		void emit_replicated(Bytecode& bc, Opcode base, size_t& counter)
		{
			int offset = static_cast<int>(counter++ % JET_REPLICATE_N);
			emit_opcode(bc, static_cast<Opcode>(static_cast<int>(base) + offset));
		}

		size_t label_target(std::unordered_map<uint32_t, size_t>& label_pos, uint32_t id)
		{
			auto it = label_pos.find(id);
			JET_DIE_WHEN(it == label_pos.end(), "lir emit: unresolved label %u", id);
			return it->second;
		}

		void fill_lambda_entry(uint16_t slot)
		{
			// Pool entry: [tag=Lambda][is_n_ary][n_params if !is_n_ary][n_regs][code_size][bytes...]
			LirLambda& L = prog.lambdas[static_cast<uint32_t>(prog.pool_to_lambda[slot])];
			Bytecode body = emit_code(L);

			std::string entry;
			entry.push_back(static_cast<char>(ConstTag::Lambda));
			bool is_n_ary = L.is_variadic;
			entry.append(reinterpret_cast<char*>(&is_n_ary), sizeof(is_n_ary));
			if (!is_n_ary)
			{
				size_t n = static_cast<size_t>(L.n_params);
				entry.append(reinterpret_cast<char*>(&n), sizeof(n));
			}
			uint16_t n_regs = static_cast<uint16_t>(L.n_regs);
			entry.append(reinterpret_cast<char*>(&n_regs), sizeof(n_regs));
			size_t code_size = body.size();
			entry.append(reinterpret_cast<char*>(&code_size), sizeof(code_size));
			entry.append(reinterpret_cast<char*>(body.data()), code_size);
			prog.pool[slot] = std::move(entry);
		}

		Bytecode emit_code(LirLambda& L)
		{
			std::unordered_map<uint32_t, size_t> label_pos;
			size_t off = 0;
			for (LirInst& i : L.code)
			{
				if (i.op == Opcode::label)
				{
					label_pos[i.u.label.id] = off;
				}
				else
				{
					off += encoded_size(i);
				}
			}

			Bytecode bc;
			for (LirInst& i : L.code)
			{
				emit_inst(bc, L, i, label_pos);
			}
			return bc;
		}

		void emit_inst(Bytecode& bc, LirLambda& L, LirInst& i,
					   std::unordered_map<uint32_t, size_t>& label_pos)
		{
			switch (i.op)
			{
				case Opcode::label:
					break;

				case Opcode::mov:
				{
					emit_opcode(bc, Opcode::mov);
					OP_mov op{};
					op.dst = i.u.mov.dst;
					op.src = i.u.mov.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldk:
				case Opcode::ldu:
				case Opcode::ldus:
				case Opcode::ldd:
				{
					emit_opcode(bc, i.op);
					OP_ldk op{};
					op.dst = i.u.load.dst;
					op.idx = i.u.load.idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stu:
				case Opcode::std:
				{
					emit_opcode(bc, i.op);
					OP_stu op{};
					op.idx = i.u.store.idx;
					op.src = i.u.store.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::box:
				{
					emit_opcode(bc, Opcode::box);
					OP_box op{};
					op.reg = i.u.box.reg;
					emit_operand(bc, op);
					break;
				}

				case Opcode::clos:
				{
					emit_opcode(bc, Opcode::clos);
					OP_clos op{};
					op.dst = i.u.closure.dst;
					op.pool_idx = i.u.closure.pool_idx;
					op.n_captures = i.u.closure.n_captures;
					emit_operand(bc, op);
					for (uint16_t c = 0; c < i.u.closure.n_captures; ++c)
					{
						emit_operand(bc, L.captures[static_cast<size_t>(i.u.closure.first_capture) + c]);
					}
					break;
				}

				case Opcode::add:
				case Opcode::sub:
				case Opcode::mul:
				case Opcode::div:
				case Opcode::eq:
				case Opcode::lt:
				case Opcode::le:
				case Opcode::gt:
				case Opcode::ge:
				case Opcode::addk:
				case Opcode::subk:
				case Opcode::mulk:
				case Opcode::divk:
				case Opcode::eqk:
				case Opcode::ltk:
				{
					emit_opcode(bc, i.op);
					OP_binop_rr op{};
					op.dst = i.u.arith.dst;
					op.a = i.u.arith.a;
					op.b = i.u.arith.b;
					emit_operand(bc, op);
					break;
				}

				case Opcode::if_false:
				{
					emit_opcode(bc, Opcode::if_false);
					OP_if_false op{};
					op.src = i.u.label.src;
					op.size = static_cast<uint32_t>(
						label_target(label_pos, i.u.label.id) - (bc.size() + sizeof(OP_if_false)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::if_eq:
				case Opcode::if_lt:
				case Opcode::if_le:
				case Opcode::if_gt:
				case Opcode::if_ge:
				case Opcode::if_eqk:
				case Opcode::if_ltk:
				{
					emit_opcode(bc, i.op);
					OP_if_cmp op{};
					op.a = i.u.if_cmp.a;
					op.b = i.u.if_cmp.b;
					op.size = static_cast<uint32_t>(
						label_target(label_pos, i.u.if_cmp.id) - (bc.size() + sizeof(OP_if_cmp)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::skip:
				{
					emit_opcode(bc, Opcode::skip);
					OP_skip op{};
					op.size = label_target(label_pos, i.u.label.id) - (bc.size() + sizeof(OP_skip));
					emit_operand(bc, op);
					break;
				}

				case Opcode::retv:
				{
					emit_opcode(bc, Opcode::retv);
					OP_retv op{};
					op.src = i.u.ret.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::callw:
				case Opcode::tcall:
				{
					emit_opcode(bc, i.op);
					OP_callw op{};
					op.w = i.u.call.w;
					op.callee = i.u.call.callee;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::recurw:
				{
					emit_opcode(bc, Opcode::recurw);
					OP_recurw op{};
					op.w = i.u.call.w;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::applyw:
				{
					emit_opcode(bc, Opcode::applyw);
					OP_applyw op{};
					op.w = i.u.call.w;
					emit_operand(bc, op);
					break;
				}

				case Opcode::cs_0:
				case Opcode::cst_0:
				{
					emit_replicated(bc, i.op, i.op == Opcode::cst_0 ? v_cst : v_cs);
					OP_cs op{};
					op.w = i.u.call.w;
					op.upvalue_idx = i.u.call.upvalue_idx;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::cd_0:
				case Opcode::cdt_0:
				{
					emit_replicated(bc, i.op, i.op == Opcode::cdt_0 ? v_cdt : v_cd);
					OP_cd op{};
					op.w = i.u.call.w;
					op.idx = i.u.call.idx;
					op.nargs = i.u.call.nargs;
					op.src = i.u.call.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldf:
				{
					emit_opcode(bc, Opcode::ldf);
					OP_ldf op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stf:
				{
					emit_opcode(bc, Opcode::stf);
					OP_stf op{};
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					op.val = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfk:
				{
					emit_opcode(bc, Opcode::ldfk);
					OP_ldfk op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stfk:
				{
					emit_opcode(bc, Opcode::stfk);
					OP_stfk op{};
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					op.val = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				default:
					JET_DIE("lir emit: unexpected opcode %d", static_cast<int>(i.op));
			}
		}

		Bytecode emit()
		{
			Bytecode code = emit_code(prog.lambdas[0]);
			for (size_t slot = 0; slot < prog.pool.size(); ++slot)
			{
				if (prog.pool_to_lambda[slot] >= 0)
				{
					fill_lambda_entry(static_cast<uint16_t>(slot));
				}
			}

			// [u32 n_toplevel_slots][u32 n_pool_entries][concatenated pool entries][code...]
			Bytecode out;
			uint32_t n_slots = prog.lambdas[0].n_regs;
			uint8_t* sp = reinterpret_cast<uint8_t*>(&n_slots);
			out.insert(out.end(), sp, sp + sizeof(n_slots));
			uint32_t n = static_cast<uint32_t>(prog.pool.size());
			uint8_t* np = reinterpret_cast<uint8_t*>(&n);
			out.insert(out.end(), np, np + sizeof(n));
			for (std::string& entry : prog.pool)
			{
				out.insert(out.end(), entry.begin(), entry.end());
			}
			out.insert(out.end(), code.begin(), code.end());
			return out;
		}
	};

} // namespace

Bytecode Compiler::compile()
{
	Program& program = ast();

	LirProgram lir;
	LirEmitter emitter{*this, lir};
	emitter.emit_program(program);

	return BytecodeEmitter{lir}.emit();
}

Bytecode compile(std::string source, std::string filename, CompileFlags flags)
{
	Compiler compiler;
	compiler.source = std::move(source);
	compiler.filename = std::move(filename);
	compiler.flags_ = flags;
	return compiler.compile();
}

namespace
{

	Atom datum_to_atom(Expr* e)
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			{
				double v = number_lit_value(e->number_lit.text);
				return box<Number>(v);
			}
			case ExprKind::StringLit:
				return box<String>(std::string{e->string_lit.value});
			case ExprKind::BooleanLit:
				return box(e->boolean_lit.value);
			case ExprKind::CharacterLit:
				return box(static_cast<Character>(e->character_lit.value));
			case ExprKind::SymbolLit:
				return box(Symbol{std::string{e->symbol_lit.name}});
			case ExprKind::Call:
			{
				Expr* proc = e->call.proc;
				JET_DIE_UNLESS(proc->kind == ExprKind::VarRef, "datum_to_atom: bad call proc");
				std::string_view name = proc->var_ref.name;
				if (name == "list")
				{
					Atom result = box(EmptyList{});
					for (size_t i = e->call.args.size(); i-- > 0;)
					{
						result = cons(datum_to_atom(e->call.args[i]), result);
					}
					return result;
				}
				if (name == "cons")
				{
					JET_DIE_UNLESS(e->call.args.size() == 2, "datum_to_atom: cons arity");
					return cons(datum_to_atom(e->call.args[0]), datum_to_atom(e->call.args[1]));
				}
				if (name == "vector")
				{
					Vec v;
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						v.push_back(datum_to_atom(e->call.args[i]));
					}
					return box(std::move(v));
				}
				JET_DIE("datum_to_atom: unexpected call proc");
			}
			default:
				JET_DIE("datum_to_atom: unexpected ExprKind %d", static_cast<int>(e->kind));
		}
	}

	Atom read_port(Atom p)
	{
		IPort* port = slow_unbox<IPort>(p);
		Arena arena;
		LexState lex{port, arena, 0};
		lex.read_mode = true;
		std::vector<std::string> file_table;
		uint32_t next_id = 0;
		ParseState parser{.tokens = {},
						  .stream_lex = &lex,
						  .arena = arena,
						  .file_table = file_table,
						  .next_id = next_id};
		if (parser.at_end())
		{
			return make_eof();
		}
		Expr* datum = parser.parse_datum();
		return datum_to_atom(datum);
	}

} // namespace

void init_reader(Env& e)
{
	e.bind("read", make_prim<read_port>());
}
