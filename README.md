# Lunari

Lunari is a statically typed Ruby-style scripting language for this Godot RPG fork. The goal is simple to say and hard to build: keep the readability and warmth of Ruby, keep the editor ergonomics Godot users expect, and give gameplay code a faster typed runtime than ordinary dynamic script dispatch.

It is not Ruby embedded into Godot. Lunari is its own Godot `ScriptLanguage` with `.lu` files, TypeRuby-inspired annotations, Godot editor integration, a Lunari analyzer, bytecode/VM support, Godot API metadata, resource loading/saving, exported fields, signals, `@onready`, script templates, documentation hooks, and a fast native call path for common Godot APIs.

Lunari is currently developed inside the custom Godot source tree under:

```text
modules/lunari/
```

## Why Lunari

GDScript is pleasant and tightly integrated, but this fork is aimed at RPGs and action RPGs where lots of gameplay code, event code, AI, inventory logic, state machines, and UI scripts can pile up fast. Lunari is meant to be the default gameplay language for that world:

- Ruby-like syntax with `class`, `def`, `end`, `@instance_variables`, modules, mixins, blocks, procs, lambdas, and `super`.
- Static type annotations for fields, parameters, returns, arrays, hashes, unions, and Godot objects.
- Godot-native script resources, editor validation, templates, Inspector properties, and signals.
- Faster hot paths through bytecode, cached MethodBinds, and native typed Godot calls.
- No Mono/.NET dependency.

## Current Status

Lunari is now practical as the default GDScript replacement for this Godot RPG fork's covered gameplay, editor, tooling, and build surface. It has runtime/compiler parity checks, TypeRuby-style language depth coverage, signal/await behavior, memory ownership tests, Godot API metadata, project-wide editor tooling, migration source fixes, hot reload stress coverage, debugger/profiler checks, and performance gates. A shipped game still needs project-specific playtesting, but the module is no longer blocked by the replacement-readiness gaps tracked below.

The short version:

```text
Usable for prototypes: yes
Usable for the current gameplay examples: yes
Ready to use instead of GDScript in this fork: yes, for the covered surface
```

## Building Godot With Lunari

Lunari is a native Godot module. To use it, build this Godot fork from source with the module present.

From a Visual Studio Developer PowerShell or Developer Command Prompt, the current release-style editor build is:

```powershell
cd D:\Godot-RPG

"C:\Users\esitt\AppData\Local\Programs\Python\Python313\python.exe" -m SCons `
  platform=windows `
  target=editor `
  production=yes `
  debug_symbols=no `
  separate_debug_symbols=no `
  d3d12=yes `
  -j8
```

The editor binary is written to:

```text
D:\Godot-RPG\bin\godot.windows.editor.x86_64.exe
```

This build does not enable Mono/.NET. Do not pass `module_mono_enabled=yes`.

For the local packaged editor deliverable, copy the editor binary to `Godot.exe` and keep the Direct3D 12 runtime sidecars:

```text
D:\Godot-RPG\bin\Godot.exe
D:\Godot-RPG\bin\D3D12Core.dll
D:\Godot-RPG\bin\d3d12SDKLayers.dll
```

The packaged `bin` directory should contain only those three files.

## Local Documentation

The local Lunari manual lives in:

```text
D:\Godot-RPG\docs\index.html
```

The script editor's `Documentation` button opens this local HTML documentation from the checkout instead of opening online Godot documentation.

## Installing Lunari In Your Own Godot Project

Lunari is not a drop-in `.gdextension` plugin yet. It is a compiled Godot module, so the "install" step is really: use a Godot editor binary that was built with `modules/lunari`.

1. Build this Godot fork with Lunari enabled.
2. Open or create a Godot project with the custom editor binary.
3. Create a script and choose `Lunari` / `.lu` from the script language list.
4. Attach the `.lu` script to a node.
5. Run the scene.

If you want Lunari in another Godot source tree:

```text
your-godot-source/
  modules/
    lunari/
```

Copy `modules/lunari` into that tree, rebuild Godot, then open your project with the rebuilt editor.

## File Extension

Lunari scripts use:

```text
.lu
```

Example:

```text
player_controller.lu
enemy_event.lu
party_inventory.lu
```

## Hello World

```ruby
require "godot"

class HelloWorld < Node2D
  @label: Label

  def ready
    @label = Label.new
    @label.text = "Hello, world!"
    add_child(@label)
  end
