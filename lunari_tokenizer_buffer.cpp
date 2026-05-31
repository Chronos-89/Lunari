/**************************************************************************/
/*  lunari_tokenizer_buffer.cpp                                           */
/**************************************************************************/

#include "lunari_tokenizer_buffer.h"

#include "core/io/marshalls.h"

static void _append_u32(Vector<uint8_t> &r_buffer, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		r_buffer.push_back((p_value >> (i * 8)) & 0xff);
	}
}

static uint32_t _read_u32(const Vector<uint8_t> &p_buffer, int &r_offset) {
	if (r_offset + 4 > p_buffer.size()) {
		return 0;
	}
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) {
		value |= uint32_t(p_buffer[r_offset++]) << (i * 8);
	}
	return value;
}

Error LunariTokenizerBuffer::set_code_buffer(const Vector<uint8_t> &p_buffer) {
	tokens.clear();
	cursor = 0;
	if (p_buffer.size() < 8) {
		return ERR_INVALID_DATA;
	}

	int offset = 0;
	const uint32_t version = _read_u32(p_buffer, offset);
	if (version != TOKENIZER_VERSION) {
		return ERR_FILE_UNRECOGNIZED;
	}

	const uint32_t token_count = _read_u32(p_buffer, offset);
	for (uint32_t i = 0; i < token_count; i++) {
		if (offset + 16 > p_buffer.size()) {
			return ERR_INVALID_DATA;
		}

		LunariTokenizer::Token token;
		token.type = (LunariTokenizer::Token::Type)_read_u32(p_buffer, offset);
		token.line = (int)_read_u32(p_buffer, offset);
		token.column = (int)_read_u32(p_buffer, offset);
		const uint32_t source_len = _read_u32(p_buffer, offset);
		if (offset + (int)source_len > p_buffer.size()) {
			return ERR_INVALID_DATA;
		}
		String source;
		for (uint32_t j = 0; j < source_len; j++) {
			source += char32_t(p_buffer[offset++]);
		}
		token.source = source;
		token.literal = token.source;
		tokens.push_back(token);
	}

	return OK;
}

Vector<uint8_t> LunariTokenizerBuffer::parse_code_string(const String &p_code, CompressMode p_compress_mode) {
	ERR_FAIL_COND_V_MSG(p_compress_mode != COMPRESS_NONE, Vector<uint8_t>(), "Lunari tokenizer compression is not implemented yet.");

	LunariTokenizer tokenizer;
	tokenizer.set_source_code(p_code);
	tokens = tokenizer.scan_all();
	cursor = 0;

	Vector<uint8_t> buffer;
	_append_u32(buffer, TOKENIZER_VERSION);
	_append_u32(buffer, tokens.size());
	for (const LunariTokenizer::Token &token : tokens) {
		_append_u32(buffer, token.type);
		_append_u32(buffer, token.line);
		_append_u32(buffer, token.column);
		CharString source_utf8 = token.source.utf8();
		const int len = source_utf8.length();
		_append_u32(buffer, len);
		for (int i = 0; i < len; i++) {
			buffer.push_back(source_utf8[i]);
		}
	}
	return buffer;
}

LunariTokenizer::Token LunariTokenizerBuffer::scan() {
	if (cursor >= tokens.size()) {
		LunariTokenizer::Token eof;
		eof.type = LunariTokenizer::Token::TK_EOF;
		return eof;
	}
	return tokens[cursor++];
}

PackedStringArray LunariTokenizerBuffer::get_debug_lines() const {
	PackedStringArray lines;
	for (const LunariTokenizer::Token &token : tokens) {
		lines.push_back(vformat("%s(%s) at %d:%d", LunariTokenizer::token_name(token.type), token.source, token.line, token.column));
	}
	return lines;
}
