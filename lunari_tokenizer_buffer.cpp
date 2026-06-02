/**************************************************************************/
/*  lunari_tokenizer_buffer.cpp                                           */
/**************************************************************************/

#include "lunari_tokenizer_buffer.h"

#include "core/io/compression.h"
#include "core/io/marshalls.h"

static const uint8_t LUNARI_TOKENIZER_BUFFER_MAGIC[4] = { 'L', 'U', 'T', 'K' };

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

	Vector<uint8_t> contents;
	const Vector<uint8_t> *read_buffer = &p_buffer;
	if (p_buffer.size() >= 12 &&
			p_buffer[0] == LUNARI_TOKENIZER_BUFFER_MAGIC[0] &&
			p_buffer[1] == LUNARI_TOKENIZER_BUFFER_MAGIC[1] &&
			p_buffer[2] == LUNARI_TOKENIZER_BUFFER_MAGIC[2] &&
			p_buffer[3] == LUNARI_TOKENIZER_BUFFER_MAGIC[3]) {
		const uint8_t *buf = p_buffer.ptr();
		const uint32_t version = decode_uint32(&buf[4]);
		if (version != TOKENIZER_VERSION) {
			return ERR_FILE_UNRECOGNIZED;
		}
		const uint32_t decompressed_size = decode_uint32(&buf[8]);
		if (decompressed_size == 0) {
			contents = p_buffer.slice(12);
		} else {
			contents.resize(decompressed_size);
			const int64_t result = Compression::decompress(contents.ptrw(), contents.size(), &buf[12], p_buffer.size() - 12, Compression::MODE_ZSTD);
			ERR_FAIL_COND_V_MSG(result != decompressed_size, ERR_INVALID_DATA, "Error decompressing Lunari tokenizer buffer.");
		}
		read_buffer = &contents;
		if (read_buffer->size() < 8) {
			return ERR_INVALID_DATA;
		}
	}

	int offset = 0;
	const uint32_t version = _read_u32(*read_buffer, offset);
	if (version != TOKENIZER_VERSION) {
		return ERR_FILE_UNRECOGNIZED;
	}

	const uint32_t token_count = _read_u32(*read_buffer, offset);
	for (uint32_t i = 0; i < token_count; i++) {
		if (offset + 16 > read_buffer->size()) {
			return ERR_INVALID_DATA;
		}

		LunariTokenizer::Token token;
		token.type = (LunariTokenizer::Token::Type)_read_u32(*read_buffer, offset);
		token.line = (int)_read_u32(*read_buffer, offset);
		token.column = (int)_read_u32(*read_buffer, offset);
		const uint32_t source_len = _read_u32(*read_buffer, offset);
		if (offset + (int)source_len > read_buffer->size()) {
			return ERR_INVALID_DATA;
		}
		String source = source_len > 0 ? String::utf8(reinterpret_cast<const char *>(read_buffer->ptr() + offset), source_len) : String();
		offset += source_len;
		token.source = source;
		token.literal = token.source;
		tokens.push_back(token);
	}

	return OK;
}

Vector<uint8_t> LunariTokenizerBuffer::parse_code_string(const String &p_code, CompressMode p_compress_mode) {
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

	switch (p_compress_mode) {
		case COMPRESS_NONE:
			return buffer;
		case COMPRESS_ZSTD: {
			Vector<uint8_t> compressed;
			const int64_t max_size = Compression::get_max_compressed_buffer_size(buffer.size(), Compression::MODE_ZSTD);
			compressed.resize(max_size);
			const int64_t compressed_size = Compression::compress(compressed.ptrw(), buffer.ptr(), buffer.size(), Compression::MODE_ZSTD);
			ERR_FAIL_COND_V_MSG(compressed_size < 0, Vector<uint8_t>(), "Error compressing Lunari tokenizer buffer.");
			compressed.resize(compressed_size);

			Vector<uint8_t> wrapped;
			wrapped.resize(12);
			wrapped.write[0] = LUNARI_TOKENIZER_BUFFER_MAGIC[0];
			wrapped.write[1] = LUNARI_TOKENIZER_BUFFER_MAGIC[1];
			wrapped.write[2] = LUNARI_TOKENIZER_BUFFER_MAGIC[2];
			wrapped.write[3] = LUNARI_TOKENIZER_BUFFER_MAGIC[3];
			encode_uint32(TOKENIZER_VERSION, &wrapped.write[4]);
			encode_uint32(buffer.size(), &wrapped.write[8]);
			wrapped.append_array(compressed);
			return wrapped;
		}
	}
	return Vector<uint8_t>();
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
