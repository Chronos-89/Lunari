/**************************************************************************/
/*  lunari_language_server.h                                               */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class LunariLanguageServer : public RefCounted {
	GDCLASS(LunariLanguageServer, RefCounted);

	Dictionary documents;
	String root_uri;
	String root_path;
	mutable Dictionary workspace_index_cache;
	mutable String workspace_index_cache_fingerprint;
	mutable bool workspace_index_cache_valid = false;
	mutable int64_t workspace_index_cache_hits = 0;
	mutable int64_t workspace_index_cache_misses = 0;
	mutable Dictionary workspace_diagnostics_cache;
	mutable String workspace_diagnostics_cache_fingerprint;
	mutable bool workspace_diagnostics_cache_valid = false;
	mutable int64_t workspace_diagnostics_cache_hits = 0;
	mutable int64_t workspace_diagnostics_cache_misses = 0;
	mutable Dictionary workspace_document_cache;
	mutable String workspace_document_cache_fingerprint;
	mutable bool workspace_document_cache_valid = false;
	mutable int64_t workspace_document_cache_hits = 0;
	mutable int64_t workspace_document_cache_misses = 0;
	mutable Dictionary workspace_completion_cache;
	mutable String workspace_completion_cache_fingerprint;
	mutable bool workspace_completion_cache_valid = false;
	mutable int64_t workspace_completion_cache_hits = 0;
	mutable int64_t workspace_completion_cache_misses = 0;
	mutable Dictionary workspace_signature_cache;
	mutable String workspace_signature_cache_fingerprint;
	mutable bool workspace_signature_cache_valid = false;
	mutable int64_t workspace_signature_cache_hits = 0;
	mutable int64_t workspace_signature_cache_misses = 0;
	mutable Array workspace_references_cache;
	mutable String workspace_references_cache_fingerprint;
	mutable bool workspace_references_cache_valid = false;
	mutable int64_t workspace_references_cache_hits = 0;
	mutable int64_t workspace_references_cache_misses = 0;

	static String _uri_to_path(const String &p_uri);
	static String _path_to_uri(const String &p_path);
	static int _position_to_offset(const String &p_code, int p_line, int p_character);
	static String _word_at_position(const String &p_code, int p_line, int p_character);
	static Dictionary _position_from_params(const Dictionary &p_params);
	static String _path_from_params(const Dictionary &p_params);
	String _code_for_path(const String &p_path) const;
	Dictionary _sources_with_document(const String &p_path = String(), const String &p_code = String()) const;
	Dictionary _workspace_sources() const;
	String _workspace_sources_fingerprint(const Dictionary &p_sources) const;
	Dictionary _workspace_document_snapshots(const Dictionary &p_sources) const;
	Dictionary _workspace_symbol_index(const Dictionary &p_sources) const;
	Dictionary _workspace_diagnostics_payload(const Dictionary &p_sources) const;
	Dictionary _workspace_completion_payload(const Dictionary &p_sources, const String &p_prefix, const String &p_path) const;
	Dictionary _workspace_signature_payload(const Dictionary &p_sources, const String &p_code, int p_offset) const;
	Array _workspace_references_payload(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line, int p_column) const;
	void _invalidate_workspace_cache();

protected:
	static void _bind_methods();

public:
	Dictionary initialize(const Dictionary &p_params = Dictionary());
	void did_open(const Dictionary &p_params);
	void did_change(const Dictionary &p_params);
	void did_close(const Dictionary &p_params);

	Dictionary get_workspace_snapshot(const Dictionary &p_params = Dictionary()) const;
	Dictionary publish_diagnostics(const Dictionary &p_params) const;
	Dictionary workspace_diagnostics(const Dictionary &p_params = Dictionary()) const;
	Array document_symbol(const Dictionary &p_params) const;
	Array document_link(const Dictionary &p_params) const;
	Array workspace_symbol(const Dictionary &p_params = Dictionary()) const;
	Dictionary completion(const Dictionary &p_params) const;
	Dictionary completion_resolve(const Dictionary &p_params) const;
	Dictionary signature_help(const Dictionary &p_params) const;
	Variant native_symbol(const Dictionary &p_params) const;
	Array references(const Dictionary &p_params) const;
	Variant prepare_rename(const Dictionary &p_params) const;
	Dictionary rename(const Dictionary &p_params) const;
	Dictionary definition(const Dictionary &p_params) const;
	Variant hover(const Dictionary &p_params) const;

	Dictionary get_open_documents() const;
	Dictionary get_workspace_cache_stats() const;

	LunariLanguageServer();
};
