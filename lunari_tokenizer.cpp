/**************************************************************************/
/*  lunari_tokenizer.cpp                                                   */
/**************************************************************************/

#include "lunari_tokenizer.h"

bool LunariTokenizer::_is_identifier_start(char32_t p_char) const {
	return (p_char >= 'A' && p_char <= 'Z') || (p_char >= 'a' && p_char <= 'z') || p_char == '_';
}

bool LunariTokenizer::_is_identifier_char(char32_t p_char) const {
	return _is_identifier_start(p_char) || (p_char >= '0' && p_char <= '9');
}

char32_t LunariTokenizer::_peek(int p_offset) const {
	int index = pos + p_offset;
	return index >= source.length() ? 0 : source[index];
}

char32_t LunariTokenizer::_advance() {
	char32_t c = _peek();
	pos++;
	if (c == '\n') {
		line++;
		column = 1;
	} else {
		column++;
	}
	return c;
}

LunariTokenizer::Token LunariTokenizer::_make_token(Token::Type p_type, const String &p_source, const Variant &p_literal, int p_line, int p_column) const {
	Token token;
	token.type = p_type;
	token.source = p_source;
	token.literal = p_literal;
	token.line = p_line < 0 ? line : p_line;
	token.column = p_column < 0 ? column : p_column;
	return token;
}

String LunariTokenizer::token_name(Token::Type p_type) {
	switch (p_type) {
		case Token::TK_EMPTY: return "EMPTY";
		case Token::TK_IDENTIFIER: return "IDENTIFIER";
		case Token::TK_LITERAL: return "LITERAL";
		case Token::TK_REQUIRE: return "REQUIRE";
		case Token::TK_CLASS: return "CLASS";
		case Token::TK_DEF: return "DEF";
		case Token::TK_END: return "END";
		case Token::TK_PUBLIC: return "PUBLIC";
		case Token::TK_PRIVATE: return "PRIVATE";
		case Token::TK_RETURN: return "RETURN";
		case Token::TK_IF: return "IF";
		case Token::TK_ELSE: return "ELSE";
		case Token::TK_ELSIF: return "ELSIF";
		case Token::TK_UNLESS: return "UNLESS";
		case Token::TK_WHILE: return "WHILE";
		case Token::TK_UNTIL: return "UNTIL";
		case Token::TK_FOR: return "FOR";
		case Token::TK_IN: return "IN";
		case Token::TK_BREAK: return "BREAK";
		case Token::TK_NEXT: return "NEXT";
		case Token::TK_SELF: return "SELF";
		case Token::TK_TRUE: return "TRUE";
		case Token::TK_FALSE: return "FALSE";
		case Token::TK_NIL: return "NIL";
		case Token::TK_COLON: return "COLON";
		case Token::TK_DOUBLE_COLON: return "DOUBLE_COLON";
		case Token::TK_DOT: return "DOT";
		case Token::TK_COMMA: return "COMMA";
		case Token::TK_EQUAL: return "EQUAL";
		case Token::TK_PAREN_OPEN: return "PAREN_OPEN";
		case Token::TK_PAREN_CLOSE: return "PAREN_CLOSE";
		case Token::TK_BRACKET_OPEN: return "BRACKET_OPEN";
		case Token::TK_BRACKET_CLOSE: return "BRACKET_CLOSE";
		case Token::TK_BRACE_OPEN: return "BRACE_OPEN";
		case Token::TK_BRACE_CLOSE: return "BRACE_CLOSE";
		case Token::TK_PLUS: return "PLUS";
		case Token::TK_MINUS: return "MINUS";
		case Token::TK_STAR: return "STAR";
		case Token::TK_SLASH: return "SLASH";
		case Token::TK_PERCENT: return "PERCENT";
		case Token::TK_ARROW: return "ARROW";
		case Token::TK_NEWLINE: return "NEWLINE";
		case Token::TK_ERROR: return "ERROR";
		case Token::TK_EOF: return "EOF";
	}
	return "<unknown>";
}

void LunariTokenizer::set_source_code(const String &p_source) {
	source = p_source;
	pos = 0;
	line = 1;
	column = 1;
}

