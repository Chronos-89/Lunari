/**************************************************************************/
/*  lunari_tooling.cpp                                                     */
/**************************************************************************/

#include "lunari_tooling.h"

#include "lunari_ast.h"
#include "lunari_parser.h"

bool LunariTooling::_is_identifier_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= '0' && p_char <= '9') || p_char == '_' || p_char == '@';
}

static String _lunari_tooling_instance_name(const String &p_symbol) {
	String symbol = p_symbol.strip_edges();
	if (symbol.begins_with("@")) {
		return symbol;
	}
	return "@" + symbol;
}

static String _lunari_tooling_plain_name(const String &p_symbol) {
	String symbol = p_symbol.strip_edges();
	while (symbol.begins_with("@")) {
		symbol = symbol.substr(1);
	}
	return symbol;
}

static bool _lunari_tooling_char_is_symbol_prefix(const String &p_line, int p_index) {
	return p_index > 0 && p_line[p_index - 1] == '@';
}

void LunariTooling::_collect_outline_from_ast(const Vector<LunariAST::Node> &p_nodes, Array *r_outline, const String &p_parent) {
	ERR_FAIL_NULL(r_outline);
	for (const LunariAST::Node &node : p_nodes) {
		bool include = false;
		String kind;
		switch (node.kind) {
			case LunariAST::Node::NODE_CLASS:
				include = true;
				kind = "class";
				break;
			case LunariAST::Node::NODE_MODULE:
				include = true;
				kind = "module";
				break;
			case LunariAST::Node::NODE_METHOD:
				include = true;
				kind = "method";
				break;
			case LunariAST::Node::NODE_FIELD:
				include = true;
				kind = "field";
				break;
			case LunariAST::Node::NODE_SIGNAL:
				include = true;
				kind = "signal";
				break;
			case LunariAST::Node::NODE_CONST:
				include = true;
				kind = "const";
				break;
			case LunariAST::Node::NODE_ENUM:
				include = true;
				kind = "enum";
				break;
			default:
				break;
		}
		String qualified_parent = p_parent;
		if (include) {
			Dictionary item;
			String display_name = node.name;
			if (node.kind == LunariAST::Node::NODE_FIELD && display_name.begins_with("@") && !display_name.begins_with("@@")) {
				display_name = display_name.substr(1);
			}
			item["name"] = display_name;
			item["source_name"] = String(node.name);
			item["kind"] = kind;
			item["type"] = String(node.type);
			item["parent"] = p_parent;
			item["line"] = node.line;
			item["static"] = node.is_static || node.is_class_method;
			item["public"] = node.is_public;
			r_outline->push_back(item);
			qualified_parent = p_parent.is_empty() ? String(node.name) : p_parent + "::" + String(node.name);
		}
		_collect_outline_from_ast(node.children, r_outline, qualified_parent);
	}
}

String LunariTooling::format_code(const String &p_code) {
	Vector<String> lines = p_code.split("\n");
	String formatted;
	int indent = 0;
	String previous_significant;
	for (int i = 0; i < lines.size(); i++) {
		String stripped = lines[i].strip_edges();
		const bool match_arm = stripped.ends_with(":") && !stripped.begins_with("def ") && !stripped.begins_with("class ") && !stripped.begins_with("module ");
		const bool first_match_arm = match_arm && previous_significant.begins_with("match ");
		if (stripped == "end" || stripped == "else" || stripped.begins_with("elsif ") || stripped.begins_with("rescue") || stripped == "ensure" || (match_arm && !first_match_arm)) {
			indent = MAX(0, indent - 1);
		}
		if (!stripped.is_empty()) {
			formatted += String("  ").repeat(indent) + stripped;
		}
		if (i + 1 < lines.size()) {
			formatted += "\n";
		}
		if (stripped.begins_with("class ") || stripped.begins_with("abstract class ") || stripped.begins_with("module ") || stripped.begins_with("def ") || stripped == "begin" || stripped == "else" || stripped.begins_with("rescue") || stripped == "ensure" || stripped.begins_with("if ") || stripped.begins_with("unless ") || stripped.begins_with("while ") || stripped.begins_with("until ") || stripped.begins_with("for ") || stripped.begins_with("match ") || stripped.ends_with(":")) {
			indent++;
		}
		if (!stripped.is_empty() && !stripped.begins_with("#")) {
			previous_significant = stripped;
		}
	}
	return formatted;
}

Array LunariTooling::collect_outline(const String &p_code) {
	LunariParser parser;
	LunariAST::Document document = parser.parse_ast(p_code);
	Array outline;
	_collect_outline_from_ast(document.children, &outline);
	return outline;
}

Array LunariTooling::find_references(const String &p_code, const String &p_symbol) {
	Array references;
	if (p_symbol.is_empty()) {
		return references;
	}
	Vector<String> symbols;
	symbols.push_back(p_symbol);
	String plain_symbol = _lunari_tooling_plain_name(p_symbol);
	String instance_symbol = _lunari_tooling_instance_name(p_symbol);
	if (plain_symbol != p_symbol) {
		symbols.push_back(plain_symbol);
	}
	if (instance_symbol != p_symbol) {
		symbols.push_back(instance_symbol);
	}
	Vector<String> lines = p_code.split("\n");
	for (int line_index = 0; line_index < lines.size(); line_index++) {
		String line = lines[line_index];
		for (const String &symbol : symbols) {
			int from = 0;
			while (true) {
				int column = line.find(symbol, from);
				if (column < 0) {
					break;
				}
				bool left_ok = column == 0 || !_is_identifier_char(line[column - 1]);
				int right_index = column + symbol.length();
				bool right_ok = right_index >= line.length() || !_is_identifier_char(line[right_index]);
				if (symbol == plain_symbol && _lunari_tooling_char_is_symbol_prefix(line, column)) {
					left_ok = false;
				}
				if (left_ok && right_ok) {
					Dictionary reference;
					reference["line"] = line_index + 1;
					reference["column"] = column + 1;
					reference["symbol"] = symbol;
					references.push_back(reference);
				}
				from = column + symbol.length();
			}
		}
	}
	return references;
}

Dictionary LunariTooling::rename_symbol(const String &p_code, const String &p_old_name, const String &p_new_name) {
	Dictionary result;
	Array references = find_references(p_code, p_old_name);
	Vector<String> lines = p_code.split("\n");
	for (int i = references.size() - 1; i >= 0; i--) {
		Dictionary reference = references[i];
		int line = int(reference["line"]) - 1;
		int column = int(reference["column"]) - 1;
		if (line < 0 || line >= lines.size()) {
			continue;
		}
		String text = lines[line];
		String matched = reference.get("symbol", p_old_name);
		String replacement = matched.begins_with("@") && !p_new_name.begins_with("@") ? "@" + p_new_name : p_new_name;
		lines.write[line] = text.substr(0, column) + replacement + text.substr(column + matched.length());
	}
	String renamed;
	for (int i = 0; i < lines.size(); i++) {
		renamed += lines[i];
		if (i + 1 < lines.size()) {
			renamed += "\n";
		}
	}
	result["code"] = renamed;
	result["references"] = references;
	result["changed"] = references.size();
	return result;
}
