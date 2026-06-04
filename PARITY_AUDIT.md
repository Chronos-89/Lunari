# Lunari / GDScript Parity Audit

This note tracks concrete parity gaps found while porting and testing gameplay demos against GDScript behavior. It is intentionally evidence-driven: each item should either name a passing regression or point at the source surface that still differs.

## Fixed In This Pass

- Project input actions are synchronized before Lunari calls into Godot `Input` and `InputMap` APIs. This lets project-defined actions such as `move_left`, `move_right`, and `jump` behave like GDScript calls without requiring Lunari scripts to manually rebuild the input map or fall back to `ui_*` actions.
  - Runtime/fallback path: `lunari_script.cpp`, `_lunari_sync_project_input_action()` and `_lunari_sync_project_input_call()`.
  - Expression/fallback path: `lunari_script.cpp`, direct `Input.get_axis(...)` and `Input.get_vector(...)` expression evaluation routes through native Godot `Input::get_axis(StringName, StringName)` and `Input::get_vector(StringName, StringName, StringName, StringName, float)`.
  - VM path: `lunari_vm.cpp`, `_lunari_vm_sync_project_input_action()` and the singleton call dispatch for `Input.get_axis`, `Input.get_vector`, and `Input.is_action_*`.
  - Regression: `tests/smoke_project/check_input_get_axis_parity.gd`.

- Native Variant property reads now work from fallback expressions. This covers expressions that gameplay code naturally writes, such as `self.global_position.y` or `some_vector.x`, when the base value is a Variant rather than a direct Object property call.
  - Runtime/fallback path: `lunari_script.cpp`, fallback postfix lookup uses `Variant::get_named()` only for Godot data structs such as vectors, colors, transforms, planes, and rects. This keeps Ruby-style calls like `name.capitalize`, `level.to_s`, and `tags.size` on Lunari's method path.
  - Regression: `tests/smoke_project/check_native_property_vector_parity.gd`.

- The HD-2D traveler demo now uses the same high-level input shape as GDScript for 2D movement: one `Input.get_vector("move_left", "move_right", "move_up", "move_down")` call feeding `CharacterBody2D.velocity`, without script-side action-name workarounds.
  - Demo path: `demos/hd_2d_traveler/player/player.lu`.
  - Regression: `tests/check_hd_2d_traveler_demo.gd`.

- GDScript-style numeric conversion now works on native property chains used by the demos. Lunari supports `int(self.global_position.y)` for the HD-2D `z_index` assignment, plus `float(...)`, `floor(...)`, and Lunari-style `Integer(...)` over the same native-property expression.
  - Runtime/parser path: `lunari_script.cpp`, builtin Variant constructor aliases for `int`, `float`, `str`, `bool`, and uppercase Lunari type names before constant-only handling.
  - Analyzer path: `lunari_analyzer.cpp`, matching constructor aliases and explicit `floor`/`floorf`/`floori` inference.
  - Regression: `tests/smoke_project/check_native_property_numeric_conversion_parity.gd` and `tests/smoke_project/check_native_property_vector_parity.gd`.

- Nested native Variant property writes now write the modified owner property back to the object. This covers `self.velocity.x = value` and `self.velocity.y = value`, matching the GDScript movement style more closely than requiring full Vector2 reassignment in user scripts.
  - Runtime/fallback path: `lunari_script.cpp`, nested property assignment uses `Variant::set_named()` and writes the changed `velocity` back through Object property assignment.
  - Regression: `tests/smoke_project/check_native_property_vector_parity.gd`.

- Godot lifecycle callback names now normalize more of the GDScript-facing method surface. `_enter_tree`, `_exit_tree`, and `_notification` map to Lunari `enter_tree`, `exit_tree`, and `notification`, alongside the existing ready/process/input mappings.
  - Runtime path: `lunari_script.cpp`, `_lunari_normalize_callback_name()` and `LunariScriptInstance::notification()`.
  - Regression: `tests/smoke_project/check_lifecycle_parity.gd`.

- Lunari scripts whose concrete class inherits from another Lunari class now prefer the class matching the script filename and resolve its native base through the dependency chain. This avoids exposing the required base script as the attached script class.
  - Analyzer path: `lunari_analyzer.cpp`, `_analyze_ast_document()`.
  - Regression: `tests/smoke_project/check_lifecycle_inheritance_parity.gd`.

- `_notification`-style dispatch now walks the Lunari script inheritance stack, including an external Lunari base script loaded through `require`, and calls base-to-child by default or child-to-base when Godot requests reversed notification order.
  - Runtime path: `lunari_script.cpp`, `LunariScript::call_notification_stack()` and `LunariScriptInstance::notification()`.
  - Regression: `tests/smoke_project/check_lifecycle_inheritance_parity.gd`.

- Script-instance method lookup now sees external Lunari base methods for `has_method`, `get_method_list`, direct `callp`, and no-parentheses calls from child method bodies.
  - Runtime path: `lunari_script.cpp`, `LunariScriptInstance::get_method_list()`, `has_method()`, `callp()`, `LunariScript::get_script_method_list()`, and no-parentheses identifier dispatch.
  - Regression: `tests/smoke_project/check_method_inheritance_parity.gd`.

- Inherited Lunari fields now participate in analyzer inference, default initialization, `Object.get`, `Object.set`, property type/revert queries, and exported property lists. This matches the GDScript behavior where script instance property metadata walks the base script chain.
  - Analyzer path: `lunari_analyzer.cpp`, inherited `@field` expression inference and current-file class selection through required base classes.
  - Runtime path: `lunari_script.cpp`, `LunariScript::get_lunari_fields_including_base()` and script instance field/property paths.
  - Regression: `tests/smoke_project/check_field_inheritance_parity.gd`.