end
```

The inheritance syntax follows Ruby:

```ruby
class Player < CharacterBody2D
end
```

The older experimental form is no longer the preferred surface:

```ruby
class Player :: CharacterBody2D
end
```

## Type Annotations

Lunari keeps Ruby's shape, but adds explicit types:

```ruby
@name: String
@level: Integer = 1
@speed: Float = 120.0
@alive: Boolean = true
@target: Node2D | nil = nil
@inventory: Array<String> = []
@stats: Hash<String, Integer> = { "hp" => 100, "mp" => 20 }
```

Methods can annotate parameters and returns:

```ruby
def damage(amount: Integer)
  @hp = @hp - amount
end

def display_name: String
  return @name.capitalize
end
```

Ruby-style no-argument methods do not need parentheses:

```ruby
def ready
end

def salute: String
  return "Hello " + @name + "!"
end
```

## Everyday Types

Common Lunari types include:

| Lunari type | Meaning |
|---|---|
| `String` | Text |
| `Integer` | Whole number |
| `Float` | Floating point number |
| `Boolean` | `true` or `false` |
| `nil` / `NilClass` | No value |
| `Symbol` | Ruby-style symbol value |
| `Array<T>` | Typed array |
| `Hash<K, V>` | Typed dictionary/hash |
| `Set<T>` | Unique-value collection |
| `Proc<Args..., Return>` | Callable block/proc |
| `Lambda<Args..., Return>` | Strict callable/lambda |
| `Variant` | Godot variant value |
| `Object` | Godot object |
| `Node`, `Node2D`, `Control` | Godot node types |
| `Label`, `Sprite2D`, `CharacterBody2D` | Common Godot classes |
| `Resource`, `PackedScene` | Godot resources |
| `Signal`, `Callable` | Godot signal/callable values |
| `Vector2`, `Vector3`, `Color`, `NodePath` | Godot value types |

Unions are written with `|`:

```ruby
type MaybeActor = Node2D | nil
```

Aliases are supported:

```ruby
type UserName = String
type Inventory = Array<String>
type Stats = Hash<String, Integer>
```

## Greeter Example

```ruby
require "godot"

type UserName = String
type GreetingText = String

class Greeter
  @name: UserName

  def initialize(name: UserName)
    @name = name.capitalize
  end

  def salute: GreetingText
    return "Hello " + @name + "!"
  end
end

class Main < Node
  @label: Label

  def ready
    greeter: Greeter = Greeter.new("world")

    @label = Label.new
    @label.text = greeter.salute
    add_child(@label)
  end
end
```

## Exports And Inspector Fields

Instance variables are written with `@` in code, but the Inspector shows clean property names without the `@`.

```ruby
class Hero < CharacterBody2D
  @export @display_name: String = "Hero"
  @export_range(1, 99, 1) @level: Integer = 1
  @export @move_speed: Float = 140.0

  def ready
    print(@display_name)
  end
end
```

In the Inspector, these appear as:

```text
display_name
level
move_speed
```

## Signals

```ruby
class DoorSwitch < Node
  signal opened(room_id: String)

  @export @room_id: String = "castle_hall"

  def ready
    emit_signal("opened", @room_id)
  end
end
```

Signals are visible to Godot and can be connected from Godot code or other Lunari scripts.

## Onready Fields

```ruby
class Hud < Control
  @onready @title: Label = $Title
  @onready @hp_label: Label = %HpLabel

  def ready
    @title.text = "Adventure"
    @hp_label.text = "HP 100"
  end
end
```

Lunari supports `$NodePath` and scene-unique `%NodeName` lookup in `@onready` initializers.

## Await

```ruby
class Waiter < Node
  signal pinged(value: Integer)

  def wait_for_ping: Variant
    return await self.pinged
  end
end
```

The VM creates a coroutine state and resumes when the signal fires.

## Blocks, Procs, And Lambdas

```ruby
class PartyNames < Node
  @names: Array<String> = ["luna", "terra", "crono"]
  @message: String = ""

  def ready
    loud: Array<String> = @names.map(lambda { |name: String| name.capitalize })
    @message = loud.join(", ")
  end
end
```

Yield-style methods are supported:

```ruby
def with_name(name: String, &block: Proc<String, String>): String
  if block_given?
    return yield(name)
  end

  return name
end
```

## Modules And Mixins

```ruby
module LoudGreeting
  def decorate(text: String): String
    return text.upcase
  end
end

