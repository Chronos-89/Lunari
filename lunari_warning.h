/**************************************************************************/
/*  lunari_warning.h                                                      */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"

class LunariWarning {
public:
	enum Code {
		UNUSED_PRIVATE_FIELD,
		UNUSED_LOCAL,
		SHADOWED_FIELD,
		UNREACHABLE_CODE,
		IMPLICIT_LOCAL_INFERENCE,
		WARNING_MAX,
	};

	enum WarnLevel {
		IGNORE,
		WARN,
		ERROR,
	};

	Code code = WARNING_MAX;
	int line = 1;
	int column = 1;
	Vector<String> symbols;

	String get_name() const;
	String get_message() const;

	static String get_name_from_code(Code p_code);
	static Code get_code_from_name(const String &p_name);
	static WarnLevel get_default_level(Code p_code);
};
