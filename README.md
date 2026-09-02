# corvid-cpp

C++20 bindings for [corvid](https://github.com/corvid-db/corvid) — an
embedded database with a typed C ABI — as a **header-first RAII
library**: every engine handle becomes a move-only class whose
destructor frees it, every failing call throws `corvid::Error` with the
frozen error `code()`, and no raw ABI type ever appears in the public
API (`include/corvid/corvid.hpp`).

The binding links the **published FFI artifacts** — the platform cdylib,
`corvid.h`, and the golden fixtures shipped in each release archive —
fetched from a pinned engine release (v0.3.2) and sha256-verified. Its
correctness floor is a full C++ port of the engine's golden-suite
harness, run against the downloaded library on every CI leg.

**Documentation:** the [corvid docs site](https://corvid-db.github.io/docs/)
is canonical — this binding has its own
[corvid-cpp page](https://corvid-db.github.io/docs/bindings/corvid-cpp/),
and the [C ABI section](https://corvid-db.github.io/docs/ffi/) documents
every symbol underneath.

## What's inside

| Path | What it is |
| --- | --- |
| `include/corvid/corvid.hpp` | The public header — `corvid::Db`, `Collection`, `Value`, `Predicate`, `Query`, `Rows`, exceptions; C++20, no dependencies beyond the standard library |
| `src/corvid.cpp` | The single implementation TU where the ABI lives (the header never names it) |
| `fetch.sh` / `fetch.ps1` | Download the pinned release archive for the host platform, verify against the release's `checksums.txt` (sha256), extract into gitignored `deps/` |
| `CMakeLists.txt` | Offline-first build consuming `deps/`; builds the library, the examples tour, the golden-suite port; installs a `find_package(corvid)` package |
| `examples/{quickstart,hybrid,vector_index,text_search,graph,geo}.cpp` | The examples tour — one runnable program per concept (also ctests): the README quickstart, hybrid RRF+MMR, the vector-index families, BM25 incl. CJK + the phrase API, graph + delete cascade, geo radius/bbox/nearest |
| `test/golden.cpp` | The golden-suite port — replays the engine's 267-line fixture suite (including the v0.3.0 map-keys and phrase ops; byte-identical at v0.3.2) against the downloaded libcorvid |
| `test/raii.cpp` | The RAII library's unit tests (145 checks): literals, initializer lists, `map_keys`, predicate builders, the fluent query, phrase search, exceptions, move semantics, TTL/graph/geo/schema/admin |
| `test/errcodes.cpp` | The frozen error-code tables, pinned at compile time (the shipped header's and this library's mirror) |
| `docs/PLAN.md` | The binding's plan: architecture ruling, binding rules, phase scope |
| `docs/SURFACE.tsv` | The surface manifest — every engine construct mapped or `N/A` with a reason (CI-gated) |

## Quick start

Requirements: a C++20 compiler (GCC 13+/Clang 16+/MSVC 19.36+ — CI
runs latest-ish versions of all three), CMake ≥ 3.28 (the Ubuntu 24.04
LTS system CMake), and one of `curl` + `shasum`/`sha256sum`
(macOS/Linux) or PowerShell 5+ (Windows).

```sh
./fetch.sh                     # download + verify corvid v0.3.2 into deps/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # golden suite + raii + examples
./build/bin/example_hybrid                    # the flagship hybrid query
```

Windows (PowerShell): `./fetch.ps1`, then the same CMake steps
(`ctest -C Release`; the examples land in `build\bin\Release\`).

A taste of the API (`examples/quickstart.cpp`):

```cpp
#include "corvid/corvid.hpp"

corvid::Db db = corvid::Db::open_memory();
corvid::Collection docs = db.collection("docs");

docs.insert("p1", corvid::Value::map({
    {"title", corvid::lit::text("rust embedded database")},
    {"v", corvid::lit::vec(probe)},            // std::span<const float>
}));

for (const corvid::Row& r :
     docs.query().vector("v", probe, 3, corvid::Metric::Cosine).run())
    std::printf("%.*s score=%f\n", ...);        // RAII frees everything
```

The flagship hybrid query is one fluent chain (`examples/hybrid.cpp`):

```cpp
corvid::Rows rows = docs.query()
    .filter(corvid::pred::eq("kind", "doc"))
    .vector("v", probe, 2, corvid::Metric::Cosine)
    .text("body", "rust database", 2)
    .fuse_rrf()            // k defaults to the engine's 60
    .rerank_mmr(1.0f)
    .limit(2)
    .run();
```

And the direct positional search (engine v0.3.0+) (`examples/text_search.cpp`):

```cpp
for (const corvid::Row& r :
     docs.phrase_search("body", "embedded database", 10)) { … }
```

## Consuming the library

Two supported paths, both CMake:

**FetchContent / add_subdirectory** (from-source, the CI shape):

```cmake
FetchContent_Declare(corvidcpp GIT_REPOSITORY https://github.com/corvid-db/corvid-cpp.git
                            GIT_TAG v0.1.0)
FetchContent_MakeAvailable(corvidcpp)   # after running the repo's fetch.sh
target_link_libraries(my_app PRIVATE corvid::corvid)
```

The binding's build consumes its own `deps/` (populated by `fetch.sh` /
`fetch.ps1`) — offline-first by design, no network at configure time.

**find_package** (installed package):

```sh
cmake --install build            # headers, libcorvidpp, the engine cdylib
```

```cmake
find_package(corvid REQUIRED)
target_link_libraries(my_app PRIVATE corvid::corvid)
```

## Surface manifest (docs/SURFACE.tsv)

Every construct of the engine's public surface (the radar-enforced list
the engine publishes as `scripts/bindings/surface.tsv` at each release
tag) is resolved in `docs/SURFACE.tsv`: the C++ API exposing it plus
the test that proves it (golden fixture line references), or `N/A` +
reason where the v1 ABI deliberately does not expose it (FFI.md §9).
`scripts/surface-gate.sh` fails CI when a line is unresolved, a cell is
empty, or the N/A count drifts from the committed baseline — so an
engine pin bump that changes the surface lands in this gate, not in a
user's bug report. `scripts/idiom-gate.sh` is the C++ idiom radar: the
public header must carry no raw ABI tokens.

## Versioning

The engine pin lives in one variable in the fetch scripts
(`CORVID_VERSION=v0.3.2`). Artifacts are always taken from that exact
tag's GitHub release and sha256-verified; `deps/` is never committed.

## License

MIT — see [LICENSE](LICENSE).