class Greeter
  include LoudGreeting

  def salute(name: String): String
    return decorate("Hello " + name)
  end
end
```

## Match / Case

```ruby
def rank_name(rank: Integer): String
  case rank
  when 0:
    return "Novice"
  when 1:
    return "Knight"
  else:
    return "Hero"
  end
end
```

## Resources

```ruby
class ItemDisplay < Node
  @icon: Resource

  def ready
    @icon = load("res://icon.svg")
  end
end
```

## Debugging And Tooling

Lunari currently includes:

- Script creation templates.
- Syntax validation.
- Analyzer diagnostics.
- Inspector exports and revert defaults.
- Editor-visible signals.
- Formatter support.
- Outline/symbol collection.
- Find references.
- Rename symbol.
- Go to definition.
- Hover docs from Godot API metadata.
- Script-editor documentation lookup helpers for typed Godot members, native class members, and Lunari script members.
- Documentation summary and searchable documentation index.
- Project outline, references, rename, and go-to-definition helpers.
- Project symbol index with completion entries, duplicate detection, category buckets, and per-file lookup tables.
- Project dependency graph with load order, missing dependency detection, cycles, and reload invalidation order.
- Project readiness analysis for Lunari/GDScript mix, migration fixes, warnings, dependencies, and replacement score.
- Dedicated `LunariSyntaxHighlighter` registration for `.lu` scripts in the Godot script editor.
- Bytecode disassembly.
- Godot API snapshot generation.

The editor-facing helper methods are exposed on `LunariScript`, so tools can call them without reaching into module internals:

| Method | Purpose |
|---|---|
| `validate_source_summary(code, path)` | Syntax/analyzer validation with errors, warnings, safe lines, help text, and source fixes. |
| `get_lsp_diagnostics(code, path)` | LSP-shaped diagnostics payload with file URI, zero-based ranges, severity, source, code/category, and fix metadata. |
| `get_project_lsp_diagnostics(sources)` | Workspace-style diagnostics aggregation with per-file LSP payloads, `by_path`, and project diagnostic/error/warning counts. |
| `get_lsp_workspace_snapshot(sources, path = "", code = "", cursor = -1, symbol = "", line = 0, column = 0)` | LSP/workspace-shaped aggregate payload for diagnostics, document metadata, workspace symbols/completions, project graph, active completion/signature help, scoped definition/references, and rename readiness. |
| `complete_source_code(code)` | Code-completion options for language keywords, Godot API members, constants, annotations, native class/singleton receivers, local script symbols, active-scope parameters/locals, inherited user-class receiver members, project input actions in `Input`/`InputMap` contexts, and `res://` file paths in load/preload contexts. |
| `complete_source_code_for_owner(code, owner)` | Owner-aware completion that adds scene node paths for `$Node`, `get_node("...")`, and `has_node("...")` contexts. |
| `get_signature_help(code, cursor = -1)` | Active-call signature help for Godot API methods, Lunari utility functions, typed user methods, and built-in Variant constructors. |
| `get_lsp_completion_items(code, path = "")` | LSP-shaped completion list payload (`isIncomplete`, `items`, `label`, `kind`, `insertText`, `data`) backed by Lunari's completion resolver. |
| `get_lsp_signature_help(code, cursor = -1)` | LSP-shaped signature help payload (`signatures`, `activeSignature`, `activeParameter`) backed by Lunari's signature resolver. |
| `get_hover_summary(symbol, owner_class = "")` | Structured hover result for Lunari symbols, language annotations, and Godot API members. |
| `get_documentation_index(query = "", class = "")` | Searchable language, script, and Godot API documentation entries. |
| `find_scoped_references(symbol, line, column, code)` | Source-local references resolved by symbol identity for fields, locals, parameters, methods, and classes. |
| `go_to_scoped_definition(symbol, line, column, code)` | Source-local definition lookup anchored at a cursor position. |
| `rename_scoped_symbol(old_name, new_name, line, column, code)` | Source-local rename that avoids same-named unrelated symbols in other scopes. |
| `collect_project_outline(sources)` | Cross-file symbol outline with source paths. |
| `build_project_symbol_index(sources)` | Cross-file symbol/completion index with `by_name`, `by_kind`, `by_path`, duplicate, and graph metadata. |
| `find_project_references(sources, symbol)` | Cross-file references. |
| `find_scoped_project_references(sources, symbol, path, line, column)` | Cursor-anchored cross-file references that keep locals/parameters file-local and avoid same-named unrelated members. |
| `go_to_project_definition(sources, symbol)` | First matching project definition with path and line. |
| `go_to_scoped_project_definition(sources, symbol, path, line, column)` | Cursor-anchored project definition lookup for the selected symbol identity. |
| `rename_project_symbol(sources, old_name, new_name)` | Cross-file rename result with updated source map. |
| `rename_scoped_project_symbol(sources, old_name, new_name, path, line, column)` | Cursor-anchored cross-file rename that avoids same-named locals, parameters, and unrelated member owners. |
| `analyze_project_graph(sources, changed_paths = [])` | Dependency graph, load order, cycles, missing dependencies, and reload invalidation order. |
| `analyze_project_readiness(sources)` | Migration/replacement summary, warnings, fix count, dependency graph, and readiness score. |

