/**************************************************************************/
/*  lunari_tokenizer_buffer.h                                             */
/**************************************************************************/

#pragma once

#include "lunari_tokenizer.h"

#include "core/templates/vector.h"

class LunariTokenizerBuffer {
public:
	static const int TOKENIZER_VERSION = 1;

	enum CompressMode {
		COMPRESS_NONE,
	};

private:
	Vector<LunariTokenizer::Token> tokens;
	int cursor = 0;

public:
	Error set_code_buffer(const Vector<uint8_t> &p_buffer);
	Vector<uint8_t> parse_code_string(const String &p_code, CompressMode p_compress_mode = COMPRESS_NONE);

	const Vector<LunariTokenizer::Token> &get_tokens() const { return tokens; }
	bool is_empty() const { return tokens.is_empty(); }
	void reset_cursor() { cursor = 0; }
	LunariTokenizer::Token scan();
	PackedStringArray get_debug_lines() const;
};
