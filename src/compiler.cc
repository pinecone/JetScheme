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
	std::optional<ResolvedBinding> lookup_name(std::string_view name);
	void push_lambda_scope(Expr* lambda);
	void pop_lambda_scope();
	bool prim_binding_lowerable(ResolvedBinding b, std::string_view prim);
	void record_ref(ResolvedBinding b);
	void record_set(ResolvedBinding b, bool is_init, Expr* value);
	void resolve_bindings(Program& program);
	void compute_binding_addresses(Program& program);
	void compute_binding_addresses_in(Expr* expr);
	void compute_lambda_bindings(Program& program);
	void compute_lambda_bindings_in(Expr* expr);
	void freeze_lambda(Expr* lambda);
	void compute_tail(Expr* expr, bool in_tail);

	struct OpSelection
	{
		Opcode op;
		union
		{
			struct { uint16_t addr; } field;   // local off / upvalue idx of addressed field forms
			struct { uint16_t addr; } var;     // local off / upvalue idx of ref/set
			struct { uint16_t upvalue_idx; uint16_t local_off; } call_ic_slot;
			struct { uint8_t src; uint16_t idx; } call_ic_direct;
		} u;
	};
	std::vector<std::optional<OpSelection>> selected_ops_;
	void run_select_ops(Program& program);
	void select_ops_in(Expr* expr, Expr* current);
	void select_call_op(Expr* expr, Expr* current);
	void select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, bool is_set);
	void select_var_op(Expr* expr, Expr* current, bool is_set);

	void run_anf_inline(Program& program);
	void run_stackify(Program& program);

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
		resolve_bindings(*ast_);
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
			expr->call.proc = expand(expr->call.proc);
			for (uint32_t i = 0; i < expr->call.args.size(); ++i)
			{
				expr->call.args[i] = expand(expr->call.args[i]);
			}
			return expr;
		case ExprKind::Apply:
			expr->apply.proc = expand(expr->apply.proc);
			expr->apply.args = expand(expr->apply.args);
			return expr;
		case ExprKind::Lambda:
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				expr->lambda.body[i] = expand(expr->lambda.body[i]);
			}
			expr->lambda.body = hoist_defines_in_body(expr->lambda.body, expr->loc);
			return expr;
		case ExprKind::Define:
			expr->define.value = expand(expr->define.value);
			return expr;
		case ExprKind::SetBang:
			expr->set_bang.value = expand(expr->set_bang.value);
			return expr;
		case ExprKind::SetRef:
			expr->set_ref.obj = expand(expr->set_ref.obj);
			expr->set_ref.key = expand(expr->set_ref.key);
			expr->set_ref.value = expand(expr->set_ref.value);
			return expr;
		case ExprKind::If:
			expr->if_.test = expand(expr->if_.test);
			expr->if_.consequent = expand(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				expr->if_.alternate = expand(expr->if_.alternate);
			}
			return expr;
		default:
			return expr;
	}
}

Expr* Compiler::expand_let(Expr* expr)
{
	for (uint32_t i = 0; i < expr->let.vals.size(); ++i)
	{
		expr->let.vals[i] = expand(expr->let.vals[i]);
	}
	for (uint32_t i = 0; i < expr->let.body.size(); ++i)
	{
		expr->let.body[i] = expand(expr->let.body[i]);
	}
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
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				expr->lambda.body[i] = compute_anf(expr->lambda.body[i]);
			}
			return expr;

		case ExprKind::Let:
			for (uint32_t i = 0; i < expr->let.vals.size(); ++i)
			{
				expr->let.vals[i] = compute_anf(expr->let.vals[i]);
			}
			for (uint32_t i = 0; i < expr->let.body.size(); ++i)
			{
				expr->let.body[i] = compute_anf(expr->let.body[i]);
			}
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
			for (Expr* arg : expr->call.args)
			{
				f(arg);
			}
			break;

		case ExprKind::Apply:
			f(expr->apply.proc);
			f(expr->apply.args);
			break;

		case ExprKind::Lambda:
			for (Expr* form : expr->lambda.body)
			{
				f(form);
			}
			break;

		case ExprKind::Let:
			for (Expr* val : expr->let.vals)
			{
				f(val);
			}
			for (Expr* form : expr->let.body)
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
	// Codegen wires each transit lambda's make_closure to forward the Slot, so
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

void Compiler::compute_lambda_bindings(Program& program)
{
	lambda_bindings_.clear();
	all_lambdas_.clear();

	push_lambda_scope(toplevel_lambda_);
	for (Expr* form : program.forms)
	{
		compute_lambda_bindings_in(form);
	}
	pop_lambda_scope();
	all_lambdas_.push_back(toplevel_lambda_);
}

void Compiler::resolve_bindings(Program& program)
{
	toplevel_lambda_ = make_expr(ExprKind::Lambda, {});
	toplevel_lambda_->lambda.params = {};
	toplevel_lambda_->lambda.is_variadic = false;
	toplevel_lambda_->lambda.body = {};
	toplevel_lambda_->lambda.names = arena.copy_slice(toplevel_names_.ordered);

	compute_binding_addresses(program);
	compute_lambda_bindings(program);

	if (flags_.inlining)
	{
		run_anf_inline(program);
		compute_lambda_bindings(program);
	}

	if (flags_.stackify)
	{
		run_stackify(program);
	}

	for (Expr* L : all_lambdas_)
	{
		freeze_lambda(L);
	}

	tail_cache_.assign(next_expr_id_ + 1, false);
	for (Expr* form : program.forms)
	{
		compute_tail(form, false);
	}

	run_select_ops(program);
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

void Compiler::compute_lambda_bindings_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
			record_ref(bindings_[expr->id]);
			break;

		case ExprKind::Lambda:
			push_lambda_scope(expr);
			walk_children(expr, [&](Expr* e) { compute_lambda_bindings_in(e); });
			pop_lambda_scope();
			all_lambdas_.push_back(expr);
			break;

		case ExprKind::SetBang:
		{
			compute_lambda_bindings_in(expr->set_bang.value);
			ResolvedBinding b = bindings_[expr->id];
			record_ref(b);
			record_set(b, expr->set_bang.is_init, expr->set_bang.value);
			break;
		}

		default:
			walk_children(expr, [&](Expr* e) { compute_lambda_bindings_in(e); });
			break;
	}
}