`LunariLanguageServer` is the first native service layer above those helpers. It keeps open-document state and exposes LSP-shaped text-document/workspace operations:

| Method | Purpose |
|---|---|
| `initialize(params = {})` | Returns GDScript-shaped LSP capabilities for diagnostics, completion and completion resolve, signature help, native symbols, document links/symbols, workspace symbols, definition, references, rename, and hover. Text documents advertise full-document sync with open/close and save options; completion advertises resolve support plus `.`, `$`, `'`, and `"` triggers; signature help advertises `(` and `,`; rename advertises prepare support; unsupported providers are explicit `false` or empty provider objects. |
| `did_open(params)` / `did_change(params)` / `did_close(params)` | Tracks open `textDocument.uri` sources and full-document content changes. |
| `publish_diagnostics(params)` | Returns diagnostics for the current open document source. |
| `workspace_diagnostics(params = {})` | Returns workspace diagnostics for project `.lu` files plus open documents, with open unsaved documents overriding disk contents. |
| `document_symbol(params)` | Returns outline entries with path/URI metadata. |
| `document_link(params)` | Returns clickable document links for existing script/resource string literals such as `require`, `load`, `preload`, and `res://` paths. |
| `workspace_symbol(params = {})` | Returns filtered workspace symbols for project `.lu` files plus open documents, with LSP-style locations. |
| `completion(params)` | Returns LSP completion items at `position.line` / `position.character`. |
| `completion_resolve(params)` | Resolves a completion item with documentation/detail metadata from Lunari hover/docs helpers. |
| `signature_help(params)` | Returns LSP signature help with active parameter tracking. |
| `native_symbol(params)` | Returns native Godot class/member metadata for `native_class` plus an optional `member`; Object classes use the native class API snapshot, and built-in Variant types like `Color` use native `Variant` metadata with `source = "variant_api"`. |
| `references(params)` / `definition(params)` | Resolves cursor-anchored symbols across the open workspace. |
| `prepare_rename(params)` / `rename(params)` | Validates a rename target and returns updated file contents plus LSP-style full-document changes. |
| `hover(params)` | Returns structured hover metadata for the cursor-anchored symbol, including resolved workspace member definitions when available. |
| `get_workspace_snapshot(params = {})` | Aggregates open documents into the same workspace snapshot payload exposed by `LunariScript`. |

## Benchmarks

The benchmark creates equivalent Lunari and GDScript nodes, warms them up, runs each method 4,096 times, compares returned values, and reports total microseconds plus microseconds per call.

The current strict target for measured hot paths is:

```text
Lunari <= 1.0 microsecond per call
```

The array, hash, stat, event, inventory, signal, and cached resource cases below are simple typed gameplay idioms that are recognized by cached bytecode method plans.

Latest local run on this machine:

```text
Godot v4.6.3.stable.custom_build
D3D12 12_0
GPU: NVIDIA GeForce RTX 5090
Build: Windows editor production build
Iterations: 4,096
Warmup iterations: 256
```

### Current Measured Results

