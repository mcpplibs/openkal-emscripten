# openkal-emscripten

An implementation of [openkal](https://github.com/mcpplibs/openkal) for
Emscripten, written **above** a C library rather than beneath one.

Every other implementation in this ecosystem is written on a kernel's own
interface: a register discipline, a trap instruction, and a table of numbers.
Emscripten has no kernel to issue a call to. It has a C library over a
JavaScript host, and clause 2 of the specification permits exactly this
arrangement in as many words: "an implementation may be built upon a C library,
beneath one, or without one." This is the first implementation here to take the
first of the three.

That direction makes the code thin and not easy. A forward and an error
translation is most of each function; what an above-libc implementation has to
get right is the places where the C library's vocabulary and openkal's do
**not** correspond, and those are the places the sources comment individually.

## What it provides

Twelve of the fifteen interfaces, in whole:

| interface | notes |
|---|---|
| `openkal.version` | the self-description every implementation exports |
| `openkal.abort` | the message reaches the host through a descriptor, not stdio |
| `openkal.stream` | the standard streams, borrowed |
| `openkal.memory` | `aligned_alloc`; the granularity is the alignment honoured, not the wasm page |
| `openkal.env` | variables from `environ`; arguments from the host, which is the one question the C library between them does not answer |
| `openkal.time` | the monotonic granularity is measured rather than asked for, because a browser deliberately coarsens its clock |
| `openkal.random` | `getentropy`, which is the host's cryptographic generator and not MEMFS's `/dev/urandom` |
| `openkal.fs` | MEMFS, with one preopen at `/`; locks and capacity are withheld by the property word |
| `openkal.terminal` | asked of the machine, so the same module answers correctly under node and in a browser |
| `openkal.net` | the calls are real and the transport is a WebSocket proxy; `kal_net_props` claims nothing |
| `openkal.datagram` | the same |
| `openkal.timeout` | `poll` and then the operation; the granularity is `poll`'s millisecond, not the clock's |

## What it does not provide, and why the absence is the report

`openkal.process`, `openkal.exec` and `openkal.space` are **not** provided.
There is no fork and no exec on this platform; there is no way to publish bytes
as executable code, because a wasm module is instantiated by the host from
bytes it validates; and a module has one linear memory and cannot obtain a
second.

A program that uses one of those seventeen names fails at **link**, naming the
symbol. That is clause 6.2's second time, and it is the mechanism rather than a
defect: providing `kal_process_spawn` so that it returned an error would be the
shape the specification forbids -- present and always failing, which the caller
cannot tell from a condition -- and it would move a fact known at link time to
run time.

## `openkal.task` and the whole-graph `-pthread`

Threads are a **link-time** decision on this platform. Emscripten compiles
`pthread_create` either way; whether it can create anything depends on
`-pthread`, which selects a different C library build, a different memory model
(a `SharedArrayBuffer`) and a different loader contract.

So the interface is carried by a feature:

```toml
[dependencies]
openkal-emscripten = { version = "0.1.1", features = ["threads"] }
```

Without it, `src/threads/task.cpp` compiles to nothing, the eight `kal_task_*`
symbols do not exist, and `kal_interfaces()` does not claim the interface --
the same treatment the three absent interfaces get, for the same reason.

**The switch belongs to the artefact, and the root manifest states it.**
`-pthread` changes the module configuration of *every* translation unit in the
link, including the specification package's, so it cannot be a flag of this
package's own units. With mcpp 2026.9.12.2 or later, the consumer writes

```toml
[target.'cfg(os = "emscripten")'.abi]
threads = true
```

and the switch reaches the standard library module, every translation unit of
every package, and the link. The `threads` feature states that it needs the
switch (`requires_abi = { threads = true }`), so a consumer that activates the
feature without the table is refused before anything compiles, naming the
feature and the table. Measured 2026-09-12 with emsdk 6.0.9 and a development
build of mcpp 2026.9.12.2: with the table, a program that starts a task and
joins it exits 0 under node, and without it the build stops at

```
error: `openkal-emscripten` requires the artefact's ABI to have threads (feature `threads`), and this build does not state it.
```

An earlier version of this section, measured 2026-09-11, concluded that mcpp had
no channel for a flag that applies to a whole dependency graph. What had been
measured was `-pthread` in the consumer's per-package `cxxflags`:

```
error: POSIX thread support was disabled in precompiled file
       '.../pcm.cache/openkal.types.pcm' but is currently enabled
```

That channel does not reach the specification package's module. A graph-wide
channel did exist (`[build] dialect_cxxflags`); what was missing was a way to
scope it to one target and to let this feature state its requirement, which the
typed table provides.

## Conformance

Measured 2026-09-11, `emsdk 6.0.9`, run under node:

```
$ OPENKAL_CONFORMANCE_RUNNER=<node> \
  bash openkal/tools/run-conformance.sh openkal-emscripten . \
       core,env,time,random,fs,terminal,timeout --target wasm32-emscripten

observations: 86 held, 0 did not hold, 13 not observed
the implementation conforms in every observation made
```

The thirteen unobserved are the interfaces this implementation does not provide
(`process`, `exec`, `space`, and the three `abort` observations that need a
started copy of the suite to make), plus the four capability words it withholds
(`fs` locks and capacity, `task` timeout and thread-local storage), plus the
two environment observations that need the runner to set a variable.

The exported surface is checked against the specification's own `SURFACE.txt`:
86 names, twelve complete groups, three absent groups, and nothing beginning
with `kal_` that the specification does not name.

## Building it, and what it needs

```bash
mcpp build --target wasm32-emscripten
```

Nothing has to be declared. The `wasm32-emscripten` row names its own payload
(`emsdk@6.0.9`), the payload brings its own sysroot and its own libc++ module
surface, and `mcpp run --target wasm32-emscripten` executes the module with
`node`.

A consumer selects this implementation by platform and names no
implementation in its source:

```toml
[dependencies]
openkal = "0.12.0"

[target.'cfg(os = "emscripten")'.dependencies]
openkal-emscripten = "0.1.1"
```

## Licence

Apache-2.0.
