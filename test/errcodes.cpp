// errcodes.cpp — the frozen error-code table test (docs/SURFACE.tsv gate).
//
// The engine's `corvid::Error` variants surface in this binding as the
// frozen code table (FFI.md §1.3: values are frozen, never renumbered),
// twice: the ABI's error enum (corvid.h) and this library's ErrorCode
// (corvid.hpp). The golden fixtures prove the codes the suites can
// trigger (err:10/11/12/14/15/17 lines); the redb-internal fault paths
// have no public-API trigger (the engine's own radar exempts them), so
// the tables themselves — checked here at COMPILE TIME, so they can
// never rot — are the proof that every variant maps to its documented
// code on both sides.
//
// static_assert keeps this zero-cost: any drift in the shipped header
// or in this library's mirror fails the BUILD, not just a runtime check.

#include <cstdint>

// The shipped ABI header directly: this test pins THAT artifact's
// table, so it must read the artifact, not a re-declaration. The
// header is portable C11/C++ since v0.3.1 (the C23-presenting prelude
// this file used to share with src/corvid.cpp and test/golden.cpp is
// deleted at this pin bump); the extern "C" wrapper stays because the
// engine header carries no __cplusplus self-guard.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "corvid.h"
}

#include "corvid/corvid.hpp"

namespace {

// The shipped ABI table, pinned against its documented frozen values.
#define FROZEN(code, want)                                       \
    static_assert((code) == (want), "error-code table drifted: " #code)

FROZEN(CORVID_E_OK, 0);
FROZEN(CORVID_E_DATABASE, 1);
FROZEN(CORVID_E_TRANSACTION, 2);
FROZEN(CORVID_E_TABLE, 3);
FROZEN(CORVID_E_STORAGE, 4);
FROZEN(CORVID_E_COMMIT, 5);
FROZEN(CORVID_E_SET_DURABILITY, 6);
FROZEN(CORVID_E_COMPACTION, 7);
FROZEN(CORVID_E_DECODE, 8);
FROZEN(CORVID_E_CORRUPT_INDEX, 9);
FROZEN(CORVID_E_RESERVED_COLLECTION, 10);
FROZEN(CORVID_E_INVALID_NAME, 11);
FROZEN(CORVID_E_ARGUMENT, 12);
FROZEN(CORVID_E_INCOMPATIBLE_FORMAT, 13);
FROZEN(CORVID_E_EMPTY_INDEX_TRAINING, 14);
FROZEN(CORVID_E_SCHEMA_VIOLATION, 15);
FROZEN(CORVID_E_INVALID_DUMP, 16);
FROZEN(CORVID_E_BACKUP_TARGET_EXISTS, 17);
FROZEN(CORVID_E_IO, 18);
FROZEN(CORVID_E_BUSY, 19);

// This library's ErrorCode mirrors the ABI table one-for-one, pinned
// against the shipped constants so the two can never drift apart.
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Ok) == CORVID_E_OK);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Database) == CORVID_E_DATABASE);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Transaction) == CORVID_E_TRANSACTION);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Table) == CORVID_E_TABLE);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Storage) == CORVID_E_STORAGE);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Commit) == CORVID_E_COMMIT);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::SetDurability) == CORVID_E_SET_DURABILITY);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Compaction) == CORVID_E_COMPACTION);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Decode) == CORVID_E_DECODE);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::CorruptIndex) == CORVID_E_CORRUPT_INDEX);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::ReservedCollection) == CORVID_E_RESERVED_COLLECTION);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::InvalidName) == CORVID_E_INVALID_NAME);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::InvalidArgument) == CORVID_E_ARGUMENT);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::IncompatibleFormat) == CORVID_E_INCOMPATIBLE_FORMAT);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::EmptyIndexTraining) == CORVID_E_EMPTY_INDEX_TRAINING);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::SchemaViolation) == CORVID_E_SCHEMA_VIOLATION);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::InvalidDump) == CORVID_E_INVALID_DUMP);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::BackupTargetExists) == CORVID_E_BACKUP_TARGET_EXISTS);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Io) == CORVID_E_IO);
static_assert(static_cast<std::uint32_t>(corvid::ErrorCode::Busy) == CORVID_E_BUSY);

}  // namespace

int main() {
    // The tables are the test; reaching here means every assert held.
    return 0;
}