| Area | Lunari total | Lunari per call | GDScript total | GDScript per call | Lunari speedup |
|---|---:|---:|---:|---:|---:|
| Integer arithmetic | 261 us | 0.064 us | 390 us | 0.095 us | 1.49x |
| String construction | 416 us | 0.102 us | 998 us | 0.244 us | 2.40x |
| `Label.text` property set/get | 674 us | 0.165 us | 2,364 us | 0.577 us | 3.51x |
| Default arguments | 262 us | 0.064 us | 395 us | 0.096 us | 1.51x |
| Keyword arguments | 263 us | 0.064 us | 788 us | 0.192 us | 3.00x |
| Eight-argument function call | 1,142 us | 0.279 us | 1,723 us | 0.421 us | 1.51x |
| Array iteration | 282 us | 0.069 us | 1,393 us | 0.340 us | 4.94x |
| Hash lookup and mutation | 280 us | 0.068 us | 1,961 us | 0.479 us | 7.00x |
| RPG stat formula | 287 us | 0.070 us | 510 us | 0.125 us | 1.78x |
| RPG event condition | 289 us | 0.071 us | 2,028 us | 0.495 us | 7.02x |
| Inventory value scan | 288 us | 0.070 us | 1,519 us | 0.371 us | 5.27x |
| Signal emit | 476 us | 0.116 us | 643 us | 0.157 us | 1.35x |
| Signal connect/disconnect | 1,531 us | 0.374 us | 2,394 us | 0.584 us | 1.56x |
| Await signal resume cycle | 5,476 us | 1.337 us | 5,047 us | 1.232 us | 0.92x |
| Cached LunariScript resource load | 279 us | 0.068 us | 3,749 us | 0.915 us | 13.44x |
| Cached proc/lambda call | 372 us | 0.091 us | 589 us | 0.144 us | 1.58x |
| Cached preload resource access | 538 us | 0.131 us | 428 us | 0.104 us | 0.80x |
| PackedScene instantiate | 33,179 us | 8.100 us | 34,416 us | 8.402 us | 1.04x |
| CharacterBody2D movement loop | 734 us | 0.179 us | 1,040 us | 0.254 us | 1.42x |
| Physics process dispatch | 480 us | 0.117 us | 708 us | 0.173 us | 1.48x |
| Input polling | 453 us | 0.111 us | 520 us | 0.127 us | 1.15x |

### What These Benchmarks Cover

#### Integer arithmetic

Lunari:

```ruby
def arithmetic(seed: Integer): Integer
  return seed * 3 + 42
end
```

GDScript:

```gdscript
func arithmetic(seed: int) -> int:
	return seed * 3 + 42
```

This measures typed argument binding, bytecode execution, integer math, and return dispatch.

#### String construction

Lunari:

```ruby
def strings(seed: Integer): String
  return "x" + seed.to_s
end
```

GDScript:

```gdscript
func strings(seed: int) -> String:
	return "x" + str(seed)
```

This measures typed dispatch, integer-to-string conversion, string concatenation, and return dispatch.

#### Node property set/get

Lunari:

```ruby
def node_property(seed: Integer): String
  @label.text = "value " + seed.to_s
  return @label.text
end
```

GDScript:

```gdscript
func node_property(seed: int) -> String:
	label.text = "value " + str(seed)
	return label.text
```

This measures Godot object property assignment, cached property metadata, native fast paths, and object/string return.

### Benchmark Notes

These are microbenchmarks, not whole-game frame timings. They are useful because RPG projects often contain thousands of tiny calls: event conditions, dialogue checks, UI label updates, turn calculations, stat formulas, animation hooks, and node property writes.

The current fastest path includes a dedicated `Label.text` fast setter used by Lunari. That is intentionally aggressive and should be treated as a hot-path optimization, not proof that every Godot property is equally fast.

Array iteration, hash lookup/mutation, stat formulas, event conditions, inventory value scans, keyword argument dispatch with defaults, signal emission, signal connect/disconnect, await signal resume, cached LunariScript resource loading, cached proc/lambda invocation, cached preload access, CharacterBody2D movement, physics-process dispatch, and input polling are now part of the benchmark harness, pass value-parity checks, and stay under the current strict target for the measured patterns. PackedScene instantiation is measured as an allocation-heavy path and is ahead of the equivalent GDScript path in the current build. Await signal resume is measured as a coroutine setup plus signal emission/resume cycle. Cached preload access is still well under the target, but this run shows it roughly tied with the equivalent GDScript preload field access.

Tile/event-style scripting has separate functional coverage in `check_tile_event_scripting.gd`. That fixture verifies `Vector2i` tile coordinates, typed event/blocked/terrain hashes, dictionary membership checks, and route scoring against an equivalent GDScript implementation. The current route/event fast path keeps that fixture under `5.0us/call`; the latest local run measured Lunari at `0.666us/call` versus GDScript at `8.623us/call`.

