# corvid-cpp — the binding's plan

corvid-cpp is the **C++ binding** for the `corvid` embedded database's
FFI: a header-first RAII library over the frozen C ABI (engine
`crates/corvid-ffi/corvid.h`, v0.3.1, 124 symbols), with the golden
suite as its correctness floor and the published release artifacts as
its only engine dependency.

Engine repo: `corvid-db/corvid` (read-only upstream; never a submodule,
never vendored).

## The locked rule: golden port BEFORE ergonomic sugar

Inherited from the bindings program's master plan and non-negotiable:

> **A binding opens with the golden-suite port.** The engine's golden
> fixtures (267 executable lines across 8 files at v0.3.1) are the
> contract; a binding that wraps the ABI before it can replay the
> contract is building on unverified ground. No ergonomic sugar ships
> until the port is green against a tagged release's published
> artifacts.

Concretely: corvid-cpp's first substantive deliverable is
`test/golden.cpp` — the C++ port of the engine's `c/smoke.c` harness at
the pinned tag — driven against the **downloaded** libcorvid from the
v0.3.1 GitHub release, including the v0.3.0 additive OPs (VMAP_KEYS /
GET_KEYS over `corvid_value_map_keys`, PHRASE / PHRASE_K0 over
`corvid_phrase_search`; v0.3.1 was a header-only portability fix — the
golden fixtures are byte-identical across the two tags). Only with
that green in CI does the RAII
library's own surface count for anything (`test/raii.cpp` proves it).

## Architecture ruling (binding-specific)

A **header-first RAII library over the C ABI**, C++20, no dependencies
beyond the C++ standard library and the engine cdylib:

- **One public header** — `include/corvid/corvid.hpp` — and one
  implementation TU (`src/corvid.cpp`) where every ABI symbol lives.
  Consumed via CMake: `FetchContent`/`add_subdirectory` of THIS repo
  (after its `fetch.sh`) or `find_package(corvid)` against an installed
  package.
- **RAII handle wrappers**: `Db`, `Collection`, `Value`, `Predicate`,
  `Query`, `Rows`, `Strs`, `GeoHits`, `GroupIter` — destructors call
  the ABI's `_free` family (or `close` for the db); **moves not
  copies** (a copied handle would double-free; `test/raii.cpp` pins
  `!std::is_copy_constructible` for every wrapper at compile time).
  Deep copies exist only where the ABI offers one (`Value::clone()`).
- **No raw ABI surface in the public API**: no ABI type, function, or
  constant appears in the public header — handles are stored as opaque
  pointers behind a private `detail::Access` bridge.
  `scripts/idiom-gate.sh` enforces this in CI by scanning the header
  for the ABI token scheme.
- **`corvid::Value`**: construction from scalars, `lit::text/bytes/vec`
  tags, nested composites by borrowing an owned `Value`, and
  initializer-list literals (`Value::map({{"k", v}, ...})`,
  `Value::array({...})`); typed accessors returning `std::optional` /
  `std::span`; borrowed children through the read-only `ValueView`;
  and **`map_keys()`** — the v0.3.0 additive symbol — returning owned
  keys in ascending key-byte order.
- **Predicate builders** (`corvid::pred::eq/ne/…`, `all/any/none`)
  returning the move-only `Predicate`; consumption follows the ABI
  (handed to `Query::filter` / `Collection::erase_where` by rvalue).
- **Fluent `Query` builder** mirroring the engine's Rust builder
  (`filter → vector/text → fuse_rrf/rerank_mmr → limit/offset/order_by/
  select → run`), with the aggregate terminals (`count`, `sum`, `avg`,
  `min`/`max`, `group_*`) consuming the builder exactly as the ABI does.
- **Exceptions**: every failing call throws `corvid::Error` with the
  frozen `code()` (mirroring the ABI's error enum 1:1 —
  `test/errcodes.cpp` pins both tables at compile time) and a copy of
  the engine's last recorded message. Destructors never throw; `Db`'s
  destructor closes best-effort and `Db::close()` is the explicit
  throwing form.