## Known Remaining Gaps

- GDScript exposes dynamic script property hooks that Lunari does not yet implement at the same level: `_set`, `_get`, `_get_property_list`, `_validate_property`, `_property_can_revert`, and `_property_get_revert`.
  - GDScript reference: `modules/gdscript/gdscript.cpp`, `GDScriptInstance::set()`, `get()`, `get_property_list()`, `validate_property()`, `property_can_revert()`, and `property_get_revert()`.
  - Lunari current surface: `modules/lunari/lunari_script.h`, `validate_property()` is currently empty and revert behavior is not script-hook driven.

- GDScript's analyzer/compiler/VM has validated typed opcodes and mature type adjustment for the full Variant surface. Lunari still has mixed VM and fallback execution paths, so every gameplay feature that touches native properties, arrays/dictionaries, operators, and singleton calls needs regression coverage on both paths.
  - Recent examples that required fixes: `Input.get_axis`, `Input.get_vector`, `self.velocity.x`, `self.global_position.y`, and `int(self.global_position.y)`.

- Editor parity is still thinner than GDScript. Lunari has script-editor helpers, docs integration, method filtering, and completions, but not the same GDScript analyzer/LSP depth, project-wide symbol model, warning system, or inheritance-aware completion surface.
  - GDScript reference areas: `modules/gdscript/editor/`, `modules/gdscript/language_server/`, and `modules/gdscript/gdscript_analyzer.cpp`.
  - Lunari reference areas: `modules/lunari/lunari_tooling.cpp`, `lunari_analyzer.cpp`, and editor integration in `lunari_script.cpp`.

### Editor And IntelliSense Parity Backlog

GDScript's editor support is not just `complete_code()`. It has a dedicated highlighter, doc generator, language server protocol layer, workspace symbol index, native symbol database, diagnostics publisher, signature help, related-symbol resolution, rename validation, and usage search. Lunari currently exposes useful helper APIs, but they are still much thinner.

Concrete gaps to close:

- Add a Lunari language-server layer mirroring the GDScript split between extended parser, text document service, workspace service, and protocol glue.
  - GDScript references: `modules/gdscript/language_server/gdscript_extend_parser.*`, `gdscript_text_document.*`, `gdscript_workspace.*`, and `gdscript_language_protocol.*`.
  - Lunari starting points: `LunariTooling::build_project_symbol_index()`, `find_project_references()`, `go_to_project_definition()`, and `rename_project_symbol()`.

- Replace text-search references/rename with resolver-backed symbols that know whether a token is a local, field, method, class, native member, signal, constant, or parameter.
  - GDScript reference: `GDScriptWorkspace::resolve_symbol()`, `find_all_usages()`, `can_rename()`, and `rename()`.
  - Current Lunari risk: `LunariTooling::find_references()` is intentionally broad and can match same-named unrelated symbols.

- Add signature help for calls and constructors, including active-parameter tracking.
  - GDScript reference: `GDScriptWorkspace::resolve_signature()` and `ExtendGDScriptParser::get_left_function_call()`.
  - Lunari starting points: `LunariGodotApi::get_method_signature()`, `LunariUtilityFunctions::get_function_info()`, and analyzer method signatures.

- Make completions context-aware beyond simple receiver/member lookup: locals in scope, method parameters, inherited fields/methods, static members, enum values, signal names, annotation arguments, node-path shorthands, and expected-type filtering.
  - GDScript reference: `GDScriptLanguage::complete_code()` plus workspace completion in `GDScriptWorkspace::completion()`.
  - Current Lunari path: `LunariLanguage::complete_code()` and `_lunari_editor_type_map()`.

- Publish structured diagnostics and warnings with ranges, severity, source, code/category, and quick fixes in the same editor/LSP shape as GDScript.
  - GDScript reference: `ExtendGDScriptParser::update_diagnostics()` and `GDScriptWorkspace::publish_diagnostics()`.
  - Current Lunari path: analyzer diagnostics and `validate_source_summary()`.

- Add a Lunari-specific syntax highlighter instead of relying on generic highlighting.
  - GDScript reference: `modules/gdscript/editor/gdscript_highlighter.*`.
  - Lunari requirements: Ruby-style keywords, `@fields`, `@@class_variables`, symbols, `%w[]`, regex literals, type annotations, annotations, node-path shorthands, and Godot class names.

- Add docgen/reference generation for Lunari language constructs, annotations, generated classes, and visual-graph generated APIs.
  - GDScript reference: `modules/gdscript/editor/gdscript_docgen.*`.
  - Current Lunari path: local HTML docs and `get_documentation_index()`.

## Check Commands

After rebuilding with `module_lunari_enabled=yes`, the following checks were run:

```powershell
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_lifecycle_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_lifecycle_inheritance_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_method_inheritance_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_field_inheritance_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_input_get_axis_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_native_property_numeric_conversion_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\tests\smoke_project --script check_native_property_vector_parity.gd
.\bin\godot.windows.editor.x86_64.console.exe --headless --path modules\lunari\demos\hd_2d_traveler --script modules\lunari\tests\check_hd_2d_traveler_demo.gd
```

The full Lunari check set and the full Lunari negative checks also passed. The intentional missing-action parity case still prints native Godot `InputMap` errors, matching the goal of allowing Godot's own diagnostics while avoiding duplicate Lunari VM/debug wrapper errors.
