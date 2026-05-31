/**************************************************************************/
/*  lunari_warning.cpp                                                    */
/**************************************************************************/

#include "lunari_warning.h"

String LunariWarning::get_name() const {
	return get_name_from_code(code);
}

String LunariWarning::get_message() const {
	switch (code) {
		case UNUSED_PRIVATE_FIELD:
			return symbols.size() > 0 ? "Private field '" + symbols[0] + "' is declared but not used." : "Private field is declared but not used.";
		case UNUSED_LOCAL:
			return symbols.size() > 0 ? "Local variable '" + symbols[0] + "' is declared but not used." : "Local variable is declared but not used.";
		case SHADOWED_FIELD:
			return symbols.size() > 0 ? "Local variable '" + symbols[0] + "' shadows a field with the same name." : "Local variable shadows a field with the same name.";
		case UNREACHABLE_CODE:
			return "Code after return is unreachable.";
		case IMPLICIT_LOCAL_INFERENCE:
			return symbols.size() > 0 ? "Local variable '" + symbols[0] + "' relies on type inference." : "Local variable relies on type inference.";
		case WARNING_MAX:
			break;
	}
	return String();
}

String LunariWarning::get_name_from_code(Code p_code) {
	switch (p_code) {
		case UNUSED_PRIVATE_FIELD: return "unused_private_field";
		case UNUSED_LOCAL: return "unused_local";
		case SHADOWED_FIELD: return "shadowed_field";
		case UNREACHABLE_CODE: return "unreachable_code";
		case IMPLICIT_LOCAL_INFERENCE: return "implicit_local_inference";
		case WARNING_MAX: break;
	}
	return String();
}

LunariWarning::Code LunariWarning::get_code_from_name(const String &p_name) {
	for (int i = 0; i < WARNING_MAX; i++) {
		Code code = (Code)i;
		if (get_name_from_code(code) == p_name) {
			return code;
		}
	}
	return WARNING_MAX;
}

LunariWarning::WarnLevel LunariWarning::get_default_level(Code p_code) {
	switch (p_code) {
		case UNUSED_PRIVATE_FIELD:
		case UNUSED_LOCAL:
		case SHADOWED_FIELD:
		case UNREACHABLE_CODE:
			return WARN;
		case IMPLICIT_LOCAL_INFERENCE:
			return IGNORE;
		case WARNING_MAX:
			break;
	}
	return IGNORE;
}