void Compiler::compute_tail(Expr* expr, bool in_tail)
{
	switch (expr->kind)
	{
		case ExprKind::Call:
			if (in_tail)
			{
				tail_cache_[expr->id] = true;
			}
			walk_children(expr, [&](Expr* e) { compute_tail(e, false); });
			break;

		case ExprKind::Lambda:
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				bool is_last = (i == expr->lambda.body.size() - 1);
				compute_tail(expr->lambda.body[i], is_last);
			}
			break;

		case ExprKind::If:
			compute_tail(expr->if_.test, false);
			compute_tail(expr->if_.consequent, in_tail);
			if (expr->if_.alternate)
			{
				compute_tail(expr->if_.alternate, in_tail);
			}
			break;

		case ExprKind::Let:
			for (Expr* val : expr->let.vals)
			{
				compute_tail(val, false);
			}
			for (uint32_t i = 0; i < expr->let.body.size(); ++i)
			{
				bool is_last = (i == expr->let.body.size() - 1);
				compute_tail(expr->let.body[i], is_last && in_tail);
			}
			break;

		default:
			walk_children(expr, [&](Expr* e) { compute_tail(e, false); });
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

} // namespace

void Compiler::run_select_ops(Program& program)
{
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
			walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
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

		default:
			walk_children(expr, [&](Expr* e) { select_ops_in(e, current); });
			break;
	}
}

