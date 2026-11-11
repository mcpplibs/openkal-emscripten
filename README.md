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

Twelve of the sixteen interfaces, in whole:

| interface | notes |
|---|---|
| `openkal.version` | the self-description every implementation exports |
| `openkal.abort` | the message reaches the host through a descriptor, not stdio |
| `openkal.stream` | the standard streams, borrowed |
| `openkal.memory` | `aligned_alloc`; the granularity is the alignment honoured, not the wasm page |
| `openkal.env` | variables from `environ`; arguments from the host, which is the one question the C library between them does not answer; the image is the script node was given (not found in a page), there is no loader, and the system is `emscripten` |
| `openkal.time` | the monotonic granularity is measured rather than asked for, because a browser deliberately coarsens its clock |
| `openkal.random` | `getentropy`, which is the host's cryptographic generator and not MEMFS's `/dev/urandom` |
| `openkal.fs` | MEMFS, with one preopen at `/`; a name held open is removed at once; a name with every link followed is the one MEMFS keeps for a descriptor of it; locks, capacity, a rename that does not replace and a rendezvous at a name are withheld by the property word |
| `openkal.terminal` | a standard stream is a terminal when the host says so (node's `tty.isatty`; never in a browser), because the C library answers for its own device even under a pipe; a terminal obeys control sequences |
| `openkal.net` | the calls are real and the transport is a WebSocket proxy; `kal_net_props` claims nothing, and a rendezvous at a name is not supported |
| `openkal.datagram` | the same |
| `openkal.timeout` | `poll` and then the operation; the granularity is `poll`'s millisecond, not the clock's |

## What it does not provide, and why the absence is the report

`openkal.process`, `openkal.exec`, `openkal.space` and `openkal.map` are
**not** provided. There is no fork and no exec on this platform; there is no way
to publish bytes as executable code, because a wasm module is instantiated by
the host from bytes it validates; a module has one linear memory and cannot
obtain a second; and the C library's `mmap` of a file copies the file's bytes
into linear memory, so a shared region would neither show another region's
writes nor carry its own to the file before a sync, which clause 11 requires of
a shared region.

openkal 0.21 adds to two of them --- `kal_exec_map_part` to `openkal.exec`, and
the processor time of an ended program to `openkal.process` --- so 0.10.0 is
this implementation written against 0.21, with nothing else changed.
openkal 0.22 adds the null device at a position of a started program to
`openkal.process`, so 0.11.0 is this implementation written against 0.22, with
nothing else changed. openkal 0.23 adds a region at an address to
`openkal.exec`, and a name with every link followed to `openkal.fs`, which
0.12.0 answers from the name MEMFS keeps for a descriptor of it
(`kal_fs_resolved_name`, `/proc/self/fd/N`). It also adds a handle with less
(`kal_fs_dir_attenuate`), which 0.12.0 answers with the table of the other
implementations over this C library --- read-only handles, a handle that states
nothing beneath changes, and a record appended through the descriptor's name
reopened to append --- a listing with attributes (`kal_fs_list_next_info`, the
rest of an entry asked of it), what the module may still use
(`kal_memory_info`: the heap's limit, what is not grown yet and what the
allocator holds free), and the resource preference (`KAL_RESOURCES`). A listing
now ends with `kal_ok`, as the specification states from 0.23; it ended with
`kal_err_not_found` before.

A program that uses one of those thirty names fails at **link**, naming the
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
openkal-emscripten = { version = "0.13.0", features = ["threads"] }
```

Without it, `src/threads/task.cpp` compiles to nothing, the ten `kal_task_*`
symbols do not exist, and `kal_interfaces()` does not claim the interface --
the same treatment the four absent interfaces get, for the same reason.

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

## C's non-local jumps, and the same table's third key

A `sigsetjmp`/`siglongjmp` pair is a **link-time** decision on this platform,
for the reason threads are: `setjmp.h` compiles either way, and whether a jump
over live frames works depends on which lowering the whole link chose -- the
wasm exception runtime's tables and the symbols that land on them, or the
host's JS trampoline that the invocation goes through when the link did not
carry them. A jump through the trampoline is not a jump: the invocation leaves
the module, and the frames it means to unwind over end it instead. So the
switch belongs to the artefact, not to any package's own units, and the
consumer states it in the same abi table threads use:

```toml
[target.'cfg(os = "emscripten")'.abi]
longjmp = "wasm"
```

**Without the table, non-local jumps are not supported**, which is a boundary
of the link and not a defect a package could patch: the lowering cannot be
chosen after the module is instantiated, and no flag of this package's own
translation units reaches the frames of the consumer's.

The assertion is carried by a program rather than a claim: `probe/longjmp/` is
a C program whose `siglongjmp` crosses two frames, each holding a `volatile`
local the jump must not have restored, and continuous integration builds and
runs it under node once an engine resolves the key. Measured 2026-11-11 with
mcpp 2026.11.10.1 and `emsdk 6.0.9`: the engine has no member `longjmp` in the
abi table (its members are `threads` and `exceptions`), and a manifest naming
it is refused naming the member -- so the step reads that refusal and reports
a skip, and the boundary above is the state until an engine that resolves the
key is released and the same step turns green with it.

## The region a context stands on

`kal_task_stack` reports the stack of the calling context, and this platform
publishes it per context: the runtime that starts a thread calls
`emscripten_stack_set_limits` for it before its entry runs, so
`emscripten_stack_get_base` and `emscripten_stack_get_end` answer about the
context that asks, and no record is kept here.

The stack grows down, so the end is the low address and the base the high one,
and the region is the pair. The end is the end of the *stack* and not of the
memory: below it is the guard Emscripten places for a thread that overflowed,
which is why the answer begins at the end rather than at the bottom of what the
thread was given.

The continuous integration gate that already builds a task program under
`-pthread` also asks each of its two contexts for its own region and asserts
that the region contains a local of that context.

## What 0.14.1 adds

Nothing the twelve interfaces do. C's non-local jumps are stated as a
link-time decision of the artefact -- `longjmp = "wasm"` in the same abi table
threads use -- the probe program `probe/longjmp/` carries the assertion, and
continuous integration runs it once an engine resolves the key, reporting a
skip until one does.

## What 0.14 adds (openkal 0.25)

`kal_fs_hard_link` is present and refuses: MEMFS stores one name for each node
(`linkat` reports `EMLINK`), so `KAL_FS_PROP_HARD_LINKS` is not claimed. The
manifest states `requires = ["mcpp:c-abi"]`: an implementation written above a C
library is never placed beneath one that is composed over it.

## Conformance

Measured 2026-10-31 against openkal 0.18.0, `emsdk 6.0.9`, run under node in
continuous integration:

```
$ OPENKAL_CONFORMANCE_RUNNER=<node> \
  bash openkal/tools/run-conformance.sh openkal-emscripten . \
       core,env,time,random,fs,terminal,timeout --target wasm32-emscripten

observations: 97 held, 0 did not hold, 16 not observed
the implementation conforms in every observation made
```

The standard streams there are a pipe, so the terminal section observes only
that `get_mode` refuses them. With a terminal (under `script`), measured
2026-10-28 against openkal 0.17.1, the count was 109 held, 0 did not hold, 14
not observed.

The unobserved are the interfaces this implementation does not provide
(`process`, `exec`, `space`, `map`, and the three `abort` observations that need
a started copy of the suite to make), the interfaces the run did not select
(`task`, `net`, `datagram`), the three capability words of the volume it
withholds (locks, capacity, a rename that does not replace), the two
environment observations that need the runner to set a variable, and under a
pipe the terminal's modes.

The exported surface is checked against the specification's own `SURFACE.txt`:
85 names, twelve complete groups, four absent groups, and nothing beginning
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
openkal = "0.25.0"

[target.'cfg(os = "emscripten")'.dependencies]
openkal-emscripten = "0.14.1"
```

## Licence

Apache-2.0.
