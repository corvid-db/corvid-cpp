#!/usr/bin/env bash
# idiom-gate.sh — the C++ idiom gate (the binding's OOP-idiom radar).
#
# The architecture ruling (docs/PLAN.md): corvid-cpp is a header-first
# RAII library over the C ABI with NO raw ABI surface in its public
# API — no ABI type, function, or constant may appear in
# include/corvid/corvid.hpp. The single translation unit src/corvid.cpp
# is where the ABI lives; the golden port (test/golden.cpp) drives the
# ABI directly BY DESIGN (it is the artifacts' contract check, not the
# wrapper's).
#
# This gate fails when any `corvid_<token>` (the ABI's naming scheme:
# types, functions, constants) appears in the public header — keeping
# the public API provably free of raw engine handles. The move-only
# half of the idiom is pinned at compile time by test/raii.cpp's
# static_asserts (std::is_copy_constructible == false for every handle
# wrapper), which run in the same CI matrix.
#
# Requirements: bash 3.2+, grep. shellcheck-clean.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADER="$ROOT/include/corvid/corvid.hpp"

[ -f "$HEADER" ] || { echo "idiom-gate: $HEADER is missing" >&2; exit 1; }

# Grep for the ABI token scheme: `corvid_` followed by a letter.
if grep -nE 'corvid_[a-zA-Z]' "$HEADER" >/dev/null; then
    echo "idiom-gate: FAIL: raw ABI tokens in the public header:" >&2
    grep -nE 'corvid_[a-zA-Z]' "$HEADER" >&2
    echo "" >&2
    echo "  The public API must expose no ABI type, function, or constant;" >&2
    echo "  move the mention into src/corvid.cpp (the single ABI TU)." >&2
    exit 1
fi

# The header must also stand alone: no include of the engine header.
if grep -nE '#include "corvid.h"' "$HEADER" >/dev/null; then
    echo "idiom-gate: FAIL: the public header includes the engine's C header" >&2
    exit 1
fi

echo "idiom-gate: ok — no raw ABI surface in include/corvid/corvid.hpp"