void Compiler::select_call_op(Expr* expr, Expr* current)
{
	Expr* proc = expr->call.proc;
	OpSelection& sel = selected_ops_[expr->id].emplace();
	sel.op = Opcode::call;

	if (!flags_.specialize_ops)
	{
		return;
	}

	if (proc->kind != ExprKind::VarRef)
	{
		return;
	}

	ResolvedBinding proc_binding = binding(proc);

	// Self-tail-call: recur.
	if (is_tail(expr)
		&& proc_binding.lambda == toplevel_lambda_
		&& current != toplevel_lambda_
		&& !current->lambda.is_variadic
		&& current->lambda.params.size() == expr->call.args.size())
	{
		LambdaBindings& tl = lambda_bindings_[toplevel_lambda_];
		if (!get(tl.reassigned_after_init, proc_binding.breadth)
			&& get(tl.bound_init, proc_binding.breadth) == current)
		{
			sel.op = Opcode::recur;
			return;
		}
	}

	// Fused two-arg arithmetic: ss, or sc when the rhs is a number literal.
	if (expr->call.args.size() == 2)
	{
		std::string_view name = proc->var_ref.name;
		Opcode op;
		bool fused = true;
		if (name == "-")       op = Opcode::sub2ss;
		else if (name == "+")  op = Opcode::add2ss;
		else if (name == "*")  op = Opcode::mul2ss;
		else if (name == "/")  op = Opcode::div2ss;
		else if (name == "=")  op = Opcode::eq2ss;
		else if (name == "<")  op = Opcode::lt2ss;
		else if (name == "<=") op = Opcode::le2ss;
		else if (name == ">")  op = Opcode::gt2ss;
		else if (name == ">=") op = Opcode::ge2ss;
		else                   fused = false;
		if (fused && prim_binding_lowerable(proc_binding, name))
		{
			if (expr->call.args[1]->kind == ExprKind::NumberLit)
			{
				switch (op)
				{
					case Opcode::sub2ss: op = Opcode::sub2sc; break;
					case Opcode::add2ss: op = Opcode::add2sc; break;
					case Opcode::mul2ss: op = Opcode::mul2sc; break;
					case Opcode::div2ss: op = Opcode::div2sc; break;
					case Opcode::eq2ss:  op = Opcode::eq2sc;  break;
					case Opcode::lt2ss:  op = Opcode::lt2sc;  break;
					default:             break;   // no sc form for le/gt/ge
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

	// Callee in a boxed binding: slot IC, local variant when the last arg is
	// a plain local.
	if (proc_binding.lambda != current && slot)
	{
		std::optional<uint16_t> found = find_upvalue(current, proc_binding.lambda,
													 static_cast<uint32_t>(proc_binding.breadth));
		JET_DIE_UNLESS(found, "codegen: cacheable call missing upvalue entry");
		sel.op = Opcode::call_ic_slot_0;
		sel.u.call_ic_slot.upvalue_idx = *found;
		if (!expr->call.args.empty() && expr->call.args.back()->kind == ExprKind::VarRef)
		{
			ResolvedBinding lb = binding(expr->call.args.back());
			if (lb.lambda == current && !needs_slot(lb.lambda, static_cast<uint32_t>(lb.breadth)))
			{
				sel.op = Opcode::call_ic_slot_local_0;
				sel.u.call_ic_slot.local_off = narrow_off(lb.breadth);
			}
		}
		return;
	}

	// Callee in an unboxed binding: direct IC.
	if (!slot)
	{
		sel.op = Opcode::call_ic_direct_0;
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

void Compiler::select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, bool is_set)
{
	struct FieldOps
	{
		Opcode ref, ref_ck, set, set_ck;
	};
	constexpr FieldOps LOCAL_FIELD_OPS = {Opcode::ref_local_field, Opcode::ref_local_field_ck,
										  Opcode::set_local_field, Opcode::set_local_field_ck};
	constexpr FieldOps UPVALUE_SLOT_FIELD_OPS = {Opcode::ref_upvalue_slot_field,
												 Opcode::ref_upvalue_slot_field_ck,
												 Opcode::set_upvalue_slot_field,
												 Opcode::set_upvalue_slot_field_ck};
	constexpr FieldOps UPVALUE_DIRECT_FIELD_OPS = {Opcode::ref_upvalue_direct_field,
												   Opcode::ref_upvalue_direct_field_ck,
												   Opcode::set_upvalue_direct_field,
												   Opcode::set_upvalue_direct_field_ck};
	constexpr FieldOps GENERIC_FIELD_OPS = {Opcode::ref_field, Opcode::ref_field_ck,
											Opcode::set_field, Opcode::set_field_ck};
	bool ck = is_literal_key(key);
	auto&& pick_field_op = [&](const FieldOps& ops)
	{
		if (is_set)
		{
			return ck ? ops.set_ck : ops.set;
		}
		return ck ? ops.ref_ck : ops.ref;
	};

	OpSelection& sel = selected_ops_[expr->id].emplace();

	if (!flags_.specialize_ops)
	{
		sel.op = is_set ? Opcode::set_field : Opcode::ref_field;
		return;
	}

	if (receiver->kind == ExprKind::VarRef)
	{
		ResolvedBinding rb = binding(receiver);
		bool slot = needs_slot(rb.lambda, static_cast<uint32_t>(rb.breadth));
		if (rb.lambda == current && !slot)
		{
			sel.op = pick_field_op(LOCAL_FIELD_OPS);
			sel.u.field.addr = narrow_off(rb.breadth);
			return;
		}
		if (rb.lambda != current)
		{
			std::optional<uint16_t> upv = find_upvalue(current, rb.lambda, static_cast<uint32_t>(rb.breadth));
			if (upv)
			{
				sel.op = pick_field_op(slot ? UPVALUE_SLOT_FIELD_OPS : UPVALUE_DIRECT_FIELD_OPS);
				sel.u.field.addr = *upv;
				return;
			}
		}
	}

	sel.op = pick_field_op(GENERIC_FIELD_OPS);
}

void Compiler::select_var_op(Expr* expr, Expr* current, bool is_set)
{
	ResolvedBinding b = binding(expr);
	bool slot = needs_slot(b.lambda, static_cast<uint32_t>(b.breadth));
	OpSelection& sel = selected_ops_[expr->id].emplace();
	if (b.lambda == current)
	{
		sel.op = is_set ? (slot ? Opcode::set_downvalue : Opcode::set_local)
						: (slot ? Opcode::ref_downvalue : Opcode::ref_local);
		sel.u.var.addr = narrow_off(b.breadth);
		return;
	}
	std::optional<uint16_t> found = find_upvalue(current, b.lambda, static_cast<uint32_t>(b.breadth));
	JET_DIE_UNLESS(found, "select-pass: ref to non-local without upvalue entry");
	sel.op = is_set ? Opcode::set_upvalue
					: (slot ? Opcode::ref_upvalue_slot : Opcode::ref_upvalue_direct);
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
		std::unordered_map<uint64_t, Expr*> lambda_cands;
		std::unordered_map<uint64_t, Expr*> const_cands;
		std::unordered_set<Expr*> candidate_lambdas;
		// Candidates being spliced or walked at their own definition: calls
		// to them stay calls, so splicing terminates and a recursive body is
		// never unrolled into itself.
		std::unordered_set<Expr*> active;
		std::vector<Expr*> hosts;
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
					expr->call.proc = walk(expr->call.proc);
					for (uint32_t i = 0; i < expr->call.args.size(); ++i)
					{
						expr->call.args[i] = walk(expr->call.args[i]);
					}
					return expr;
				}

				case ExprKind::Apply:
					expr->apply.proc = walk(expr->apply.proc);
					expr->apply.args = walk(expr->apply.args);
					return expr;

				case ExprKind::Lambda:
				{
					bool guard = candidate_lambdas.count(expr) && active.insert(expr).second;
					hosts.push_back(expr);
					for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
					{
						expr->lambda.body[i] = walk(expr->lambda.body[i]);
					}
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
					for (uint32_t i = 0; i < expr->let.vals.size(); ++i)
					{
						expr->let.vals[i] = walk(expr->let.vals[i]);
					}
					for (uint32_t i = 0; i < expr->let.body.size(); ++i)
					{
						expr->let.body[i] = walk(expr->let.body[i]);
					}
					return expr;
				}

				case ExprKind::SetBang:
					expr->set_bang.value = walk(expr->set_bang.value);
					return expr;

				case ExprKind::SetRef:
					expr->set_ref.obj = walk(expr->set_ref.obj);
					expr->set_ref.key = walk(expr->set_ref.key);
					expr->set_ref.value = walk(expr->set_ref.value);
					return expr;

				case ExprKind::If:
					expr->if_.test = walk(expr->if_.test);
					expr->if_.consequent = walk(expr->if_.consequent);
					if (expr->if_.alternate)
					{
						expr->if_.alternate = walk(expr->if_.alternate);
					}
					return expr;

				default:
					JET_DIE("%d:%d: anf-inline: unhandled ExprKind %d", expr->loc.line,
							 expr->loc.col, static_cast<int>(expr->kind));
			}
		}
	};

} // namespace

void Compiler::run_anf_inline(Program& program)
{
	// Needs current compute_lambda_bindings results, and the caller must rerun
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

	struct Stackify
	{
		Compiler& db;
		std::unordered_map<uint64_t, uint32_t> use_counts;

		static bool is_temp(std::string_view name)
		{
			return name.size() > 3 && name[0] == '%' && name[1] == 't' && name[2] == ' ';
		}

		void count_uses(Expr* e)
		{
			switch (e->kind)
			{
				case ExprKind::NumberLit:
				case ExprKind::StringLit:
				case ExprKind::BooleanLit:
				case ExprKind::CharacterLit:
				case ExprKind::SymbolLit:
				case ExprKind::UnknownLit:
				case ExprKind::PrimRef:
					break;
				case ExprKind::VarRef:
					++use_counts[binding_key(get(db.bindings_, e->id))];
					break;
				case ExprKind::Call:
					count_uses(e->call.proc);
					for (Expr* arg : e->call.args)
					{
						count_uses(arg);
					}
					break;
				case ExprKind::Apply:
					count_uses(e->apply.proc);
					count_uses(e->apply.args);
					break;
				case ExprKind::Lambda:
					for (Expr* form : e->lambda.body)
					{
						count_uses(form);
					}
					break;
				case ExprKind::Let:
					for (Expr* val : e->let.vals)
					{
						count_uses(val);
					}
					for (Expr* form : e->let.body)
					{
						count_uses(form);
					}
					break;
				case ExprKind::SetBang:
					count_uses(e->set_bang.value);
					break;
				case ExprKind::SetRef:
					count_uses(e->set_ref.obj);
					count_uses(e->set_ref.key);
					count_uses(e->set_ref.value);
					break;
				case ExprKind::If:
					count_uses(e->if_.test);
					count_uses(e->if_.consequent);
					if (e->if_.alternate)
					{
						count_uses(e->if_.alternate);
					}
					break;
				default:
					JET_DIE("%d:%d: stackify: unhandled ExprKind %d", e->loc.line, e->loc.col,
							 static_cast<int>(e->kind));
			}
		}

		Expr** scan_operand(Expr** op, uint64_t target, bool allow_atom_skip, bool& done)
		{
			// A complex operand evaluates before everything scanned after it, so
			// reaching one means the target must be inside it or nowhere.
			if ((*op)->kind == ExprKind::VarRef)
			{
				if (binding_key(get(db.bindings_, (*op)->id)) == target)
				{
					done = true;
					return op;
				}
				done = !allow_atom_skip;
				return nullptr;
			}
			if (is_anf_atom(*op))
			{
				done = !allow_atom_skip;
				return nullptr;
			}
			done = true;
			return find_first_use(op, target, allow_atom_skip);
		}

		Expr** find_first_use(Expr** site, uint64_t target, bool allow_atom_skip)
		{
			// Returns the location holding the target's use if that use is the
			// first thing *site evaluates, else null.
			Expr* e = *site;
			switch (e->kind)
			{
				case ExprKind::VarRef:
					return binding_key(get(db.bindings_, e->id)) == target ? site : nullptr;

				// A let sequences its vals before its body: no atom skips
				// across that boundary, so drill without scanning.
				case ExprKind::Let:
					return find_first_use(e->let.names.size() ? &e->let.vals[0] : &e->let.body[0],
										  target, allow_atom_skip);

				case ExprKind::If:
					return find_first_use(&e->if_.test, target, allow_atom_skip);

				case ExprKind::SetBang:
					return find_first_use(&e->set_bang.value, target, allow_atom_skip);

				case ExprKind::Call:
				{
					bool done = false;
					Expr** r = scan_operand(&e->call.proc, target, allow_atom_skip, done);
					if (done)
					{
						return r;
					}
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						r = scan_operand(&e->call.args[i], target, allow_atom_skip, done);
						if (done)
						{
							return r;
						}
					}
					return nullptr;
				}

				case ExprKind::Apply:
				{
					bool done = false;
					Expr** r = scan_operand(&e->apply.proc, target, allow_atom_skip, done);
					if (done)
					{
						return r;
					}
					r = scan_operand(&e->apply.args, target, allow_atom_skip, done);
					return done ? r : nullptr;
				}

				case ExprKind::SetRef:
				{
					bool done = false;
					Expr** r = scan_operand(&e->set_ref.obj, target, allow_atom_skip, done);
					if (done)
					{
						return r;
					}
					r = scan_operand(&e->set_ref.key, target, allow_atom_skip, done);
					if (done)
					{
						return r;
					}
					r = scan_operand(&e->set_ref.value, target, allow_atom_skip, done);
					return done ? r : nullptr;
				}

				default:
					return nullptr;
			}
		}

		Expr* walk(Expr* e)
		{
			switch (e->kind)
			{
				case ExprKind::NumberLit:
				case ExprKind::StringLit:
				case ExprKind::BooleanLit:
				case ExprKind::CharacterLit:
				case ExprKind::SymbolLit:
				case ExprKind::UnknownLit:
				case ExprKind::PrimRef:
				case ExprKind::VarRef:
					return e;

				case ExprKind::Call:
					e->call.proc = walk(e->call.proc);
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						e->call.args[i] = walk(e->call.args[i]);
					}
					return e;

				case ExprKind::Apply:
					e->apply.proc = walk(e->apply.proc);
					e->apply.args = walk(e->apply.args);
					return e;

				case ExprKind::Lambda:
					for (uint32_t i = 0; i < e->lambda.body.size(); ++i)
					{
						e->lambda.body[i] = walk(e->lambda.body[i]);
					}
					return e;

				case ExprKind::SetBang:
					e->set_bang.value = walk(e->set_bang.value);
					return e;

				case ExprKind::SetRef:
					e->set_ref.obj = walk(e->set_ref.obj);
					e->set_ref.key = walk(e->set_ref.key);
					e->set_ref.value = walk(e->set_ref.value);
					return e;

				case ExprKind::If:
					e->if_.test = walk(e->if_.test);
					e->if_.consequent = walk(e->if_.consequent);
					if (e->if_.alternate)
					{
						e->if_.alternate = walk(e->if_.alternate);
					}
					return e;

				case ExprKind::Let:
				{
					for (uint32_t i = 0; i < e->let.vals.size(); ++i)
					{
						e->let.vals[i] = walk(e->let.vals[i]);
					}
					for (uint32_t i = 0; i < e->let.body.size(); ++i)
					{
						e->let.body[i] = walk(e->let.body[i]);
					}

					uint32_t n = e->let.names.size();
					Compiler::LambdaBindings& lb = db.lambda_bindings_[e->let.owner];
					// Only trailing bindings contract: an earlier binding's val
					// may not move past a later binding's.
					while (n > 0)
					{
						uint32_t idx = n - 1;
						uint32_t breadth = e->let.slot_base + idx;
						if (get(lb.captured, breadth) || get(lb.mutated, breadth))
						{
							break;
						}
						uint64_t target = binding_key({e->let.owner, breadth});
						Expr* val = e->let.vals[idx];
						auto it = use_counts.find(target);
						uint32_t uses = it == use_counts.end() ? 0 : it->second;
						if (uses == 0)
						{
							if (!is_anf_atom(val))
							{
								Expr** data = db.arena.alloc_array<Expr*>(e->let.body.size() + 1);
								data[0] = val;
								for (uint32_t i = 0; i < e->let.body.size(); ++i)
								{
									data[i + 1] = e->let.body[i];
								}
								e->let.body = {data, e->let.body.size() + 1};
							}
							--n;
							continue;
						}
						if (uses != 1)
						{
							break;
						}
						// A temp's val may hop over sibling atom reads: a combination's
						// operand order is unspecified, so the reorder just restores the
						// source's order. A user-written let promises its val runs before
						// the whole body, so it gets no skips.
						Expr** site = find_first_use(&e->let.body[0], target, is_temp(e->let.names[idx]));
						if (!site)
						{
							break;
						}
						*site = val;
						--n;
					}
					if (n != e->let.names.size())
					{
						e->let.names = {e->let.names.data, n};
						e->let.vals = {e->let.vals.data, n};
					}
					if (n == 0 && e->let.body.size() == 1)
					{
						return e->let.body[0];
					}
					return e;
				}

				default:
					JET_DIE("%d:%d: stackify: unhandled ExprKind %d", e->loc.line, e->loc.col,
							 static_cast<int>(e->kind));
			}
		}
	};

} // namespace