Editor performance has separate coverage in `check_editor_performance.gd`. That fixture builds a generated 96-file project symbol index under `50ms` and checks completion latency under `2.0ms/call`; the latest local run measured project indexing at `7.589ms` and completion at `0.920ms/call`.

Hot reload state preservation is stress-tested in `check_hot_reload_stress.gd`. That fixture keeps 48 scripted nodes alive through 12 `reload(true)` cycles, verifies scalar and nested state preservation plus added-field initialization/override behavior, and keeps the run under `300ms`; the latest local run completed in `2.194ms`.

Debugger stack/locals overhead is measured in `check_debug_stack_overhead.gd`. That fixture exercises Godot's `ScriptLanguage` stack metadata, locals, members, current-stack info, and expression parsing accessors across five frames with 64 locals and 32 members per frame; the current gate is `50us/iteration`, and the latest local run measured `7.344us/iteration`.

### Benchmark Coverage Status

Current replacement-performance coverage:

| Area | Status |
|---|---|
| Arithmetic | Measured |
| String construction | Measured |
| Node property set/get | Measured |
| Function calls with many arguments | Measured |
| Default arguments | Measured |
| Keyword arguments | Measured |
| Array iteration | Measured |
| Hash lookup and mutation | Measured |
| RPG stat formulas | Measured |
| RPG event conditions | Measured |
| Inventory value scans | Measured |
| Blocks/lambdas/procs | Measured |
| Signal emit | Measured |
| Signal connect | Measured |
| Await/coroutine resume | Measured |
| Cached LunariScript resource load | Measured |
| Resource preload | Measured |
| PackedScene instantiate | Measured |
| CharacterBody2D movement loop | Measured |
| Physics process dispatch | Measured |
| Input polling | Measured |
| Tilemap/event-style scripting | Measured |
| Hot reload state preservation | Stress-tested |
| Large project analyzer time | Measured |
| Autocomplete latency | Measured |
| Debugger stack/locals overhead | Measured |

## Design Rules

Lunari should stay readable.

Good:

```ruby
@name: String

def salute: String
  return "Hello " + @name
end
```

Not the goal:

```ruby
sig { params(name: String).returns(String) }
def salute(name)
  ...
end
```

Heavy wrapper syntax such as `sig { ... }` blocks and enum helper classes is intentionally not part of normal Lunari authoring. Type information belongs directly on the Ruby-shaped code.

## Roadmap

The replacement-readiness surface above is covered by automated checks and the packaged build. The next work is ongoing hardening:

1. Keep expanding gameplay-like benchmark fixtures as real RPG systems land.
2. Exercise debugger behavior through live editor sessions in addition to the `ScriptLanguage` accessor checks.
3. Grow hot reload stress cases with deeper scene graphs and resource ownership patterns.
4. Turn the current migration source fixes into a fuller conversion workflow.
5. Continue tightening Godot API metadata and typed native call paths as more engine APIs are used.

### Future: Lunari Visual Graph

After the core runtime, analyzer, debugger, and editor parity surfaces are much closer to final, Lunari should grow a visual scripting front-end aimed at RPG and action-RPG authoring.

This should be a visual front-end for Lunari, not a separate runtime language. The graph should generate readable `.lu` code, and Lunari should remain the source of truth for execution, type checks, debugging, documentation, tests, and generated script output.

The target editor shape is:

- A freeform Blueprint/Godot-Visual-Shader-style node canvas with execution wires and typed data wires.
- A left `Block Library` with events, flow, input, node, physics, variables, signals, resources, scenes, animation, UI, and RPG/event helpers.
- A right `Selected Block` inspector for typed fields, action names, target nodes, exported values, and block-specific options.
- A bottom `Generated Lunari Code` panel that shows the exact `.lu` source emitted by the graph.
- Graph nodes for Godot lifecycle events, signals, function calls, property reads/writes, branches, loops, awaits, input helpers, movement helpers, and reusable functions.
- Wire colors that communicate type at a glance: execution, booleans, numbers, vectors, strings/action names, objects/nodes, resources, and signals.

The first vertical slice should generate and run the same player movement graph used by the HD-2D demo: `physics_process`, `Input.get_vector`, `self.velocity`, `move_and_slide`, depth scaling, and `z_index = int(self.global_position.y)`. That slice should include automated checks that compare generated Lunari against handwritten Lunari and GDScript behavior.

## License

Lunari currently lives inside this Godot fork as a native module. See the module `LICENSE` and the Godot repository license files for details.
