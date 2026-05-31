/**************************************************************************/
/*  lunari_tokenizer.h                                                     */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

class LunariTokenizer {
public:
	struct Token {
		enum Type {
			TK_EMPTY,
			TK_IDENTIFIER,
			TK_LITERAL,
			TK_REQUIRE,
			TK_CLASS,
			TK_DEF,
			TK_END,
			TK_PUBLIC,
			TK_PRIVATE,
			TK_RETURN,
			TK_IF,
			TK_ELSE,
			TK_ELSIF,
			TK_UNLESS,
			TK_WHILE,
			TK_UNTIL,
			TK_FOR,
			TK_IN,
			TK_BREAK,
			TK_NEXT,
			TK_SELF,
			TK_TRUE,
			TK_FALSE,
			TK_NIL,
			TK_COLON,
			TK_DOUBLE_COLON,
			TK_DOT,
			TK_COMMA,
			TK_EQUAL,
			TK_PAREN_OPEN,
			TK_PAREN_CLOSE,
			TK_BRACKET_OPEN,
			TK_BRACKET_CLOSE,
			TK_BRACE_OPEN,
			TK_BRACE_CLOSE,
			TK_PLUS,
			TK_MINUS,
			TK_STAR,
			TK_SLASH,
			TK_PERCENT,
			TK_ARROW,
			TK_NEWLINE,
			TK_ERROR,
			TK_EOF,
		};

		Type type = TK_EMPTY;
		Variant literal;
		String source;
		int line = 1;
		int column = 1;
	};

private:
	String source;
	int pos = 0;
	int line = 1;
	int column = 1;

	bool _is_identifier_start(char32_t p_char) const;
	bool _is_identifier_char(char32_t p_char) const;
	char32_t _peek(int p_offset = 0) const;
	char32_t _advance();
	Token _make_token(Token::Type p_type, const String &p_source = String(), const Variant &p_literal = Variant(), int p_line = -1, int p_column = -1) const;

public:
	static String token_name(Token::Type p_type);
	void set_source_code(const String &p_source);
	Token scan_token();
	Vector<Token> scan_all();
};
