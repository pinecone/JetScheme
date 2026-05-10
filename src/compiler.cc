// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "error.h"
#include "runtime.h"
#include "vm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	CITY_DIE("%d:%d: unknown character name '#\\%.*s'", loc.line, loc.col,
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

struct ResolvedBinding
{
	Expr* lambda;
	size_t breadth;
};

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

	std::optional<std::vector<Token>> tokens_;
	std::optional<Program> ast_;
	// Counter used by both parse() and expand-time desugaring so synthesized
	// Expr nodes get unique IDs the binding/tail caches can index by.
	uint32_t next_expr_id_ = 0;
	std::vector<ResolvedBinding> bindings_;
	std::vector<bool> tail_cache_;
	OrderedNameSet toplevel_names_;

	// Compile-time chain of enclosing lambdas during binding resolution.
	// Index 0 is a synthesized toplevel lambda whose names are the program's
	// top-level defines, so binding lookup and codegen can treat the program
	// uniformly as one big nested lambda.
	std::vector<Expr*> lambdas_;
	std::vector<std::unordered_map<std::string_view, size_t>> lambda_name_index_;
	Expr* toplevel_lambda_ = nullptr;

	// Lambdas in resolution order; freeze runs after the inliner mutates
	// upvalue/capture state, so freeze is deferred to a single bulk pass.
	std::vector<Expr*> all_lambdas_;
	std::vector<Expr*> inline_decisions_;
	std::vector<Expr*> const_fold_decisions_;

	bool no_inline_ = false;

	struct LambdaScope
	{
		std::vector<UpvalueRef> upvalues;
		std::unordered_set<uint64_t> upvalue_keys;
		std::vector<bool> captured;
		std::vector<bool> mutated;
		std::vector<bool> reassigned_after_init;
		std::vector<Expr*> bound_init;
	};
	std::unordered_map<Expr*, LambdaScope> lambda_scopes_;

	Bytecode compile();

	std::vector<Token>& tokens();
	Program& ast();
	ResolvedBinding binding(Expr* expr);
	bool is_tail(Expr* expr);

	Expr* expand(Expr* expr);
	Expr* expand_let(Expr* expr);
	Expr* expand_letrec(Expr* expr);
	Expr* expand_begin(Expr* expr);

	std::optional<ResolvedBinding> lookup_name(std::string_view name);
	void push_lambda_scope(Expr* lambda);
	void pop_lambda_scope();
	Expr* binding_lambda(ResolvedBinding b);
	void record_ref(ResolvedBinding b);
	void record_set(ResolvedBinding b, bool is_init, Expr* value);
	void resolve_bindings(Program& program);
	void resolve_bindings_in(Expr* expr);
	void freeze_lambda(Expr* lambda);
	void run_inliner();

	void compute_tail(Expr* expr, bool in_tail);

	// Lenient internal-define hoist: collects every (define x v) reachable
	// from a lambda body (descending through Begin only) into a fresh IIFE
	// of the form ((lambda (x ...) body-with-defines-as-set!s) #f ...),
	// giving letrec*-equivalent semantics.
	Slice<Expr*> hoist_defines_in_body(Slice<Expr*> body, SourceLoc loc);
	Expr* rewrite_define_in(Expr* expr, OrderedNameSet& names);
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
					if (!at_end())
					{
						buf += advance();
					}
				}
				else
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

			char c = peek();
			if (c == 't' || c == 'f')
			{
				buf += advance();
				if (is_delimiter(peek()))
				{
					emit(TokenKind::Boolean, intern(buf), start);
				}
				else
				{
					CITY_DIE("%d:%d: invalid boolean", line, col);
				}
			}
			else if (c == '\\')
			{
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
			}
			else if (c == '(')
			{
				// Leave the '(' in the stream; it lexes as LParen next.
				emit(TokenKind::Hash, intern(buf), start);
			}
			else
			{
				CITY_DIE("%d:%d: invalid # syntax", line, col);
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

		// peek() is on the first digit; buf may already hold a sign. Reads the digit
		// and everything after (hex prefix on a leading '0', else decimal w/ optional
		// fractional and exponent), then emits.
		void finish_number(std::string& buf, SourceLoc start)
		{
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

			CITY_DIE("%d:%d: unexpected character '%c'", line, col, c);
		}

		TokenKind classify_ident(std::string_view text)
		{
			if (text == "lambda")
			{
				return TokenKind::Lambda;
			}
			if (text == "define")
			{
				return TokenKind::Define;
			}
			if (text == "if")
			{
				return TokenKind::If;
			}
			if (text == "set!")
			{
				return TokenKind::Set;
			}
			if (text == "setf!")
			{
				return TokenKind::Setf;
			}
			if (text == "quote")
			{
				return TokenKind::QuoteWord;
			}
			if (text == "apply")
			{
				return TokenKind::Apply;
			}
			if (text == "let")
			{
				return TokenKind::Let;
			}
			if (text == "let*")
			{
				return TokenKind::LetStar;
			}
			if (text == "letrec" || text == "letrec*")
			{
				return TokenKind::Letrec;
			}
			if (text == "begin")
			{
				return TokenKind::Begin;
			}
			if (text == "when")
			{
				return TokenKind::When;
			}
			if (text == "unless")
			{
				return TokenKind::Unless;
			}
			if (text == "cond")
			{
				return TokenKind::Cond;
			}
			if (text == "and")
			{
				return TokenKind::And;
			}
			if (text == "or")
			{
				return TokenKind::Or;
			}
			if (text == "include")
			{
				return TokenKind::Include;
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

			if (c == '(')
			{
				advance();
				emit(TokenKind::LParen, intern(std::string{c}), l);
			}
			else if (c == ')')
			{
				advance();
				in_quote = false;
				emit(TokenKind::RParen, intern(std::string{c}), l);
			}
			else if (c == '\'')
			{
				advance();
				in_quote = true;
				emit(TokenKind::Quote, intern(std::string{c}), l);
			}
			else if (c == '"')
			{
				lex_string();
			}
			else if (c == '#')
			{
				lex_hash();
			}
			else if (c == '.')
			{
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
			}
			else
			{
				lex_number_or_ident();
				// Quote mode wraps a single Variable token; for lists, in_quote exits via ')'.
				if (in_quote && pending_.kind == TokenKind::Variable)
				{
					in_quote = false;
				}
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
		// Token source. Either span+pos (batch) or stream_lex (lex on demand).
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
				CITY_DIE("%d:%d: expected token %d, got %d", peek().loc.line, peek().loc.col,
						 static_cast<int>(kind), static_cast<int>(peek().kind));
			}
			advance();
		}

		std::string_view expect_identifier(const char* what)
		{
			CITY_DIE_UNLESS(peek().kind == TokenKind::Variable,
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

		Slice<Expr*> make_slice(std::vector<Expr*>& vec)
		{
			uint32_t count = static_cast<uint32_t>(vec.size());
			Expr** data = arena.alloc_array<Expr*>(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				data[i] = vec[i];
			}
			return {data, count};
		}

		Slice<std::string_view> make_string_slice(std::vector<std::string_view>& vec)
		{
			uint32_t count = static_cast<uint32_t>(vec.size());
			std::string_view* data = arena.alloc_array<std::string_view>(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				data[i] = vec[i];
			}
			return {data, count};
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
					CITY_DIE("%d:%d: unexpected token", tok.loc.line, tok.loc.col);
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
					return parse_dollar_line(loc);
				}
				if (head.text == "$col")
				{
					return parse_dollar_col(loc);
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
				CITY_DIE("%d:%d: expected formals", loc.line, loc.col);
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
				CITY_DIE("%d:%d: %%prim expects a string literal", loc.line, loc.col);
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
				CITY_DIE("%d:%d: setf! place form must be (ref obj key)", place_loc.line, place_loc.col);
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

		// (let* ((a v1) (b v2) ...) body...) →
		//   (let ((a v1)) (let ((b v2)) ... body ...))
		Expr* parse_let_star(SourceLoc loc)
		{
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
				CITY_DIE("%d:%d: include expects a string path", loc.line, loc.col);
			}
			std::string_view raw_path = advance().text;
			expect(TokenKind::RParen);

			std::string_view raw_inc_path = process_string_escapes(raw_path);

			// Relative paths resolve against the including file's directory, not the cwd.
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
				CITY_DIE("%d:%d: cannot open '%.*s'", loc.line, loc.col,
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

		Expr* parse_dollar_line(SourceLoc loc)
		{
			advance();
			expect(TokenKind::RParen);
			Expr* e = make_expr(ExprKind::NumberLit, loc);
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", loc.line);
			e->number_lit.text = arena.copy_string(buf);
			return e;
		}

		Expr* parse_dollar_col(SourceLoc loc)
		{
			advance();
			expect(TokenKind::RParen);
			Expr* e = make_expr(ExprKind::NumberLit, loc);
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", loc.col);
			e->number_lit.text = arena.copy_string(buf);
			return e;
		}

		// ($check expr) → (%check expr "file" line col).
		Expr* parse_dollar_check(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			expect(TokenKind::RParen);

			Expr* check_ref = make_expr(ExprKind::VarRef, loc);
			check_ref->var_ref.name = arena.copy_string("%check");

			Expr* file_lit = make_expr(ExprKind::StringLit, loc);
			file_lit->string_lit.value = arena.copy_string(file_table[loc.file_id]);

			char buf[16];

			Expr* line_lit = make_expr(ExprKind::NumberLit, loc);
			snprintf(buf, sizeof(buf), "%d", loc.line);
			line_lit->number_lit.text = arena.copy_string(buf);

			Expr* col_lit = make_expr(ExprKind::NumberLit, loc);
			snprintf(buf, sizeof(buf), "%d", loc.col);
			col_lit->number_lit.text = arena.copy_string(buf);

			std::vector<Expr*> args{test, file_lit, line_lit, col_lit};

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
					CITY_DIE("%d:%d: unexpected token in datum", tok.loc.line, tok.loc.col);
			}
		}

		// '(a b c) → Call{VarRef{"list"}, [a, b, c]}.
		Expr* parse_quoted_list()
		{
			SourceLoc loc = peek().loc;
			expect(TokenKind::LParen);

			std::vector<Expr*> elems;
			Expr* tail = nullptr;
			while (peek().kind != TokenKind::RParen)
			{
				if (peek().kind == TokenKind::Dot)
				{
					advance();
					CITY_DIE_UNLESS(!elems.empty(), "%d:%d: dotted pair needs a head", peek().loc.line,
								peek().loc.col);
					tail = parse_datum();
					CITY_DIE_UNLESS(peek().kind == TokenKind::RParen,
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

		// #(a b c) → Call{VarRef{"vector"}, [a, b, c]}.
		Expr* parse_quoted_vector()
		{
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

	struct Emitter
	{
		Bytecode& bc;
		Compiler& db;
		std::vector<Expr*> outer_lambdas{db.toplevel_lambda_};

		// Dedup constants by byte-encoded form so identical literals share
		// one pool slot.
		std::vector<std::string> pool{};
		std::unordered_map<std::string, uint16_t> pool_idx{};

		// Rotate among CITY_REPLICATE_N variants so distinct call sites
		// land on distinct asm dispatch tails (Ertl & Gregg 2003).
		size_t v_call_ic_slot = 0;
		size_t v_call_ic_slot_local = 0;
		size_t v_call_ic_direct = 0;

		struct InlineCtx
		{
			Expr* inner_lambda;
			uint32_t k_offset;
			bool was_tail;
		};
		std::vector<InlineCtx> inline_ctxs{};
		std::vector<uint32_t> max_locals_stack{
			static_cast<uint32_t>(db.toplevel_lambda_->lambda.names.size())};
		uint16_t last_pool_n_locals = 0;

		uint32_t& current_max_locals() { return max_locals_stack.back(); }

		ResolvedBinding translate_binding(ResolvedBinding b)
		{
			for (auto it = inline_ctxs.rbegin(); it != inline_ctxs.rend(); ++it)
			{
				if (it->inner_lambda == b.lambda)
				{
					b.lambda = outer_lambdas.back();
					b.breadth = it->k_offset + b.breadth;
					break;
				}
			}
			return b;
		}

		bool effective_tail(Expr* expr)
		{
			if (!db.is_tail(expr))
			{
				return false;
			}
			for (InlineCtx& ctx : inline_ctxs)
			{
				if (!ctx.was_tail)
				{
					return false;
				}
			}
			return true;
		}

		void emit_raw(const void* data, size_t size)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			bc.insert(bc.end(), p, p + size);
		}

		uint16_t narrow_off(size_t v)
		{
			CITY_DIE_WHEN(v > UINT16_MAX, "codegen: frame offset %zu exceeds uint16_t", v);
			return static_cast<uint16_t>(v);
		}

		template <class T>
		void emit(T& val)
		{
			emit_raw(&val, sizeof(T));
		}

		void emit_opcode(Opcode op)
		{
			size_t at = bc.size();
			bc.resize(at + OPCODE_SIZE);
			bc[at + VM_OP_SLOT_SIZE] = static_cast<uint8_t>(op);
		}

		void emit_replicated(Opcode base, size_t& counter)
		{
			int offset = static_cast<int>(counter++ % CITY_REPLICATE_N);
			emit_opcode(static_cast<Opcode>(static_cast<int>(base) + offset));
		}

		void emit_cstring(std::string_view s)
		{
			emit_raw(s.data(), s.size());
			bc.push_back(0);
		}

		size_t emit_size_hole()
		{
			size_t pos = bc.size();
			size_t zero = 0;
			emit(zero);
			return pos;
		}

		void patch_size(size_t hole_pos, size_t end_pos)
		{
			size_t size = end_pos - (hole_pos + sizeof(size_t));
			memcpy(&bc[hole_pos], &size, sizeof(size_t));
		}

		uint16_t intern_constant(std::string& serialized)
		{
			std::unordered_map<std::string, uint16_t>::iterator it = pool_idx.find(serialized);
			if (it != pool_idx.end())
			{
				return it->second;
			}
			uint16_t idx = static_cast<uint16_t>(pool.size());
			pool.push_back(serialized);
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

		void emit_ldc(uint16_t idx)
		{
			emit_opcode(Opcode::ldc);
			OP_ldc op{};
			op.idx = idx;
			emit(op);
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

		uint16_t intern_literal_key(Expr* e)
		{
			switch (e->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = strtod_l(std::string{e->number_lit.text}.c_str(), nullptr, c_locale);
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
					CITY_DIE("intern_literal_key: not a literal Expr (kind %d)", static_cast<int>(e->kind));
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
					emit_opcode(Opcode::pop);
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
					emit_opcode(Opcode::box_local);
					OP_box_local op{};
					op.off = narrow_off(i);
					emit(op);
				}
			}
		}

		void emit_lambda_body(Expr* expr)
		{
			outer_lambdas.push_back(expr);
			uint32_t init_locals =
				static_cast<uint32_t>(expr->lambda.params.size() + (expr->lambda.is_variadic ? 1 : 0));
			max_locals_stack.push_back(init_locals);
			std::vector<InlineCtx> saved_ctxs;
			saved_ctxs.swap(inline_ctxs);
			emit_prologue(expr);
			emit_sequence(expr->lambda.body);
			saved_ctxs.swap(inline_ctxs);
			last_pool_n_locals = static_cast<uint16_t>(max_locals_stack.back());
			max_locals_stack.pop_back();
			outer_lambdas.pop_back();
			emit_opcode(Opcode::ret);
		}

		// Pool entry: [tag=Lambda][is_n_ary][n_params|][n_locals][code_size][bytes...]
		uint16_t intern_lifted_lambda(Expr* expr)
		{
			size_t body_start = bc.size();
			emit_lambda_body(expr);
			size_t code_size = bc.size() - body_start;

			std::string entry;
			entry.push_back(static_cast<char>(ConstTag::Lambda));
			bool is_n_ary = expr->lambda.is_variadic;
			entry.append(reinterpret_cast<char*>(&is_n_ary), sizeof(is_n_ary));
			if (!is_n_ary)
			{
				size_t n = static_cast<size_t>(expr->lambda.params.size());
				entry.append(reinterpret_cast<char*>(&n), sizeof(n));
			}
			uint16_t n_locals = last_pool_n_locals;
			entry.append(reinterpret_cast<char*>(&n_locals), sizeof(n_locals));
			entry.append(reinterpret_cast<char*>(&code_size), sizeof(code_size));
			entry.append(reinterpret_cast<char*>(bc.data() + body_start), code_size);
			bc.resize(body_start);

			uint16_t idx = static_cast<uint16_t>(pool.size());
			pool.push_back(entry);
			return idx;
		}

		uint32_t find_upvalue(Expr* current, Expr* owner, uint32_t breadth)
		{
			Slice<UpvalueRef>& ups = current->lambda.upvalues;
			for (uint32_t i = 0; i < ups.size(); ++i)
			{
				if (ups[i].owner == owner && ups[i].breadth == breadth)
				{
					return i;
				}
			}
			return UINT32_MAX;
		}

		void emit_capture_recipe(Expr* inner)
		{
			Expr* current = outer_lambdas.back();
			Slice<UpvalueRef>& ups = inner->lambda.upvalues;
			for (uint32_t i = 0; i < ups.size(); ++i)
			{
				UpvalueRef u = ups[i];
				ResolvedBinding rb{.lambda = u.owner, .breadth = u.breadth};
				rb = translate_binding(rb);
				OP_make_closure_capture cap{};
				if (rb.lambda == current)
				{
					cap.src = static_cast<uint8_t>(CaptureSource::Local);
					cap.idx = static_cast<uint16_t>(rb.breadth);
				}
				else
				{
					uint32_t found = find_upvalue(current, rb.lambda, static_cast<uint32_t>(rb.breadth));
					CITY_DIE_WHEN(found == UINT32_MAX, "codegen: upvalue not in parent's upvalue list");
					cap.src = static_cast<uint8_t>(CaptureSource::Upvalue);
					cap.idx = static_cast<uint16_t>(found);
				}
				emit(cap);
			}
		}

		void emit_lambda_value(Expr* expr)
		{
			uint16_t pool_index = intern_lifted_lambda(expr);
			if (expr->lambda.upvalues.empty())
			{
				emit_ldc(pool_index);
			}
			else
			{
				emit_opcode(Opcode::make_closure);
				OP_make_closure op{};
				op.pool_idx = pool_index;
				op.n_captures = static_cast<uint16_t>(expr->lambda.upvalues.size());
				emit(op);
				emit_capture_recipe(expr);
			}
		}

		// Splices the body of an immediately-applied lambda (inner) into outer's
		// bytecode. Inner's params get fresh slots in outer's frame at K..K+n-1;
		// args are evaluated and stored into them, and the body runs in outer's
		// frame directly.
		void emit_call_inline(Expr* inner, Slice<Expr*>& args, bool was_tail)
		{
			uint32_t base_slot = current_max_locals();
			uint32_t n = static_cast<uint32_t>(inner->lambda.params.size());
			if (base_slot + n > current_max_locals())
			{
				current_max_locals() = base_slot + n;
			}

			for (uint32_t i = 0; i < args.size(); ++i)
			{
				emit_expr(args[i]);
				emit_opcode(Opcode::set_local);
				OP_set_local sop{};
				sop.off = narrow_off(base_slot + i);
				emit(sop);
				emit_opcode(Opcode::pop);
			}

			Slice<bool>& captured = inner->lambda.captured_locals;
			Slice<bool>& mutated = inner->lambda.mutated_locals;
			for (uint32_t i = 0; i < captured.size(); ++i)
			{
				if (captured[i] && mutated[i])
				{
					emit_opcode(Opcode::box_local);
					OP_box_local bop{};
					bop.off = narrow_off(base_slot + i);
					emit(bop);
				}
			}

			inline_ctxs.push_back({.inner_lambda = inner, .k_offset = base_slot, .was_tail = was_tail});
			emit_sequence(inner->lambda.body);
			inline_ctxs.pop_back();
		}

		bool needs_slot(Expr* owner, uint32_t breadth)
		{
			return owner->lambda.captured_locals[breadth] && owner->lambda.mutated_locals[breadth];
		}

		void emit_var_ref_or_set(ResolvedBinding b, bool is_set)
		{
			Expr* current = outer_lambdas.back();
			Expr* orig_owner = b.lambda;
			size_t orig_breadth = b.breadth;
			b = translate_binding(b);
			if (b.lambda == current)
			{
				bool slot = needs_slot(orig_owner, static_cast<uint32_t>(orig_breadth));
				Opcode opc = is_set ? (slot ? Opcode::set_downvalue : Opcode::set_local)
									: (slot ? Opcode::ref_downvalue : Opcode::ref_local);
				emit_opcode(opc);
				OP_ref_local op{};
				op.off = narrow_off(b.breadth);
				emit(op);
				return;
			}
			uint32_t found = find_upvalue(current, b.lambda, static_cast<uint32_t>(b.breadth));
			CITY_DIE_WHEN(found == UINT32_MAX, "codegen: ref to non-local without upvalue entry");
			uint16_t idx = static_cast<uint16_t>(found);
			if (is_set)
			{
				emit_opcode(Opcode::set_upvalue);
				OP_set_upvalue op{};
				op.idx = idx;
				emit(op);
				return;
			}
			Opcode opc = needs_slot(orig_owner, static_cast<uint32_t>(orig_breadth))
							 ? Opcode::ref_upvalue_slot
							 : Opcode::ref_upvalue_direct;
			emit_opcode(opc);
			OP_ref_upvalue_direct op{};
			op.idx = idx;
			emit(op);
		}

		void emit_expr(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = strtod_l(std::string{expr->number_lit.text}.c_str(), nullptr, c_locale);
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
				{
					if (Expr* fold = db.const_fold_decisions_[expr->id])
					{
						emit_expr(fold);
						break;
					}
					emit_var_ref_or_set(db.binding(expr), false);
					break;
				}

				case ExprKind::Call:
				{
					bool tail = effective_tail(expr);
					size_t nargs = static_cast<size_t>(expr->call.args.size());

					Expr* proc = expr->call.proc;

					// Self-tail-call: proc resolves to a top-level slot whose
					// init was the lambda we're currently emitting, the slot
					// isn't reassigned, lambda is non-variadic, and arity
					// matches. Emit `recur` -- pop args into our own slots and
					// jump back to the function's entry pc.
					if (tail && proc->kind == ExprKind::VarRef)
					{
						ResolvedBinding rb = db.binding(proc);
						Expr* current = outer_lambdas.back();
						if (rb.lambda == db.toplevel_lambda_ && current != db.toplevel_lambda_ &&
							!current->lambda.is_variadic &&
							current->lambda.params.size() == nargs)
						{
							Compiler::LambdaScope& tl = db.lambda_scopes_[db.toplevel_lambda_];
							bool reassigned = rb.breadth < tl.reassigned_after_init.size() &&
											  tl.reassigned_after_init[rb.breadth];
							Expr* init = rb.breadth < tl.bound_init.size() ? tl.bound_init[rb.breadth] : nullptr;
							if (!reassigned && init == current)
							{
								for (Expr* arg : expr->call.args)
								{
									emit_expr(arg);
								}
								emit_opcode(Opcode::recur);
								OP_recur op{};
								op.nargs = static_cast<uint8_t>(nargs);
								emit(op);
								break;
							}
						}
					}

					Expr* inlined_callee = db.inline_decisions_[expr->id];
					if (inlined_callee)
					{
						bool already_inlining = false;
						for (InlineCtx& c : inline_ctxs)
						{
							if (c.inner_lambda == inlined_callee)
							{
								already_inlining = true;
								break;
							}
						}
						if (!already_inlining)
						{
							emit_call_inline(inlined_callee, expr->call.args, tail);
							break;
						}
					}
					if (proc->kind == ExprKind::Lambda && !proc->lambda.is_variadic &&
						proc->lambda.params.size() == expr->call.args.size())
					{
						emit_call_inline(proc, expr->call.args, tail);
						break;
					}
					if (proc->kind == ExprKind::VarRef && nargs == 2)
					{
						std::string_view name = proc->var_ref.name;
						Opcode op;
						bool fused = true;
						if (name == "-")       op = Opcode::sub2ss;
						else if (name == "+")  op = Opcode::add2ss;
						else if (name == "=")  op = Opcode::eq2ss;
						else if (name == "<")  op = Opcode::lt2ss;
						else if (name == "<=") op = Opcode::le2ss;
						else if (name == ">")  op = Opcode::gt2ss;
						else if (name == ">=") op = Opcode::ge2ss;
						else                   fused = false;
						if (fused)
						{
							ResolvedBinding rb = db.binding(proc);
							if (db.binding_lambda(rb) == db.toplevel_lambda_)
							{
								Expr* lhs = expr->call.args[0];
								Expr* rhs = expr->call.args[1];
								Opcode sc_op = op;
								bool has_sc = (rhs->kind == ExprKind::NumberLit);
								if (has_sc)
								{
									switch (op)
									{
										case Opcode::sub2ss: sc_op = Opcode::sub2sc; break;
										case Opcode::add2ss: sc_op = Opcode::add2sc; break;
										case Opcode::eq2ss:  sc_op = Opcode::eq2sc;  break;
										case Opcode::lt2ss:  sc_op = Opcode::lt2sc;  break;
										default:             has_sc = false;         break;
									}
								}
								if (has_sc)
								{
									emit_expr(lhs);
									uint16_t idx = intern_literal_key(rhs);
									emit_opcode(sc_op);
									OP_binop_sc spec{};
									spec.idx = idx;
									emit(spec);
								}
								else
								{
									emit_expr(lhs);
									emit_expr(rhs);
									emit_opcode(op);
								}
								break;
							}
						}
					}
					if (proc->kind == ExprKind::VarRef && proc->var_ref.name == "ref" && nargs == 2)
					{
						ResolvedBinding rb = db.binding(proc);
						if (db.binding_lambda(rb) == db.toplevel_lambda_)
						{
							Expr* obj_expr = expr->call.args[0];
							Expr* key_expr = expr->call.args[1];
							bool ck = is_literal_key(key_expr);
							if (obj_expr->kind == ExprKind::VarRef && !db.const_fold_decisions_[obj_expr->id])
							{
								Expr* current = outer_lambdas.back();
								ResolvedBinding ob = db.binding(obj_expr);
								Expr* orig_owner = ob.lambda;
								size_t orig_breadth = ob.breadth;
								bool slot = needs_slot(orig_owner, static_cast<uint32_t>(orig_breadth));
								ob = translate_binding(ob);
								if (ob.lambda == current && !slot)
								{
									if (ck)
									{
										emit_opcode(Opcode::ref_local_field_ck);
										OP_ref_local_field_ck op{};
										op.off = narrow_off(ob.breadth);
										op.key_idx = intern_literal_key(key_expr);
										emit(op);
									}
									else
									{
										emit_expr(key_expr);
										emit_opcode(Opcode::ref_local_field);
										OP_ref_local_field op{};
										op.off = narrow_off(ob.breadth);
										emit(op);
									}
									break;
								}
								if (ob.lambda != current)
								{
									uint32_t upv = find_upvalue(current, ob.lambda,
																static_cast<uint32_t>(ob.breadth));
									if (upv != UINT32_MAX)
									{
										if (ck)
										{
											emit_opcode(slot ? Opcode::ref_upvalue_slot_field_ck
															 : Opcode::ref_upvalue_direct_field_ck);
											OP_ref_upvalue_field_ck op{};
											op.idx = static_cast<uint16_t>(upv);
											op.key_idx = intern_literal_key(key_expr);
											emit(op);
										}
										else
										{
											emit_expr(key_expr);
											emit_opcode(slot ? Opcode::ref_upvalue_slot_field
															 : Opcode::ref_upvalue_direct_field);
											OP_ref_upvalue_field op{};
											op.idx = static_cast<uint16_t>(upv);
											emit(op);
										}
										break;
									}
								}
							}
							emit_expr(obj_expr);
							if (ck)
							{
								emit_opcode(Opcode::ref_field_ck);
								OP_ref_field_ck op{};
								op.key_idx = intern_literal_key(key_expr);
								emit(op);
							}
							else
							{
								emit_expr(key_expr);
								emit_opcode(Opcode::ref_field);
								OP_ref_field op{};
								emit(op);
							}
							break;
						}
					}

					Expr* current = outer_lambdas.back();
					enum class IcKind : uint8_t
					{
						None,
						Slot,
						Direct
					};
					IcKind ic_kind = IcKind::None;
					uint16_t slot_upvalue_idx = 0;
					uint8_t direct_src = 0;
					uint16_t direct_idx = 0;
					if (proc->kind == ExprKind::VarRef)
					{
						ResolvedBinding cb = db.binding(proc);
						Expr* orig_owner = cb.lambda;
						size_t orig_breadth = cb.breadth;
						cb = translate_binding(cb);
						bool slot = needs_slot(orig_owner, static_cast<uint32_t>(orig_breadth));
						if (cb.lambda != current && slot)
						{
							uint32_t found =
								find_upvalue(current, cb.lambda, static_cast<uint32_t>(cb.breadth));
							CITY_DIE_WHEN(found == UINT32_MAX,
										  "codegen: cacheable call missing upvalue entry");
							slot_upvalue_idx = static_cast<uint16_t>(found);
							ic_kind = IcKind::Slot;
						}
						else if (!slot)
						{
							ic_kind = IcKind::Direct;
							if (cb.lambda == current)
							{
								direct_src = 0;
								direct_idx = static_cast<uint16_t>(cb.breadth);
							}
							else
							{
								uint32_t found =
									find_upvalue(current, cb.lambda, static_cast<uint32_t>(cb.breadth));
								CITY_DIE_WHEN(found == UINT32_MAX,
											  "codegen: cacheable call missing upvalue entry");
								direct_src = 1;
								direct_idx = static_cast<uint16_t>(found);
							}
						}
					}

					if (ic_kind == IcKind::Slot)
					{
						size_t last_arg_pos = 0;
						for (size_t i = 0; i < expr->call.args.size(); ++i)
						{
							last_arg_pos = bc.size();
							emit_expr(expr->call.args[i]);
						}
						if (!expr->call.args.empty() &&
							bc.size() - last_arg_pos == OPCODE_SIZE + sizeof(OP_ref_local) &&
							bc[last_arg_pos] == static_cast<uint8_t>(Opcode::ref_local))
						{
							OP_ref_local prev =
								*reinterpret_cast<OP_ref_local*>(&bc[last_arg_pos + OPCODE_SIZE]);
							bc.resize(last_arg_pos);
							emit_replicated(Opcode::call_ic_slot_local_0, v_call_ic_slot_local);
							OP_call_ic_slot_local op{};
							op.local_off = prev.off;
							op.upvalue_idx = slot_upvalue_idx;
							op.tail = tail;
							op.nargs = nargs;
							emit(op);
						}
						else
						{
							emit_replicated(Opcode::call_ic_slot_0, v_call_ic_slot);
							OP_call_ic_slot op{};
							op.upvalue_idx = slot_upvalue_idx;
							op.tail = tail;
							op.nargs = nargs;
							emit(op);
						}
					}
					else if (ic_kind == IcKind::Direct)
					{
						for (Expr* arg : expr->call.args)
						{
							emit_expr(arg);
						}
						emit_replicated(Opcode::call_ic_direct_0, v_call_ic_direct);
						OP_call_ic_direct op{};
						op.src = direct_src;
						op.idx = direct_idx;
						op.tail = tail;
						op.nargs = nargs;
						emit(op);
					}
					else
					{
						emit_expr(expr->call.proc);
						for (Expr* arg : expr->call.args)
						{
							emit_expr(arg);
						}
						emit_opcode(Opcode::call);
						OP_call op{};
						op.tail = tail;
						op.nargs = nargs;
						emit(op);
					}
					break;
				}

				case ExprKind::Apply:
					emit_expr(expr->apply.proc);
					emit_expr(expr->apply.args);
					emit_opcode(Opcode::apply);
					break;

				case ExprKind::Lambda:
					emit_lambda_value(expr);
					break;

				case ExprKind::PrimRef:
					emit_ldc(intern_global_name(expr->prim_ref.name));
					break;

				case ExprKind::SetBang:
					emit_expr(expr->set_bang.value);
					emit_var_ref_or_set(db.binding(expr), true);
					break;

				case ExprKind::SetRef:
				{
					Expr* obj_expr = expr->set_ref.obj;
					Expr* key_expr = expr->set_ref.key;
					Expr* val_expr = expr->set_ref.value;
					bool ck = is_literal_key(key_expr);
					if (obj_expr->kind == ExprKind::VarRef && !db.const_fold_decisions_[obj_expr->id])
					{
						Expr* current = outer_lambdas.back();
						ResolvedBinding ob = db.binding(obj_expr);
						Expr* orig_owner = ob.lambda;
						size_t orig_breadth = ob.breadth;
						bool slot = needs_slot(orig_owner, static_cast<uint32_t>(orig_breadth));
						ob = translate_binding(ob);
						if (ob.lambda == current && !slot)
						{
							if (ck)
							{
								emit_expr(val_expr);
								emit_opcode(Opcode::set_local_field_ck);
								OP_set_local_field_ck op{};
								op.off = narrow_off(ob.breadth);
								op.key_idx = intern_literal_key(key_expr);
								emit(op);
							}
							else
							{
								emit_expr(key_expr);
								emit_expr(val_expr);
								emit_opcode(Opcode::set_local_field);
								OP_set_local_field op{};
								op.off = narrow_off(ob.breadth);
								emit(op);
							}
							break;
						}
						if (ob.lambda != current)
						{
							uint32_t upv = find_upvalue(current, ob.lambda,
														static_cast<uint32_t>(ob.breadth));
							if (upv != UINT32_MAX)
							{
								if (ck)
								{
									emit_expr(val_expr);
									emit_opcode(slot ? Opcode::set_upvalue_slot_field_ck
													 : Opcode::set_upvalue_direct_field_ck);
									OP_set_upvalue_field_ck op{};
									op.idx = static_cast<uint16_t>(upv);
									op.key_idx = intern_literal_key(key_expr);
									emit(op);
								}
								else
								{
									emit_expr(key_expr);
									emit_expr(val_expr);
									emit_opcode(slot ? Opcode::set_upvalue_slot_field
													 : Opcode::set_upvalue_direct_field);
									OP_set_upvalue_field op{};
									op.idx = static_cast<uint16_t>(upv);
									emit(op);
								}
								break;
							}
						}
					}
					emit_expr(obj_expr);
					if (ck)
					{
						emit_expr(val_expr);
						emit_opcode(Opcode::set_field_ck);
						OP_set_field_ck op{};
						op.key_idx = intern_literal_key(key_expr);
						emit(op);
					}
					else
					{
						emit_expr(key_expr);
						emit_expr(val_expr);
						emit_opcode(Opcode::set_field);
						OP_set_field op{};
						emit(op);
					}
					break;
				}

				case ExprKind::If:
				{
					emit_expr(expr->if_.test);
					emit_opcode(Opcode::if_then_else);
					size_t cons_hole = emit_size_hole();

					emit_expr(expr->if_.consequent);
					emit_opcode(Opcode::skip);

					patch_size(cons_hole, bc.size() + sizeof(size_t));

					size_t alt_hole = emit_size_hole();
					if (expr->if_.alternate)
					{
						emit_expr(expr->if_.alternate);
					}
					else
					{
						emit_ldc(intern_empty(ConstTag::Unknown));
					}
					patch_size(alt_hole, bc.size());
					break;
				}

				case ExprKind::Begin:
					emit_sequence(expr->begin.body);
					break;

				default:
					CITY_DIE("%d:%d: codegen: unhandled ExprKind %d (surface form not expanded?)",
							 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
			}
		}
	};

	Atom datum_to_atom(Expr* e)
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			{
				double v = strtod_l(std::string{e->number_lit.text}.c_str(), nullptr, c_locale);
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
				CITY_DIE_UNLESS(proc->kind == ExprKind::VarRef, "datum_to_atom: bad call proc");
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
					CITY_DIE_UNLESS(e->call.args.size() == 2, "datum_to_atom: cons arity");
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
				CITY_DIE("datum_to_atom: unexpected call proc");
			}
			default:
				CITY_DIE("datum_to_atom: unexpected ExprKind %d", static_cast<int>(e->kind));
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
		// Hoist toplevel defines: rewrite (define x v) to (set! x v) and
		// collect the names. The loader pre-allocates one slot per name
		// from the bytecode header before any code runs.
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = rewrite_define_in(ast_->forms[i], toplevel_names_);
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

	CITY_DIE("%d:%d: unresolved binding", expr->loc.line, expr->loc.col);
}

bool Compiler::is_tail(Expr* expr)
{
	ast();
	return tail_cache_[expr->id];
}

Bytecode Compiler::compile()
{
	Program& program = ast();
	Bytecode code;
	Emitter emitter{code, *this};

	emitter.emit_prologue(toplevel_lambda_);
	for (Expr* form : program.forms)
	{
		emitter.emit_expr(form);
	}
	emitter.emit_opcode(Opcode::ret);

	// [u32 n_toplevel_slots][u32 n_constants][concatenated pool entries][code...]
	Bytecode out;
	uint32_t n_slots = emitter.max_locals_stack.back();
	uint8_t* sp = reinterpret_cast<uint8_t*>(&n_slots);
	out.insert(out.end(), sp, sp + sizeof(n_slots));
	uint32_t n = static_cast<uint32_t>(emitter.pool.size());
	uint8_t* np = reinterpret_cast<uint8_t*>(&n);
	out.insert(out.end(), np, np + sizeof(n));
	for (std::string& entry : emitter.pool)
	{
		out.insert(out.end(), entry.begin(), entry.end());
	}
	out.insert(out.end(), code.begin(), code.end());
	return out;
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
		{
			// (when test body...) -> (if test (begin body...))
			expr->when.test = expand(expr->when.test);
			Expr* begin_e = arena.alloc<Expr>();
			begin_e->kind = ExprKind::Begin;
			begin_e->id = expr->id;
			begin_e->loc = expr->loc;
			begin_e->begin.body = expr->when.body;
			begin_e = expand_begin(begin_e);

			Expr* if_e = arena.alloc<Expr>();
			if_e->kind = ExprKind::If;
			if_e->id = expr->id;
			if_e->loc = expr->loc;
			if_e->if_.test = expr->when.test;
			if_e->if_.consequent = begin_e;
			if_e->if_.alternate = nullptr;
			return if_e;
		}

		case ExprKind::Unless:
		{
			// (unless test body...) -> (if test <void> (begin body...))
			expr->unless.test = expand(expr->unless.test);
			Expr* begin_e = arena.alloc<Expr>();
			begin_e->kind = ExprKind::Begin;
			begin_e->id = expr->id;
			begin_e->loc = expr->loc;
			begin_e->begin.body = expr->unless.body;
			begin_e = expand_begin(begin_e);

			Expr* if_e = arena.alloc<Expr>();
			if_e->kind = ExprKind::If;
			if_e->id = expr->id;
			if_e->loc = expr->loc;
			if_e->if_.test = expr->unless.test;
			if_e->if_.consequent = arena.alloc<Expr>();
			if_e->if_.alternate = begin_e;
			return if_e;
		}

		case ExprKind::And:
		{
			Slice<Expr*>& exprs = expr->and_.exprs;
			if (exprs.empty())
			{
				Expr* t = arena.alloc<Expr>();
				t->kind = ExprKind::BooleanLit;
				t->id = expr->id;
				t->loc = expr->loc;
				t->boolean_lit.value = true;
				return t;
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				Expr* if_e = arena.alloc<Expr>();
				if_e->kind = ExprKind::If;
				if_e->id = expr->id;
				if_e->loc = exprs[i]->loc;
				if_e->if_.test = expand(exprs[i]);
				if_e->if_.consequent = result;
				Expr* f = arena.alloc<Expr>();
				f->kind = ExprKind::BooleanLit;
				f->id = expr->id;
				f->loc = expr->loc;
				f->boolean_lit.value = false;
				if_e->if_.alternate = f;
				result = if_e;
			}
			return result;
		}

		case ExprKind::Or:
		{
			Slice<Expr*>& exprs = expr->or_.exprs;
			if (exprs.empty())
			{
				Expr* f = arena.alloc<Expr>();
				f->kind = ExprKind::BooleanLit;
				f->id = expr->id;
				f->loc = expr->loc;
				f->boolean_lit.value = false;
				return f;
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			// Build right-to-left: ((lambda (t) (if t t rest)) expr[i])
			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				std::string_view tmp_name = arena.copy_string("%or-tmp");
				Expr* ref1 = arena.alloc<Expr>();
				ref1->kind = ExprKind::VarRef;
				ref1->id = next_expr_id_++;
				ref1->loc = expr->loc;
				ref1->var_ref.name = tmp_name;
				Expr* ref2 = arena.alloc<Expr>();
				ref2->kind = ExprKind::VarRef;
				ref2->id = next_expr_id_++;
				ref2->loc = expr->loc;
				ref2->var_ref.name = tmp_name;

				Expr* if_e = arena.alloc<Expr>();
				if_e->kind = ExprKind::If;
				if_e->id = next_expr_id_++;
				if_e->loc = exprs[i]->loc;
				if_e->if_.test = ref1;
				if_e->if_.consequent = ref2;
				if_e->if_.alternate = result;

				std::string_view* params = arena.alloc_array<std::string_view>(1);
				params[0] = tmp_name;
				Expr** body_arr = arena.alloc_array<Expr*>(1);
				body_arr[0] = if_e;
				Expr* lam = arena.alloc<Expr>();
				lam->kind = ExprKind::Lambda;
				lam->id = next_expr_id_++;
				lam->loc = expr->loc;
				lam->lambda.params = {params, 1};
				lam->lambda.is_variadic = false;
				lam->lambda.body = {body_arr, 1};

				Expr** args = arena.alloc_array<Expr*>(1);
				args[0] = expand(exprs[i]);
				Expr* call = arena.alloc<Expr>();
				call->kind = ExprKind::Call;
				call->id = next_expr_id_++;
				call->loc = expr->loc;
				call->call.proc = lam;
				call->call.args = {args, 1};
				result = call;
			}
			return result;
		}

		case ExprKind::Cond:
		{
			Slice<Expr*>& clauses = expr->cond.clauses;
			if (clauses.empty())
			{
				return arena.alloc<Expr>();
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
				Expr* if_e = arena.alloc<Expr>();
				if_e->kind = ExprKind::If;
				if_e->id = expr->id;
				if_e->loc = test->loc;
				if_e->if_.test = test;
				if_e->if_.consequent = body;
				if_e->if_.alternate = result;
				result = if_e;
			}
			return result ? result : arena.alloc<Expr>();
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

	// Desugar (let ((x v) (y w)) body...) into ((lambda (x y) body...) v w)
	// so the rest of the pipeline can treat it like any stack-call site.
	Expr* lambda = arena.alloc<Expr>();
	lambda->kind = ExprKind::Lambda;
	lambda->id = next_expr_id_++;
	lambda->loc = expr->loc;
	lambda->lambda.params = expr->let.names;
	lambda->lambda.is_variadic = false;
	lambda->lambda.body = expr->let.body;

	Expr* call = arena.alloc<Expr>();
	call->kind = ExprKind::Call;
	call->id = next_expr_id_++;
	call->loc = expr->loc;
	call->call.proc = lambda;
	call->call.args = expr->let.vals;
	return call;
}

Expr* Compiler::expand_begin(Expr* expr)
{
	for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
	{
		expr->begin.body[i] = expand(expr->begin.body[i]);
	}
	return expr;
}

// In-place: replace any (define x v) reachable from `expr` (descending only
// through Begin) with (set! x v); push each x onto `names`. Other Expr kinds
// are opaque -- they introduce their own scope or aren't sequence sugar.
Expr* Compiler::rewrite_define_in(Expr* expr, OrderedNameSet& names)
{
	if (expr->kind == ExprKind::Define)
	{
		bool inserted = names.insert(expr->define.name);
		Expr* set_e = arena.alloc<Expr>();
		set_e->kind = ExprKind::SetBang;
		set_e->id = next_expr_id_++;
		set_e->loc = expr->loc;
		set_e->set_bang.name = expr->define.name;
		set_e->set_bang.value = expr->define.value;
		set_e->set_bang.is_init = inserted;
		return set_e;
	}
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

	std::string_view* params = arena.alloc_array<std::string_view>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		params[i] = names.ordered[i];
	}

	Expr* lam = arena.alloc<Expr>();
	lam->kind = ExprKind::Lambda;
	lam->id = next_expr_id_++;
	lam->loc = loc;
	lam->lambda.params = {params, n};
	lam->lambda.is_variadic = false;
	lam->lambda.body = body;

	Expr** args = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		Expr* f = arena.alloc<Expr>();
		f->kind = ExprKind::BooleanLit;
		f->id = next_expr_id_++;
		f->loc = loc;
		f->boolean_lit.value = false;
		args[i] = f;
	}

	Expr* call = arena.alloc<Expr>();
	call->kind = ExprKind::Call;
	call->id = next_expr_id_++;
	call->loc = loc;
	call->call.proc = lam;
	call->call.args = {args, n};

	Expr** new_body = arena.alloc_array<Expr*>(1);
	new_body[0] = call;
	return {new_body, 1};
}

// (letrec ((x1 e1) ... (xn en)) body...)
//   ==>
// (let ((x1 #f) ... (xn #f))
//   (set! x1 e1) ... (set! xn en)
//   body...)
// Sequential set!s give letrec* semantics, which is what almost all uses
// of letrec actually want and matches what we accept for both keywords.
Expr* Compiler::expand_letrec(Expr* expr)
{
	uint32_t n = expr->let.names.size();

	Expr** sentinels = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		Expr* f = arena.alloc<Expr>();
		f->kind = ExprKind::BooleanLit;
		f->id = next_expr_id_++;
		f->loc = expr->loc;
		f->boolean_lit.value = false;
		sentinels[i] = f;
	}

	uint32_t body_n = n + expr->let.body.size();
	Expr** new_body = arena.alloc_array<Expr*>(body_n);
	for (uint32_t i = 0; i < n; ++i)
	{
		Expr* set_e = arena.alloc<Expr>();
		set_e->kind = ExprKind::SetBang;
		set_e->id = next_expr_id_++;
		set_e->loc = expr->let.vals[i]->loc;
		set_e->set_bang.name = expr->let.names[i];
		set_e->set_bang.value = expr->let.vals[i];
		set_e->set_bang.is_init = true;
		new_body[i] = set_e;
	}
	for (uint32_t i = 0; i < expr->let.body.size(); ++i)
	{
		new_body[n + i] = expr->let.body[i];
	}

	Expr* let_e = arena.alloc<Expr>();
	let_e->kind = ExprKind::Let;
	let_e->id = next_expr_id_++;
	let_e->loc = expr->loc;
	let_e->let.names = expr->let.names;
	let_e->let.vals = {sentinels, n};
	let_e->let.body = {new_body, body_n};

	return expand_let(let_e);
}

Expr* Compiler::binding_lambda(ResolvedBinding b)
{
	return b.lambda;
}

// Marks the binding as captured in its owner and registers it as an upvalue
// in every transit lambda between owner and the current scope, so codegen
// can later wire each transit lambda's make_closure to forward the Slot.
void Compiler::record_ref(ResolvedBinding b)
{
	if (lambdas_.back() == b.lambda)
	{
		return;
	}

	std::vector<bool>& capture = lambda_scopes_[b.lambda].captured;
	if (capture.size() <= b.breadth)
	{
		capture.resize(b.breadth + 1, false);
	}
	capture[b.breadth] = true;

	uint32_t bw = static_cast<uint32_t>(b.breadth);
	uint64_t key = (static_cast<uint64_t>(b.lambda->id) << 32) | bw;
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		Expr* lam = lambdas_[i];
		if (lam == b.lambda)
		{
			return;
		}
		LambdaScope& sc = lambda_scopes_[lam];
		if (sc.upvalue_keys.insert(key).second)
		{
			sc.upvalues.push_back({b.lambda, bw});
		}
	}
	CITY_DIE("record_ref: owner not in lambdas_");
}

void Compiler::record_set(ResolvedBinding b, bool is_init, Expr* value)
{
	LambdaScope& scope = lambda_scopes_[b.lambda];
	if (scope.mutated.size() <= b.breadth)
	{
		scope.mutated.resize(b.breadth + 1, false);
	}
	scope.mutated[b.breadth] = true;
	if (!is_init)
	{
		if (scope.reassigned_after_init.size() <= b.breadth)
		{
			scope.reassigned_after_init.resize(b.breadth + 1, false);
		}
		scope.reassigned_after_init[b.breadth] = true;
	}
	if (is_init && value)
	{
		if (scope.bound_init.size() <= b.breadth)
		{
			scope.bound_init.resize(b.breadth + 1, nullptr);
		}
		scope.bound_init[b.breadth] = value;
	}
}

std::optional<ResolvedBinding> Compiler::lookup_name(std::string_view name)
{
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_[i];
		std::unordered_map<std::string_view, size_t>::iterator it = idx.find(name);
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

static Slice<std::string_view> make_names_slice(Arena& arena, std::vector<std::string_view>& src)
{
	std::string_view* data = arena.alloc_array<std::string_view>(src.size());
	for (size_t i = 0; i < src.size(); ++i)
	{
		data[i] = src[i];
	}
	return {data, static_cast<uint32_t>(src.size())};
}

void Compiler::freeze_lambda(Expr* lambda)
{
	LambdaScope& scope = lambda_scopes_[lambda];

	UpvalueRef* up_data = arena.alloc_array<UpvalueRef>(scope.upvalues.size());
	for (size_t i = 0; i < scope.upvalues.size(); ++i)
	{
		up_data[i] = scope.upvalues[i];
	}
	lambda->lambda.upvalues = {up_data, static_cast<uint32_t>(scope.upvalues.size())};

	uint32_t n = lambda->lambda.names.size();
	bool* captured_data = arena.alloc_array<bool>(n);
	bool* mutated_data = arena.alloc_array<bool>(n);
	bool* reassigned_data = arena.alloc_array<bool>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		captured_data[i] = i < scope.captured.size() ? scope.captured[i] : false;
		mutated_data[i] = i < scope.mutated.size() ? scope.mutated[i] : false;
		reassigned_data[i] = i < scope.reassigned_after_init.size() ? scope.reassigned_after_init[i] : false;
	}
	lambda->lambda.captured_locals = {captured_data, n};
	lambda->lambda.mutated_locals = {mutated_data, n};
	lambda->lambda.reassigned_after_init_locals = {reassigned_data, n};
}

namespace
{

	constexpr uint32_t INLINE_BUDGET = 20;

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
			case ExprKind::Begin:
				n += count_exprs_slice(e->begin.body);
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
			case ExprKind::Define:
				n += count_exprs(e->define.value);
				break;
			default:
				break;
		}
		return n;
	}

	struct InlinerWalk
	{
		Compiler& db;
		std::vector<Expr*>& lambda_candidates;
		std::vector<Expr*>& const_candidates;
		std::vector<std::vector<Expr*>>& caller_outer_lambdas;
		std::vector<Expr*> outer_lambdas;

		void walk(Expr* e)
		{
			if (!e)
			{
				return;
			}
			switch (e->kind)
			{
				case ExprKind::Lambda:
				{
					outer_lambdas.push_back(e);
					for (uint32_t i = 0; i < e->lambda.body.size(); ++i)
					{
						walk(e->lambda.body[i]);
					}
					outer_lambdas.pop_back();
					break;
				}
				case ExprKind::VarRef:
				{
					ResolvedBinding rb = db.bindings_[e->id];
					if (rb.lambda == nullptr)
					{
						break;
					}
					if (rb.lambda != db.toplevel_lambda_)
					{
						break;
					}
					if (rb.breadth >= const_candidates.size())
					{
						break;
					}
					Expr* lit = const_candidates[rb.breadth];
					if (!lit)
					{
						break;
					}
					db.const_fold_decisions_[e->id] = lit;
					break;
				}
				case ExprKind::Call:
				{
					walk(e->call.proc);
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						walk(e->call.args[i]);
					}
					Expr* proc = e->call.proc;
					if (proc->kind != ExprKind::VarRef)
					{
						break;
					}
					ResolvedBinding rb = db.bindings_[proc->id];
					if (rb.lambda == nullptr)
					{
						break;
					}
					if (rb.lambda != db.toplevel_lambda_)
					{
						break;
					}
					if (rb.breadth >= lambda_candidates.size())
					{
						break;
					}
					Expr* callee = lambda_candidates[rb.breadth];
					if (!callee)
					{
						break;
					}
					if (callee->lambda.params.size() != e->call.args.size())
					{
						break;
					}
					bool on_outer_lambdas = false;
					for (Expr* lam : outer_lambdas)
					{
						if (lam == callee)
						{
							on_outer_lambdas = true;
							break;
						}
					}
					if (on_outer_lambdas)
					{
						break;
					}
					db.inline_decisions_[e->id] = callee;
					caller_outer_lambdas[e->id] = outer_lambdas;
					break;
				}
				case ExprKind::Apply:
					walk(e->apply.proc);
					walk(e->apply.args);
					break;
				case ExprKind::If:
					walk(e->if_.test);
					walk(e->if_.consequent);
					walk(e->if_.alternate);
					break;
				case ExprKind::Begin:
					for (uint32_t i = 0; i < e->begin.body.size(); ++i)
					{
						walk(e->begin.body[i]);
					}
					break;
				case ExprKind::SetBang:
					walk(e->set_bang.value);
					break;
				case ExprKind::SetRef:
					walk(e->set_ref.obj);
					walk(e->set_ref.key);
					walk(e->set_ref.value);
					break;
				default:
					break;
			}
		}
	};

} // namespace

void Compiler::run_inliner()
{
	LambdaScope& tl = lambda_scopes_[toplevel_lambda_];
	uint32_t n_top = toplevel_lambda_->lambda.names.size();
	std::vector<Expr*> lambda_candidates(n_top, nullptr);
	std::vector<Expr*> const_candidates(n_top, nullptr);

	for (uint32_t i = 0; i < tl.bound_init.size() && i < n_top; ++i)
	{
		Expr* init = tl.bound_init[i];
		if (!init)
		{
			continue;
		}
		if (i < tl.reassigned_after_init.size() && tl.reassigned_after_init[i])
		{
			continue;
		}
		if (init->kind == ExprKind::Lambda)
		{
			if (init->lambda.is_variadic)
			{
				continue;
			}
			if (count_exprs_slice(init->lambda.body) > INLINE_BUDGET)
			{
				continue;
			}
			lambda_candidates[i] = init;
		}
		else if (init->kind == ExprKind::NumberLit || init->kind == ExprKind::BooleanLit ||
				 init->kind == ExprKind::CharacterLit)
		{
			const_candidates[i] = init;
		}
	}

	std::vector<std::vector<Expr*>> caller_outer_lambdas(next_expr_id_ + 1);
	InlinerWalk w{
		.db = *this,
		.lambda_candidates = lambda_candidates,
		.const_candidates = const_candidates,
		.caller_outer_lambdas = caller_outer_lambdas,
		.outer_lambdas = {},
	};
	w.outer_lambdas.push_back(toplevel_lambda_);
	for (Expr* form : ast_->forms)
	{
		w.walk(form);
	}
	w.outer_lambdas.pop_back();

	bool changed = true;
	while (changed)
	{
		changed = false;
		for (ExprId call_id = 0; call_id < inline_decisions_.size(); ++call_id)
		{
			Expr* callee = inline_decisions_[call_id];
			if (!callee)
			{
				continue;
			}
			std::vector<Expr*>& caller_outers = caller_outer_lambdas[call_id];
			std::vector<UpvalueRef>& callee_ups = lambda_scopes_[callee].upvalues;
			for (UpvalueRef u : callee_ups)
			{
				std::vector<bool>& cap = lambda_scopes_[u.owner].captured;
				if (cap.size() <= u.breadth)
				{
					cap.resize(u.breadth + 1, false);
				}
				if (!cap[u.breadth])
				{
					cap[u.breadth] = true;
					changed = true;
				}
				uint64_t key = (static_cast<uint64_t>(u.owner->id) << 32) | u.breadth;
				for (size_t i = caller_outers.size(); i-- > 0;)
				{
					Expr* lam = caller_outers[i];
					if (lam == u.owner)
					{
						break;
					}
					LambdaScope& sc = lambda_scopes_[lam];
					if (sc.upvalue_keys.insert(key).second)
					{
						sc.upvalues.push_back(u);
						changed = true;
					}
				}
			}
		}
	}
}

void Compiler::resolve_bindings(Program& program)
{
	bindings_.assign(next_expr_id_ + 1, ResolvedBinding{});
	tail_cache_.assign(next_expr_id_ + 1, false);
	inline_decisions_.assign(next_expr_id_ + 1, nullptr);
	const_fold_decisions_.assign(next_expr_id_ + 1, nullptr);

	// Synthesize a toplevel lambda. Its names are the hoisted top-level
	// binding names, in source order; nested lambdas reach those bindings as
	// upvalues just like any other.
	toplevel_lambda_ = arena.alloc<Expr>();
	toplevel_lambda_->kind = ExprKind::Lambda;
	toplevel_lambda_->id = next_expr_id_++;
	toplevel_lambda_->loc = {};
	toplevel_lambda_->lambda.params = {};
	toplevel_lambda_->lambda.is_variadic = false;
	toplevel_lambda_->lambda.body = {};
	toplevel_lambda_->lambda.names = make_names_slice(arena, toplevel_names_.ordered);

	push_lambda_scope(toplevel_lambda_);
	for (Expr* form : program.forms)
	{
		resolve_bindings_in(form);
	}
	pop_lambda_scope();
	all_lambdas_.push_back(toplevel_lambda_);

	if (!no_inline_)
	{
		run_inliner();
	}

	for (Expr* L : all_lambdas_)
	{
		freeze_lambda(L);
	}

	for (Expr* form : program.forms)
	{
		compute_tail(form, false);
	}
}

void Compiler::resolve_bindings_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
			break;

		case ExprKind::VarRef:
		{
			std::optional<ResolvedBinding> b = lookup_name(expr->var_ref.name);
			if (b)
			{
				bindings_[expr->id] = *b;
				record_ref(*b);
			}
			else
			{
				CITY_DIE("%d:%d: unresolved variable '%.*s'", expr->loc.line, expr->loc.col,
						 static_cast<int>(expr->var_ref.name.size()), expr->var_ref.name.data());
			}
			break;
		}

		case ExprKind::Call:
			resolve_bindings_in(expr->call.proc);
			for (Expr* arg : expr->call.args)
			{
				resolve_bindings_in(arg);
			}
			break;

		case ExprKind::Apply:
			resolve_bindings_in(expr->apply.proc);
			resolve_bindings_in(expr->apply.args);
			break;

		case ExprKind::Lambda:
		{
			std::vector<std::string_view> names;
			for (std::string_view param : expr->lambda.params)
			{
				names.push_back(param);
			}
			expr->lambda.names = make_names_slice(arena, names);

			push_lambda_scope(expr);
			for (Expr* form : expr->lambda.body)
			{
				resolve_bindings_in(form);
			}
			pop_lambda_scope();
			all_lambdas_.push_back(expr);
			break;
		}

		case ExprKind::PrimRef:
			break;

		case ExprKind::SetBang:
		{
			resolve_bindings_in(expr->set_bang.value);
			std::optional<ResolvedBinding> b = lookup_name(expr->set_bang.name);
			if (b)
			{
				bindings_[expr->id] = *b;
				record_ref(*b);
				record_set(*b, expr->set_bang.is_init, expr->set_bang.value);
			}
			else
			{
				CITY_DIE("%d:%d: unresolved variable '%.*s' in set!", expr->loc.line, expr->loc.col,
						 static_cast<int>(expr->set_bang.name.size()), expr->set_bang.name.data());
			}
			break;
		}

		case ExprKind::SetRef:
			resolve_bindings_in(expr->set_ref.obj);
			resolve_bindings_in(expr->set_ref.key);
			resolve_bindings_in(expr->set_ref.value);
			break;

		case ExprKind::If:
			resolve_bindings_in(expr->if_.test);
			resolve_bindings_in(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				resolve_bindings_in(expr->if_.alternate);
			}
			break;

		case ExprKind::Begin:
			for (Expr* form : expr->begin.body)
			{
				resolve_bindings_in(form);
			}
			break;

		default:
			CITY_DIE("%d:%d: resolve: unhandled ExprKind %d (surface form not expanded?)",
					 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
	}
}

void Compiler::compute_tail(Expr* expr, bool in_tail)
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
			if (in_tail)
			{
				tail_cache_[expr->id] = true;
			}
			compute_tail(expr->call.proc, false);
			for (Expr* arg : expr->call.args)
			{
				compute_tail(arg, false);
			}
			break;

		case ExprKind::Apply:
			compute_tail(expr->apply.proc, false);
			compute_tail(expr->apply.args, false);
			break;

		case ExprKind::Lambda:
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				bool is_last = (i == expr->lambda.body.size() - 1);
				compute_tail(expr->lambda.body[i], is_last);
			}
			break;

		case ExprKind::Define:
			compute_tail(expr->define.value, false);
			break;

		case ExprKind::SetBang:
			compute_tail(expr->set_bang.value, false);
			break;

		case ExprKind::SetRef:
			compute_tail(expr->set_ref.obj, false);
			compute_tail(expr->set_ref.key, false);
			compute_tail(expr->set_ref.value, false);
			break;

		case ExprKind::If:
			compute_tail(expr->if_.test, false);
			compute_tail(expr->if_.consequent, in_tail);
			if (expr->if_.alternate)
			{
				compute_tail(expr->if_.alternate, in_tail);
			}
			break;

		case ExprKind::Begin:
			for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
			{
				bool is_last = (i == expr->begin.body.size() - 1);
				compute_tail(expr->begin.body[i], is_last && in_tail);
			}
			break;

		default:
			CITY_DIE("%d:%d: tail-pass: unhandled ExprKind %d (surface form not expanded?)",
					 expr->loc.line, expr->loc.col, static_cast<int>(expr->kind));
	}
}

Bytecode compile(std::string source, std::string filename, bool no_inline)
{
	Compiler compiler;
	compiler.source = std::move(source);
	compiler.filename = std::move(filename);
	compiler.no_inline_ = no_inline;
	return compiler.compile();
}


void init_reader(Env& e)
{
	e.bind("read", make_prim<read_port>());
}