- **Callbacks**: `Collection::scan` and `Collection::update` bridge the
  ABI's C callbacks onto `std::function` through C-linkage thunks;
  exceptions thrown inside a callback stop the walk, are carried across
  the C frame, and rethrow after the call returns (the update path
  aborts with the engine's argument error, per the ABI contract).
- **Toolchain policy** (engine `scripts/bindings/README.md`): modern
  minimums, no compat base to protect — **C++20 floor**, CMake ≥ 3.28
  (Ubuntu 24.04 LTS system CMake), CI on latest-ish GCC, Clang, and
  MSVC across linux/macos/windows.

### The C-header-under-C++ note (historical; resolved at v0.3.1)

The published `corvid.h` is a C header by contract. Through v0.3.0 its
enum idiom — `enum corvid_status` plus, for pre-C23 compilers,
`typedef uint32_t corvid_status` — was two types under one name, which
C++ rejects (in C they live in different namespaces). The workaround was
not an artifact patch (see the rules below) but presentation: a
preprocessor prelude presenting C23 to the header selected its
fixed-underlying-type branch (`enum X : uint32_t` + a same-name
typedef), which is plain valid C++. The trick was scoped to the single
`#include "corvid.h"` in each of the three ABI-touching TUs
(`src/corvid.cpp`, `test/golden.cpp`, `test/errcodes.cpp`) and restored
immediately after.

**Resolved:** the engine shipped the portable header in **v0.3.1** — the
generated enums are the plain `typedef enum <tag> { ... } <tag>;` the
spec shows, valid C11, C23, and every C++ standard (found by this
binding; engine CHANGELOG 0.3.1). Per the plan of record, this bump
commit (the pin's move to v0.3.1) DELETED the prelude and its twins in
`test/golden.cpp` / `test/errcodes.cpp` rather than carrying dead code;
the `extern "C"` wrappers stay (the header carries no `__cplusplus`
self-guard). The v0.3.0-era idiom lives on only in this note as history.

## Binding rules (from the master plan)

- **Pin EXACT engine tags.** One engine version at a time; today it is
  `v0.3.1`. The pin lives in exactly one variable per fetch script
  (`CORVID_VERSION`) and is stamped into `deps/version.txt`; CMake
  reads the stamp, never guesses.
- **Artifacts come from the tag's GitHub release**, not from a local
  build of the engine: `https://github.com/corvid-db/corvid/releases/download/<tag>/…`,
  verified against the release's `checksums.txt` (sha256) before anything
  is extracted or used.
- **No vendored binaries in git.** `deps/` (the extracted engine
  artifacts) is gitignored; every consumer — human, CI — runs
  `fetch.sh`/`fetch.ps1` to populate it deterministically.
- **No FetchContent / no network at build time.** CMake consumes
  `deps/` only; the build is offline-first once fetch has run.
- **Published-artifact defects are findings, not patches.** If the
  released header/dylib/fixtures disagree in a way that blocks this
  repo, we stop and report upstream (`corvid-db/corvid`). We never
  carry a local header patch or fixture edit to work around a bad
  artifact.

## Phase CPP1 (this bootstrap) — scope

1. **Plan doc** (this file) — the binding's own program, written first.
2. **Repo scaffold** — README (role, usage, requirements), MIT LICENSE
   (matching corvid's copyright line), `.gitignore` (`build/`, `deps/`).
3. **Fetch + verify** — `fetch.sh` / `fetch.ps1` (founding pin v0.3.0,
   the first release carrying the additive ABI: map keys + phrase
   search; now v0.3.1, byte-identical fixtures).
4. **The library** — `include/corvid/corvid.hpp` + `src/corvid.cpp`
   per the architecture ruling, packaged for CMake consumption
   (`corvid::corvid` target; FetchContent or find_package).
5. **The golden port** — `test/golden.cpp` replays the v0.3.1 fixture
   grammar (`OP<TAB>args<TAB>expected`) over every executable line of
   the release's `golden/*.txt` — 267 lines across 8 files, including
   the new VMAP_KEYS/GET_KEYS and PHRASE/PHRASE_K0 ops — with the same
   discipline as the engine driver: every counted line must dispatch,
   first failure names file:line + OP + expected-vs-got. **Success
   criterion: the same 267/267 green the engine-side suite reports.**
6. **The RAII tests** — `test/raii.cpp` (the library's own surface:
   literals, initializer lists, accessors, map keys, predicate
   builders, the fluent query, phrase search, exceptions with code(),
   move semantics, TTL/graph/geo/schema/admin round-trips) and
   `test/errcodes.cpp` (the frozen error tables, compile-time).
7. **The examples tour** — six runnable programs, each a ctest:
   quickstart, hybrid (RRF+MMR), vector-index families, text search
   (English + CJK + the phrase API), graph + delete cascade, geo.
8. **Gates + CI** — `scripts/surface-gate.sh` (docs/SURFACE.tsv vs the
   engine surface list at the pin; N/A baseline 147), the C++
   `scripts/idiom-gate.sh` (no raw ABI tokens in the public header),
   and `.github/workflows/ci.yml`: linux/macos/windows matrix over
   latest-ish GCC + Clang + MSVC running the whole ctest suite, an
   ASan/LSan leg (zero-leak expectation), and the two gates.

Out of scope for CPP1: vcpkg/Conan packaging, C++ module interfaces,
and coroutines-style iteration (the input iterators are the C++20
idiomatic floor).

## Verdict protocol

The golden port keeps the engine harness's output contract: one
`SMOKE <file> lines=<n> executed=<n>` line per fixture on stdout, exit
0 only when every expectation of every executable line passed and the
dispatch count matches the pre-scan count. Divergence from the
engine-side suite's pass/fail verdicts is a defect here; divergence of
the *artifacts* from the engine repo is a finding for the engine repo.

## Versioning

The engine pin lives in one variable in the fetch scripts
(`CORVID_VERSION=v0.3.1`). Bumps are a one-variable change plus a
re-run of the golden suite against the new artifacts; `bump.sh` in the
engine's `scripts/bindings/` registry opens the bump PR mechanically
(this repo is registered there).
