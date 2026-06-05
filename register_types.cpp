/**************************************************************************/
/*  register_types.cpp                                                     */
/**************************************************************************/

#include "register_types.h"

#include "lunari_cache.h"
#include "lunari_highlighter.h"
#include "lunari_language_server.h"
#include "lunari_rpc_callable.h"
#include "lunari_script.h"
#include "lunari_tokenizer_buffer.h"
#include "lunari_utility_functions.h"
#include "lunari_warning.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "editor/script/script_editor_plugin.h"
#endif

LunariLanguage *script_language_lunari = nullptr;
Ref<ResourceFormatLoaderLunariScript> resource_loader_lunari;
Ref<ResourceFormatSaverLunariScript> resource_saver_lunari;

#ifdef TOOLS_ENABLED
static void _lunari_editor_init() {
	Ref<LunariSyntaxHighlighter> lunari_syntax_highlighter;
	lunari_syntax_highlighter.instantiate();
	ScriptEditor::get_singleton()->register_syntax_highlighter(lunari_syntax_highlighter);
}
#endif

void initialize_lunari_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		GDREGISTER_CLASS(LunariScript);
		GDREGISTER_CLASS(LunariObject);
		GDREGISTER_CLASS(LunariLanguageServer);
		LunariCache::initialize();

		script_language_lunari = memnew(LunariLanguage);
		ScriptServer::register_language(script_language_lunari);

		LunariUtilityFunctions::register_functions();

		resource_loader_lunari.instantiate();
		ResourceLoader::add_resource_format_loader(resource_loader_lunari);

		resource_saver_lunari.instantiate();
		ResourceSaver::add_resource_format_saver(resource_saver_lunari);

#ifdef TOOLS_ENABLED
		EditorNode::add_init_callback(_lunari_editor_init);
#endif
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(LunariSyntaxHighlighter);
	}
#endif
}

void uninitialize_lunari_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	ScriptServer::unregister_language(script_language_lunari);

	if (script_language_lunari) {
		memdelete(script_language_lunari);
	}

	LunariUtilityFunctions::unregister_functions();
	LunariCache::finalize();

	ResourceLoader::remove_resource_format_loader(resource_loader_lunari);
	resource_loader_lunari.unref();

	ResourceSaver::remove_resource_format_saver(resource_saver_lunari);
	resource_saver_lunari.unref();
}