LunariTokenizer::Token LunariTokenizer::scan_token() {
	while (_peek() == ' ' || _peek() == '\t' || _peek() == '\r') {
		_advance();
	}
	if (_peek() == '#') {
		while (_peek() != 0 && _peek() != '\n') {
			_advance();
		}
	}

	const int start_line = line;
	const int start_column = column;
	char32_t c = _advance();
	if (c == 0) {
		return _make_token(Token::TK_EOF, String(), Variant(), start_line, start_column);
	}
	if (c == '\n') {
		return _make_token(Token::TK_NEWLINE, "\\n", Variant(), start_line, start_column);
	}
	if (c == '"' || c == '\'') {
		char32_t quote = c;
		String value;
		while (_peek() != 0 && _peek() != quote) {
			value += _advance();
		}
		if (_peek() != quote) {
			return _make_token(Token::TK_ERROR, "Unterminated string.", Variant(), start_line, start_column);
		}
		_advance();
		return _make_token(Token::TK_LITERAL, value, value, start_line, start_column);
	}
	if (c >= '0' && c <= '9') {
		String number;
		number += c;
		bool has_dot = false;
		while ((_peek() >= '0' && _peek() <= '9') || _peek() == '.') {
			if (_peek() == '.') {
				has_dot = true;
			}
			number += _advance();
		}
		return _make_token(Token::TK_LITERAL, number, has_dot ? Variant(number.to_float()) : Variant(number.to_int()), start_line, start_column);
	}
	if (_is_identifier_start(c)) {
		String ident;
		ident += c;
		while (_is_identifier_char(_peek())) {
			ident += _advance();
		}
		if (ident == "require") return _make_token(Token::TK_REQUIRE, ident, ident, start_line, start_column);
		if (ident == "class") return _make_token(Token::TK_CLASS, ident, ident, start_line, start_column);
		if (ident == "def") return _make_token(Token::TK_DEF, ident, ident, start_line, start_column);
		if (ident == "end") return _make_token(Token::TK_END, ident, ident, start_line, start_column);
		if (ident == "public") return _make_token(Token::TK_PUBLIC, ident, ident, start_line, start_column);
		if (ident == "private") return _make_token(Token::TK_PRIVATE, ident, ident, start_line, start_column);
		if (ident == "return") return _make_token(Token::TK_RETURN, ident, ident, start_line, start_column);
		if (ident == "if") return _make_token(Token::TK_IF, ident, ident, start_line, start_column);
		if (ident == "else") return _make_token(Token::TK_ELSE, ident, ident, start_line, start_column);
		if (ident == "elsif") return _make_token(Token::TK_ELSIF, ident, ident, start_line, start_column);
		if (ident == "unless") return _make_token(Token::TK_UNLESS, ident, ident, start_line, start_column);
		if (ident == "while") return _make_token(Token::TK_WHILE, ident, ident, start_line, start_column);
		if (ident == "until") return _make_token(Token::TK_UNTIL, ident, ident, start_line, start_column);
		if (ident == "for") return _make_token(Token::TK_FOR, ident, ident, start_line, start_column);
		if (ident == "in") return _make_token(Token::TK_IN, ident, ident, start_line, start_column);
		if (ident == "break") return _make_token(Token::TK_BREAK, ident, ident, start_line, start_column);
		if (ident == "next") return _make_token(Token::TK_NEXT, ident, ident, start_line, start_column);
		if (ident == "self") return _make_token(Token::TK_SELF, ident, ident, start_line, start_column);
		if (ident == "true") return _make_token(Token::TK_TRUE, ident, true, start_line, start_column);
		if (ident == "false") return _make_token(Token::TK_FALSE, ident, false, start_line, start_column);
		if (ident == "nil") return _make_token(Token::TK_NIL, ident, Variant(), start_line, start_column);
		return _make_token(Token::TK_IDENTIFIER, ident, ident, start_line, start_column);
	}

	switch (c) {
		case ':':
			if (_peek() == ':') {
				_advance();
				return _make_token(Token::TK_DOUBLE_COLON, "::", Variant(), start_line, start_column);
			}
			return _make_token(Token::TK_COLON, ":", Variant(), start_line, start_column);
		case '.': return _make_token(Token::TK_DOT, ".", Variant(), start_line, start_column);
		case ',': return _make_token(Token::TK_COMMA, ",", Variant(), start_line, start_column);
		case '=': return _make_token(Token::TK_EQUAL, "=", Variant(), start_line, start_column);
		case '(': return _make_token(Token::TK_PAREN_OPEN, "(", Variant(), start_line, start_column);
		case ')': return _make_token(Token::TK_PAREN_CLOSE, ")", Variant(), start_line, start_column);
		case '[': return _make_token(Token::TK_BRACKET_OPEN, "[", Variant(), start_line, start_column);
		case ']': return _make_token(Token::TK_BRACKET_CLOSE, "]", Variant(), start_line, start_column);
		case '{': return _make_token(Token::TK_BRACE_OPEN, "{", Variant(), start_line, start_column);
		case '}': return _make_token(Token::TK_BRACE_CLOSE, "}", Variant(), start_line, start_column);
		case '+': return _make_token(Token::TK_PLUS, "+", Variant(), start_line, start_column);
		case '*': return _make_token(Token::TK_STAR, "*", Variant(), start_line, start_column);
		case '/': return _make_token(Token::TK_SLASH, "/", Variant(), start_line, start_column);
		case '%': return _make_token(Token::TK_PERCENT, "%", Variant(), start_line, start_column);
		case '-':
			if (_peek() == '>') {
				_advance();
				return _make_token(Token::TK_ARROW, "->", Variant(), start_line, start_column);
			}
			return _make_token(Token::TK_MINUS, "-", Variant(), start_line, start_column);
	}
	return _make_token(Token::TK_ERROR, String::chr(c), Variant(), start_line, start_column);
}

Vector<LunariTokenizer::Token> LunariTokenizer::scan_all() {
	Vector<Token> tokens;
	while (true) {
		Token token = scan_token();
		tokens.push_back(token);
		if (token.type == Token::TK_EOF || token.type == Token::TK_ERROR) {
			break;
		}
	}
	return tokens;
}