void Compiler::run_stackify(Program& program)
{
	// Needs current compute_lambda_bindings results; the tree need not stay
	// ANF afterwards.
	Stackify pass{.db = *this};
	for (Expr* form : program.forms)
	{
		pass.count_uses(form);
	}
	for (uint32_t i = 0; i < program.forms.size(); ++i)
	{
		program.forms[i] = pass.walk(program.forms[i]);
	}
}

namespace
{

	// LIR: linear IR between stackified resolved ANF and bytes.

	struct LirInst
	{
		Opcode op;   // base opcode only: no _1.._7 replicas; label is IR-only
		union
		{
			struct { uint16_t off; } local;                // ldl stl ldd std boxl
			struct { uint16_t idx; } upvalue;              // ldu ldus stu
			struct { uint16_t idx; } pool;                 // ldc, *2sc rhs
			struct { uint32_t id; } label;                 // label; if/skip target
			struct { uint32_t nargs; bool tail; } call;    // call, recur
			struct { uint32_t nargs; bool tail; uint16_t upvalue_idx; uint16_t local_off; } call_ic_slot;
			struct { uint32_t nargs; bool tail; uint8_t src; uint16_t idx; } call_ic_direct;
			struct { uint16_t pool_idx; uint16_t first_capture; uint16_t n_captures; } closure;
			struct { uint16_t addr; uint16_t key_idx; } field;   // key_idx only _ck
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
		uint32_t n_locals = 0;
		uint16_t pool_slot = 0;   // unused for the toplevel (index 0)
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

