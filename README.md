<p align="center">
  <img src="docs/lunari-ruby-logo.svg" alt="Lunari Ruby Logo" width="900">
</p>

# Lunari

Lunari is a statically typed Ruby-style scripting language for this Godot RPG fork. The goal is simple to say and hard to build: keep the readability and warmth of Ruby, keep the editor ergonomics Godot users expect, and give gameplay code a faster typed runtime than ordinary dynamic script dispatch.

It is not Ruby embedded into Godot. It is not Sorbet syntax pasted on top of Ruby. Lunari is its own Godot `ScriptLanguage` with `.lu` files, TypeRuby-inspired annotations, Godot editor integration, a Lunari analyzer, bytecode/VM support, Godot API metadata, resource loading/saving, exported fields, signals, `@onready`, script templates, documentation hooks, and a fast native call path for common Godot APIs.

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

Lunari is actively experimental. It already covers editor integration, TypeRuby-style language depth, signal/await behavior, memory ownership, Godot API metadata, and performance benchmarks. It is not yet something I would honestly call a fully proven GDScript replacement for a large shipped game.

The short version:

```text
Usable for prototypes: yes
Usable for the current gameplay examples: yes
Ready to delete GDScript from Godot: not yet
```

## Building Godot With Lunari

Lunari is a native Godot module. To use it, build this Godot fork from source with the module present.

From a Visual Studio Developer PowerShell or Developer Command Prompt:

```powershell
cd D:\Godot-RPG

"C:\Users\esitt\AppData\Local\Programs\Python\Python313\python.exe" -m SCons `
  platform=windows `
  target=editor `
  dev_build=yes `
  d3d12=yes `
  -j8
```

The editor binary is written to:

```text
D:\Godot-RPG\bin\godot.windows.editor.dev.x86_64.exe
D:\Godot-RPG\bin\godot.windows.editor.dev.x86_64.console.exe
```

This build does not enable Mono/.NET. Do not pass `module_mono_enabled=yes`.

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

  def ready: void
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

Not:

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
def damage(amount: Integer): void
  @hp = @hp - amount
end

def display_name: String
  return @name.capitalize
end
```

Ruby-style no-argument methods do not need parentheses:

```ruby
def ready: void
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

  def initialize(name: UserName): void
    @name = name.capitalize
  end

  def salute: GreetingText
    return "Hello " + @name + "!"
  end
end

class Main < Node
  @label: Label

  def ready: void
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

  def ready: void
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

  def ready: void
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

  def ready: void
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

  def ready: void
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

  def ready: void
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
- Bytecode disassembly.
- Godot API snapshot generation.

## Benchmarks

The benchmark creates equivalent Lunari and GDScript nodes, warms them up, runs each method 4,096 times, compares returned values, and reports total microseconds plus microseconds per call.

The current strict target is:

```text
Lunari <= 1.0 microsecond per call
```

Latest local run on this machine:

```text
Godot v4.6.3.stable.custom_build
D3D12 12_0
GPU: NVIDIA GeForce RTX 5090
Build: Windows editor dev build
Iterations: 4,096
Warmup iterations: 256
```

### Current Measured Results

| Area | Lunari total | Lunari per call | GDScript total | GDScript per call | Lunari speedup |
|---|---:|---:|---:|---:|---:|
| Integer arithmetic | 805 us | 0.197 us | 1,868 us | 0.456 us | 2.31x |
| String construction | 1,490 us | 0.364 us | 4,367 us | 1.066 us | 2.93x |
| `Label.text` property set/get | 2,848 us | 0.695 us | 9,022 us | 2.203 us | 3.17x |

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

### Benchmark Areas Still Needed

To honestly claim full replacement performance, Lunari still needs broader benchmark coverage:

| Area | Status |
|---|---|
| Arithmetic | Measured |
| String construction | Measured |
| Node property set/get | Measured |
| Function calls with many arguments | Needed |
| Default arguments | Tested, benchmark needed |
| Keyword arguments | Tested, benchmark needed |
| Array iteration | Tested, benchmark needed |
| Hash lookup and mutation | Tested, benchmark needed |
| Blocks/lambdas/procs | Tested, benchmark needed |
| Signal connect/emit | Tested, benchmark needed |
| Await/coroutine resume | Tested, benchmark needed |
| Resource load/preload | Tested, benchmark needed |
| PackedScene instantiate | Needed |
| CharacterBody2D movement loop | Needed |
| Physics process dispatch | Needed |
| Input polling | Needed |
| Tilemap/event-style scripting | Needed |
| Hot reload state preservation | Tested, benchmark/stress needed |
| Large project analyzer time | Needed |
| Autocomplete latency | Needed |
| Debugger stack/locals overhead | Needed |

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

Sorbet-style `T.sig`, `T::Enum`, and similar syntax are intentionally not part of normal Lunari authoring. If Sorbet-like ideas are useful, they belong in the analyzer/compiler behind the scenes, not in the game script surface.

## Roadmap

The next big work is not more syntax for its own sake. It is replacement confidence:

1. Expand benchmark coverage into gameplay-like loops.
2. Harden debugger parity.
3. Stress test hot reload on deeper object graphs.
4. Prove analyzer performance on large projects.
5. Build RPG/event scripting fixtures.
6. Add migration tooling for GDScript-to-Lunari experiments.
7. Continue tightening Godot API metadata and typed native call paths.

## License

Lunari currently lives inside this Godot fork as a native module. See the module `LICENSE` and the Godot repository license files for details.