		void emit(LirInst i) { current_lambda().code.push_back(i); }

		void emit_op(Opcode op) { emit(inst(op)); }

		void emit_local(Opcode op, uint16_t off)
		{
			LirInst i = inst(op);
			i.u.local.off = off;
			emit(i);
		}

		void emit_upvalue(Opcode op, uint16_t idx)
		{
			LirInst i = inst(op);
			i.u.upvalue.idx = idx;
			emit(i);
		}

		void emit_label(Opcode op, uint32_t id)
		{
			LirInst i = inst(op);
			i.u.label.id = id;
			emit(i);
		}

		void emit_ldc(uint16_t idx)
		{
			LirInst i = inst(Opcode::ldc);
			i.u.pool.idx = idx;
			emit(i);
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

		void emit_sequence(Slice<Expr*>& forms)
		{
			uint32_t n = forms.size();
			for (uint32_t i = 0; i < n; ++i)
			{
				emit_expr(forms[i]);
				if (i + 1 < n)
				{
					emit_op(Opcode::pop);
				}
			}
		}

		void emit_prologue(Expr* lambda)
		{
			Slice<bool>& captured = lambda->lambda.captured_locals;
			Slice<bool>& mutated = lambda->lambda.mutated_locals;
			for (uint32_t i = 0; i < captured.size(); ++i)
			{
				if (captured[i] && mutated[i])
				{
					emit_local(Opcode::box_local, narrow_off(i));
				}
			}
		}

		uint16_t emit_lifted_lambda(Expr* expr)
		{
			uint32_t idx = static_cast<uint32_t>(prog.lambdas.size());
			prog.lambdas.emplace_back();
			prog.lambdas[idx].is_variadic = expr->lambda.is_variadic;
			prog.lambdas[idx].n_params = expr->lambda.params.size();
			prog.lambdas[idx].n_locals = static_cast<uint32_t>(
				expr->lambda.params.size() + (expr->lambda.is_variadic ? 1 : 0));

			outer_lambdas.push_back(expr);
			lambda_stack.push_back(idx);
			emit_prologue(expr);
			emit_sequence(expr->lambda.body);
			emit_op(Opcode::ret);
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
					cap.idx = static_cast<uint16_t>(rb.breadth);
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

		void emit_lambda_value(Expr* expr)
		{
			uint16_t pool_index = emit_lifted_lambda(expr);
			if (expr->lambda.upvalues.empty())
			{
				emit_ldc(pool_index);
				return;
			}
			LirInst i = inst(Opcode::make_closure);
			i.u.closure.pool_idx = pool_index;
			i.u.closure.first_capture = static_cast<uint16_t>(current_lambda().captures.size());
			i.u.closure.n_captures = static_cast<uint16_t>(expr->lambda.upvalues.size());
			emit_capture_recipe(expr);
			emit(i);
		}

		void emit_var_ref_or_set(Expr* expr)
		{
			JET_DIE_WHEN(!db.selected_ops_[expr->id], "%d:%d: codegen: var access without selection",
						  expr->loc.line, expr->loc.col);
			Compiler::OpSelection sel = *db.selected_ops_[expr->id];
			switch (sel.op)
			{
				case Opcode::ref_local:
				case Opcode::ref_downvalue:
				case Opcode::set_local:
				case Opcode::set_downvalue:
					emit_local(sel.op, sel.u.var.addr);
					break;

				case Opcode::ref_upvalue_direct:
				case Opcode::ref_upvalue_slot:
				case Opcode::set_upvalue:
					emit_upvalue(sel.op, sel.u.var.addr);
					break;

				default:
					JET_DIE("%d:%d: codegen: unexpected var selection %d",
							 expr->loc.line, expr->loc.col, static_cast<int>(sel.op));
			}
		}

		void emit_args(Slice<Expr*>& args, size_t n)
		{
			for (size_t i = 0; i < n; ++i)
			{
				emit_expr(args[i]);
			}
		}

		void emit_field_op(Compiler::OpSelection sel, Expr* receiver, Expr* key, Expr* val)
		{
			auto&& takes_receiver_from_stack = [](Opcode o)
			{
				switch (o)
				{
					case Opcode::ref_field: case Opcode::ref_field_ck:
					case Opcode::set_field: case Opcode::set_field_ck:
						return true;
					default:
						return false;
				}
			};
			auto&& takes_key_from_pool = [](Opcode o)
			{
				switch (o)
				{
					case Opcode::ref_field_ck:                case Opcode::set_field_ck:
					case Opcode::ref_local_field_ck:          case Opcode::set_local_field_ck:
					case Opcode::ref_upvalue_direct_field_ck: case Opcode::set_upvalue_direct_field_ck:
					case Opcode::ref_upvalue_slot_field_ck:   case Opcode::set_upvalue_slot_field_ck:
						return true;
					default:
						return false;
				}
			};

			bool from_stack = takes_receiver_from_stack(sel.op);
			bool from_pool = takes_key_from_pool(sel.op);
			if (from_stack)
			{
				emit_expr(receiver);
			}
			if (!from_pool)
			{
				emit_expr(key);
			}
			if (val)
			{
				emit_expr(val);
			}
			LirInst i = inst(sel.op);
			if (!from_stack)
			{
				i.u.field.addr = sel.u.field.addr;
			}
			if (from_pool)
			{
				i.u.field.key_idx = intern_literal_key(key);
			}
			emit(i);
		}

		void emit_program(Program& program)
		{
			prog.lambdas.emplace_back();
			prog.lambdas[0].n_locals = static_cast<uint32_t>(db.toplevel_lambda_->lambda.names.size());
			emit_prologue(db.toplevel_lambda_);
			for (Expr* form : program.forms)
			{
				emit_expr(form);
			}
			emit_op(Opcode::ret);
		}

		void emit_expr(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = number_lit_value(expr->number_lit.text);
					Number n = static_cast<Number>(val);
					emit_ldc(intern_typed(ConstTag::Number, n));
					break;
				}

				case ExprKind::BooleanLit:
				{
					bool v = expr->boolean_lit.value;
					emit_ldc(intern_typed(ConstTag::Boolean, v));
					break;
				}

				case ExprKind::CharacterLit:
				{
					Character c = static_cast<Character>(expr->character_lit.value);
					emit_ldc(intern_typed(ConstTag::Character, c));
					break;
				}

				case ExprKind::StringLit:
					emit_ldc(intern_string(ConstTag::String, expr->string_lit.value));
					break;

				case ExprKind::SymbolLit:
					emit_ldc(intern_string(ConstTag::Symbol, expr->symbol_lit.name));
					break;

				case ExprKind::UnknownLit:
					emit_ldc(intern_empty(ConstTag::Unknown));
					break;

				case ExprKind::VarRef:
					emit_var_ref_or_set(expr);
					break;

				case ExprKind::Call:
				{
					bool tail = db.is_tail(expr);
					size_t nargs = static_cast<size_t>(expr->call.args.size());
					JET_DIE_WHEN(!db.selected_ops_[expr->id], "%d:%d: codegen: Call without selection",
								  expr->loc.line, expr->loc.col);
					Compiler::OpSelection sel = *db.selected_ops_[expr->id];

					switch (sel.op)
					{
						case Opcode::recur:
						{
							emit_args(expr->call.args, nargs);
							LirInst i = inst(Opcode::recur);
							i.u.call.nargs = static_cast<uint32_t>(nargs);
							i.u.call.tail = true;
							emit(i);
							break;
						}

						case Opcode::sub2ss:
						case Opcode::add2ss:
						case Opcode::mul2ss:
						case Opcode::div2ss:
						case Opcode::eq2ss:
						case Opcode::lt2ss:
						case Opcode::le2ss:
						case Opcode::gt2ss:
						case Opcode::ge2ss:
							emit_expr(expr->call.args[0]);
							emit_expr(expr->call.args[1]);
							emit_op(sel.op);
							break;

						case Opcode::sub2sc:
						case Opcode::add2sc:
						case Opcode::mul2sc:
						case Opcode::div2sc:
						case Opcode::eq2sc:
						case Opcode::lt2sc:
						{
							emit_expr(expr->call.args[0]);
							LirInst i = inst(sel.op);
							i.u.pool.idx = intern_literal_key(expr->call.args[1]);
							emit(i);
							break;
						}

						case Opcode::ref_field:
						case Opcode::ref_field_ck:
						case Opcode::ref_local_field:
						case Opcode::ref_local_field_ck:
						case Opcode::ref_upvalue_direct_field:
						case Opcode::ref_upvalue_direct_field_ck:
						case Opcode::ref_upvalue_slot_field:
						case Opcode::ref_upvalue_slot_field_ck:
							emit_field_op(sel, expr->call.args[0], expr->call.args[1], nullptr);
							break;

						case Opcode::call_ic_slot_0:
						{
							emit_args(expr->call.args, nargs);
							LirInst i = inst(Opcode::call_ic_slot_0);
							i.u.call_ic_slot.nargs = static_cast<uint32_t>(nargs);
							i.u.call_ic_slot.tail = tail;
							i.u.call_ic_slot.upvalue_idx = sel.u.call_ic_slot.upvalue_idx;
							emit(i);
							break;
						}

						case Opcode::call_ic_slot_local_0:
						{
							emit_args(expr->call.args, nargs - 1);
							LirInst i = inst(Opcode::call_ic_slot_local_0);
							i.u.call_ic_slot.nargs = static_cast<uint32_t>(nargs);
							i.u.call_ic_slot.tail = tail;
							i.u.call_ic_slot.upvalue_idx = sel.u.call_ic_slot.upvalue_idx;
							i.u.call_ic_slot.local_off = sel.u.call_ic_slot.local_off;
							emit(i);
							break;
						}

						case Opcode::call_ic_direct_0:
						{
							emit_args(expr->call.args, nargs);
							LirInst i = inst(Opcode::call_ic_direct_0);
							i.u.call_ic_direct.nargs = static_cast<uint32_t>(nargs);
							i.u.call_ic_direct.tail = tail;
							i.u.call_ic_direct.src = sel.u.call_ic_direct.src;
							i.u.call_ic_direct.idx = sel.u.call_ic_direct.idx;
							emit(i);
							break;
						}

						case Opcode::call:
						{
							emit_expr(expr->call.proc);
							emit_args(expr->call.args, nargs);
							LirInst i = inst(Opcode::call);
							i.u.call.nargs = static_cast<uint32_t>(nargs);
							i.u.call.tail = tail;
							emit(i);
							break;
						}

						default:
							JET_DIE("%d:%d: codegen: unexpected Call selection %d",
									 expr->loc.line, expr->loc.col, static_cast<int>(sel.op));
					}
					break;
				}

				case ExprKind::Apply:
					emit_expr(expr->apply.proc);
					emit_expr(expr->apply.args);
					emit_op(Opcode::apply);
					break;

				case ExprKind::Lambda:
					emit_lambda_value(expr);
					break;

				case ExprKind::PrimRef:
					emit_ldc(intern_global_name(expr->prim_ref.name));
					break;

				case ExprKind::SetBang:
					emit_expr(expr->set_bang.value);
					emit_var_ref_or_set(expr);
					break;

				case ExprKind::Let:
				{
					uint32_t sb = expr->let.slot_base;
					uint32_t n = expr->let.names.size();
					if (sb + n > current_lambda().n_locals)
					{
						current_lambda().n_locals = sb + n;
					}
					for (uint32_t i = 0; i < n; ++i)
					{
						emit_expr(expr->let.vals[i]);
						emit_local(Opcode::set_local, narrow_off(sb + i));
						emit_op(Opcode::pop);
					}
					for (uint32_t i = 0; i < n; ++i)
					{
						if (needs_slot(expr->let.owner, sb + i))
						{
							emit_local(Opcode::box_local, narrow_off(sb + i));
						}
					}
					emit_sequence(expr->let.body);
					break;
				}

				case ExprKind::SetRef:
				{
					JET_DIE_WHEN(!db.selected_ops_[expr->id], "%d:%d: codegen: SetRef without selection",
								  expr->loc.line, expr->loc.col);
					emit_field_op(*db.selected_ops_[expr->id], expr->set_ref.obj, expr->set_ref.key,
								  expr->set_ref.value);
					break;
				}

				case ExprKind::If:
				{
					emit_expr(expr->if_.test);
					uint32_t l_alt = prog.next_label++;
					uint32_t l_end = prog.next_label++;
					emit_label(Opcode::if_then_else, l_alt);
					emit_expr(expr->if_.consequent);
					emit_label(Opcode::skip, l_end);
					emit_label(Opcode::label, l_alt);
					if (expr->if_.alternate)
					{
						emit_expr(expr->if_.alternate);
					}
					else
					{
						emit_ldc(intern_empty(ConstTag::Unknown));
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
		size_t v_call_ic_slot = 0;
		size_t v_call_ic_slot_local = 0;
		size_t v_call_ic_direct = 0;

		static size_t encoded_size(const LirInst& i)
		{
			if (i.op == Opcode::label)
			{
				return 0;
			}
			if (i.op == Opcode::make_closure)
			{
				return OPCODE_SIZE + sizeof(OP_make_closure) +
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
			// Pool entry: [tag=Lambda][is_n_ary][n_params|][n_locals][code_size][bytes...]
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
			uint16_t n_locals = static_cast<uint16_t>(L.n_locals);
			entry.append(reinterpret_cast<char*>(&n_locals), sizeof(n_locals));
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

				case Opcode::ldc:
				{
					emit_opcode(bc, Opcode::ldc);
					OP_ldc op{};
					op.idx = i.u.pool.idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::make_closure:
				{
					emit_opcode(bc, Opcode::make_closure);
					OP_make_closure op{};
					op.pool_idx = i.u.closure.pool_idx;
					op.n_captures = i.u.closure.n_captures;
					emit_operand(bc, op);
					for (uint16_t c = 0; c < i.u.closure.n_captures; ++c)
					{
						emit_operand(bc, L.captures[static_cast<size_t>(i.u.closure.first_capture) + c]);
					}
					break;
				}

				case Opcode::if_then_else:
				{
					emit_opcode(bc, Opcode::if_then_else);
					OP_if_then_else op{};
					op.consequent_size =
						label_target(label_pos, i.u.label.id) - (bc.size() + sizeof(OP_if_then_else));
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

				case Opcode::call:
				{
					emit_opcode(bc, Opcode::call);
					OP_call op{};
					op.tail = i.u.call.tail;
					op.nargs = static_cast<size_t>(i.u.call.nargs);
					emit_operand(bc, op);
					break;
				}

				case Opcode::recur:
				{
					emit_opcode(bc, Opcode::recur);
					OP_recur op{};
					op.nargs = static_cast<uint8_t>(i.u.call.nargs);
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_ic_slot_0:
				{
					emit_replicated(bc, Opcode::call_ic_slot_0, v_call_ic_slot);
					OP_call_ic_slot op{};
					op.upvalue_idx = i.u.call_ic_slot.upvalue_idx;
					op.tail = i.u.call_ic_slot.tail;
					op.nargs = static_cast<size_t>(i.u.call_ic_slot.nargs);
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_ic_slot_local_0:
				{
					emit_replicated(bc, Opcode::call_ic_slot_local_0, v_call_ic_slot_local);
					OP_call_ic_slot_local op{};
					op.local_off = i.u.call_ic_slot.local_off;
					op.upvalue_idx = i.u.call_ic_slot.upvalue_idx;
					op.tail = i.u.call_ic_slot.tail;
					op.nargs = static_cast<size_t>(i.u.call_ic_slot.nargs);
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_ic_direct_0:
				{
					emit_replicated(bc, Opcode::call_ic_direct_0, v_call_ic_direct);
					OP_call_ic_direct op{};
					op.src = i.u.call_ic_direct.src;
					op.idx = i.u.call_ic_direct.idx;
					op.tail = i.u.call_ic_direct.tail;
					op.nargs = static_cast<size_t>(i.u.call_ic_direct.nargs);
					emit_operand(bc, op);
					break;
				}

				case Opcode::apply:
				case Opcode::pop:
				case Opcode::ret:
				case Opcode::sub2ss:
				case Opcode::add2ss:
				case Opcode::mul2ss:
				case Opcode::div2ss:
				case Opcode::eq2ss:
				case Opcode::lt2ss:
				case Opcode::le2ss:
				case Opcode::gt2ss:
				case Opcode::ge2ss:
					emit_opcode(bc, i.op);
					break;

				case Opcode::sub2sc:
				case Opcode::add2sc:
				case Opcode::mul2sc:
				case Opcode::div2sc:
				case Opcode::eq2sc:
				case Opcode::lt2sc:
				{
					emit_opcode(bc, i.op);
					OP_binop_sc op{};
					op.idx = i.u.pool.idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_local:
				case Opcode::set_local:
				case Opcode::ref_downvalue:
				case Opcode::set_downvalue:
				case Opcode::box_local:
					emit_opcode(bc, i.op);
					emit_operand(bc, i.u.local.off);
					break;

				case Opcode::ref_upvalue_direct:
				case Opcode::ref_upvalue_slot:
				case Opcode::set_upvalue:
					emit_opcode(bc, i.op);
					emit_operand(bc, i.u.upvalue.idx);
					break;

				case Opcode::ref_field:
				case Opcode::set_field:
				{
					emit_opcode(bc, i.op);
					OP_ref_field op{};
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_local_field:
				case Opcode::set_local_field:
				{
					emit_opcode(bc, i.op);
					OP_ref_local_field op{};
					op.off = i.u.field.addr;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_upvalue_direct_field:
				case Opcode::set_upvalue_direct_field:
				case Opcode::ref_upvalue_slot_field:
				case Opcode::set_upvalue_slot_field:
				{
					emit_opcode(bc, i.op);
					OP_ref_upvalue_field op{};
					op.idx = i.u.field.addr;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_field_ck:
				case Opcode::set_field_ck:
				{
					emit_opcode(bc, i.op);
					OP_ref_field_ck op{};
					op.key_idx = i.u.field.key_idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_local_field_ck:
				case Opcode::set_local_field_ck:
				{
					emit_opcode(bc, i.op);
					OP_ref_local_field_ck op{};
					op.off = i.u.field.addr;
					op.key_idx = i.u.field.key_idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ref_upvalue_direct_field_ck:
				case Opcode::set_upvalue_direct_field_ck:
				case Opcode::ref_upvalue_slot_field_ck:
				case Opcode::set_upvalue_slot_field_ck:
				{
					emit_opcode(bc, i.op);
					OP_ref_upvalue_field_ck op{};
					op.idx = i.u.field.addr;
					op.key_idx = i.u.field.key_idx;
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

			// [u32 n_toplevel_slots][u32 n_constants][concatenated pool entries][code...]
			Bytecode out;
			uint32_t n_slots = prog.lambdas[0].n_locals;
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
