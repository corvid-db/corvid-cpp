// corvid.hpp — corvid-cpp: a header-first RAII library over the corvid
// database's C ABI (engine v0.3.0, 124 frozen symbols).
//
// Architecture ruling (docs/PLAN.md): every engine handle becomes a
// move-only RAII class whose destructor calls the ABI's free family;
// moves not copies (a copy of a handle would double-free — deep copies
// exist only where the ABI offers one, `Value::clone()`); errors throw
// `corvid::Error`, which carries the frozen error `code()`; and NO raw
// ABI type or function name ever appears in this public header — the
// handles are stored as opaque pointers and everything ABI-shaped lives
// in the one translation unit that is src/corvid.cpp (scripts/idiom-gate
// .sh enforces this in CI by scanning this file for ABI tokens).
//
// Requires C++20. No dependencies beyond the C++ standard library and
// the engine's shared library.
//
// Ownership translation (the ABI's transfer rules, restated natively):
//   - Values passed to insert/put/update-style calls are CLONED by the
//     engine; your Value stays alive and owned by you.
//   - A Predicate handed to `Query::filter()` or `Collection::erase_where()`
//     is CONSUMED (moved); the wrapper you passed in is left empty.
//   - A Query is consumed by `run()` and every aggregate terminal
//     (count/sum/avg/min/max/group_*).
//   - Row documents, map/array children, and text/bytes/vector views are
//     BORROWED: valid until the next `next()` on the producing cursor or
//     until the parent value dies. `Value::clone()` is the sanctioned way
//     to keep borrowed data; ValueView is the read-only window onto it.

#ifndef CORVID_CPP_CORVID_HPP
#define CORVID_CPP_CORVID_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace corvid {

namespace detail {
struct Access;  // the library's private handle bridge (see src/corvid.cpp)
}

// ---------------------------------------------------------------------------
// Errors — the frozen code table, thrown
// ---------------------------------------------------------------------------

/// The engine's frozen error discriminants (never renumbered). The table
/// mirrors the ABI's error enum 1:1; test/errcodes.cpp pins both sides
/// at compile time.
enum class ErrorCode : std::uint32_t {
    Ok = 0,
    Database = 1,
    Transaction = 2,
    Table = 3,
    Storage = 4,
    Commit = 5,
    SetDurability = 6,
    Compaction = 7,
    Decode = 8,
    CorruptIndex = 9,
    ReservedCollection = 10,
    InvalidName = 11,
    InvalidArgument = 12,
    IncompatibleFormat = 13,
    EmptyIndexTraining = 14,
    SchemaViolation = 15,
    InvalidDump = 16,
    BackupTargetExists = 17,
    Io = 18,
    Busy = 19,
};

/// The exception every failing call throws. Carries the frozen `code()`
/// and a copy of the engine's last recorded error message (the message is
/// copied because the engine's slot is thread-local and mutable).
class Error : public std::exception {
public:
    /// Captures the thread's last recorded engine error verbatim.
    explicit Error(ErrorCode code);

    ErrorCode code() const noexcept { return code_; }
    std::string_view message() const noexcept { return message_; }
    const char* what() const noexcept override { return what_.c_str(); }

private:
    ErrorCode code_;
    std::string message_;  // copied out of the engine's thread-local slot
    std::string what_;     // "corvid: [<code>] <message>" for what()
};

// ---------------------------------------------------------------------------
// Value model
// ---------------------------------------------------------------------------

/// A value's discriminant, mirroring the ABI's frozen type tags 0..=8.
enum class Type : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    Text = 4,
    Bytes = 5,
    Array = 6,
    Map = 7,
    Vector = 8,
};

namespace lit {

/// Literal tags: disambiguate overloaded constructors (a string is text,
/// not bytes; a brace of floats is not a vector until you say so).
struct Text {
    std::string_view s;
};
struct Bytes {
    std::span<const std::uint8_t> b;
};
struct Vec {
    std::span<const float> v;
};

inline constexpr Text text(std::string_view s) { return Text{s}; }
inline constexpr Bytes bytes(std::span<const std::uint8_t> b) { return Bytes{b}; }
inline constexpr Vec vec(std::span<const float> v) { return Vec{v}; }

}  // namespace lit

/// A copyable reference to one value literal — null, bool, int, float,
/// text, bytes, vector, or a borrowed owned Value (cloned at
/// materialization; this is how nested composites compose inside the
/// initializer-list constructors). It borrows its bytes: the referenced
/// storage must outlive the full-expression that materializes the Value
/// (string literals and named buffers always do).
class Value;

class Lit {
public:
    constexpr Lit() noexcept : kind_(Kind::Null) {}
    constexpr Lit(std::nullptr_t) noexcept : kind_(Kind::Null) {}
    constexpr Lit(bool b) noexcept : kind_(Kind::Bool), bool_(b) {}
    constexpr Lit(std::int64_t i) noexcept : kind_(Kind::Int), int_(i) {}
    constexpr Lit(int i) noexcept : kind_(Kind::Int), int_(i) {}
    constexpr Lit(double d) noexcept : kind_(Kind::Float), float_(d) {}
    constexpr Lit(const char* s) noexcept
        : kind_(Kind::Text), text_(s, s ? std::char_traits<char>::length(s) : 0) {}
    constexpr Lit(std::string_view s) noexcept : kind_(Kind::Text), text_(s) {}
    constexpr Lit(lit::Text t) noexcept : kind_(Kind::Text), text_(t.s) {}
    constexpr Lit(lit::Bytes b) noexcept : kind_(Kind::Bytes), bytes_(b.b) {}
    constexpr Lit(lit::Vec v) noexcept : kind_(Kind::Vector), vec_(v.v) {}
    Lit(const Value& v) noexcept : kind_(Kind::Value), value_(&v) {}

private:
    friend class Value;
    friend struct detail::Access;
    enum class Kind : std::uint8_t { Null, Bool, Int, Float, Text, Bytes, Vector, Value };
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double float_ = 0.0;
    std::string_view text_{};
    std::span<const std::uint8_t> bytes_{};
    std::span<const float> vec_{};
    const Value* value_ = nullptr;
};

/// A read-only window onto a BORROWED value — a row document, a map/array
/// child, or the `current` an update callback sees. No ownership: the view
/// dies with its parent (the cursor's next step, the parent Value, or the
/// collection handle). An absent/invalid view converts to false.
class ValueView {
public:
    constexpr ValueView() noexcept = default;

    explicit constexpr operator bool() const noexcept { return h_ != nullptr; }

    Type type() const noexcept;
    bool is_null() const noexcept;
    std::size_t len() const noexcept;

    std::optional<bool> as_bool() const noexcept;
    std::optional<std::int64_t> as_int() const noexcept;
    std::optional<double> as_float() const noexcept;
    std::optional<std::string_view> as_text() const noexcept;
    /// Empty span when the value is not bytes (the ABI's wrong-type
    /// convention — not an error).
    std::span<const std::uint8_t> as_bytes() const noexcept;
    std::span<const float> as_vector() const noexcept;

    /// Borrowed child under `key`; an invalid view when absent or the
    /// receiver is not a map.
    ValueView get(std::string_view key) const noexcept;
    /// Borrowed child at `index`; an invalid view when out of range or
    /// the receiver is not an array.
    ValueView at(std::size_t index) const noexcept;
    /// The map's keys, owned copies in ascending key-byte order (empty
    /// for a non-map — the ABI's inert rule).
    std::vector<std::string> map_keys() const;

private:
    friend struct detail::Access;
    const void* h_ = nullptr;
};

/// An OWNED engine value — documents, literals, aggregates' results.
/// Move-only (a copied handle would double-free); deep copies are
/// explicit via `clone()`.
class Value {
public:
    /// A null value.
    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool b);
    Value(std::int64_t i);
    Value(int i);
    Value(double d);
    Value(std::string_view s);         ///< text
    Value(const char* s);              ///< text
    Value(lit::Text t);                ///< text
    Value(lit::Bytes b);               ///< bytes
    Value(lit::Vec v);                 ///< vector
    /// Any literal (the overload set above, resolved without ambiguity).
    Value(Lit l);

    /// array({"a", 1, 2.5, text("x")}) — an owned array from literals.
    static Value array(std::initializer_list<Lit> items);
    /// map({{"title", text("ada")}, {"v", vec(p)}}) — an owned map.
    static Value map(std::initializer_list<std::pair<std::string_view, Lit>> entries);
    static Value text(std::string_view s);
    static Value bytes(std::span<const std::uint8_t> b);
    static Value vec(std::span<const float> v);
    /// An empty owned array / map, for incremental building.
    static Value empty_array();
    static Value empty_map();

    ~Value();
    Value(Value&& other) noexcept;
    Value& operator=(Value&& other) noexcept;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    /// Deep copy (the ABI's sanctioned escape for borrowed data).
    Value clone() const;

    /// Append to an owned array (throws on a non-array receiver).
    Value& push(Lit item);
    /// Insert/overwrite `key` in an owned map (throws on a non-map).
    Value& put(std::string_view key, Lit item);

    // The full read surface of ValueView (a Value owns what it reads).
    Type type() const noexcept;
    bool is_null() const noexcept;
    std::size_t len() const noexcept;
    std::optional<bool> as_bool() const noexcept;
    std::optional<std::int64_t> as_int() const noexcept;
    std::optional<double> as_float() const noexcept;
    std::optional<std::string_view> as_text() const noexcept;
    std::span<const std::uint8_t> as_bytes() const noexcept;
    std::span<const float> as_vector() const noexcept;
    ValueView get(std::string_view key) const noexcept;
    ValueView at(std::size_t index) const noexcept;
    std::vector<std::string> map_keys() const;

    /// True when this wrapper still owns a handle (false after a move-
    /// from). A default-constructed Value owns a null value, so this is
    /// about wrapper state, not about the null type.
    bool valid() const noexcept { return h_ != nullptr; }

private:
    friend struct detail::Access;
    explicit Value(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;  // opaque engine handle
};

// ---------------------------------------------------------------------------
// Predicates
// ---------------------------------------------------------------------------

/// The six comparison operators (numeric int/float interop, else
/// structural; text comparisons by bytes).
enum class Cmp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

/// An owned predicate tree — move-only. Handing one to `Query::filter()`
/// or `Collection::erase_where()` CONSUMES it (by rvalue); the tree is
/// freed by its destructor on every other path, per the ABI's rule.
class Predicate {
public:
    Predicate(Predicate&& other) noexcept;
    Predicate& operator=(Predicate&& other) noexcept;
    ~Predicate();
    Predicate(const Predicate&) = delete;
    Predicate& operator=(const Predicate&) = delete;

    bool valid() const noexcept { return h_ != nullptr; }

private:
    friend struct detail::Access;
    explicit Predicate(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

namespace pred {

/// field == value
Predicate eq(std::string_view path, Lit value);
/// field != value
Predicate ne(std::string_view path, Lit value);
Predicate lt(std::string_view path, Lit value);
Predicate le(std::string_view path, Lit value);
Predicate gt(std::string_view path, Lit value);
Predicate ge(std::string_view path, Lit value);
/// The raw form of the six above.
Predicate compare(std::string_view path, Cmp op, Lit value);
/// The field exists (and is non-null).
Predicate exists(std::string_view path);
/// field ∈ {v1, …, vn}
Predicate in(std::string_view path, std::initializer_list<Lit> values);
/// lo ≤ field ≤ hi
Predicate between(std::string_view path, Lit lo, Lit hi);
Predicate starts_with(std::string_view path, std::string_view prefix);
Predicate contains(std::string_view path, std::string_view needle);
/// The field's [lat, lon] point lies within `km` of (lat, lon).
Predicate geo_within(std::string_view path, double lat, double lon, double km);
/// a AND b (both consumed).
Predicate all(Predicate a, Predicate b);
/// a OR b (both consumed).
Predicate any(Predicate a, Predicate b);
/// NOT a (consumed).
Predicate none(Predicate a);

}  // namespace pred

// ---------------------------------------------------------------------------
// Query building
// ---------------------------------------------------------------------------

enum class Metric : std::uint8_t { Cosine, Dot, L2 };
enum class Quant : std::uint8_t { None, Binary, Scalar };

/// One result row: the key and document are BORROWED from the cursor —
/// valid until the next `Rows::next()` / the end of the range-for, or
/// `Value::clone()` the document to keep it.
struct Row {
    std::string_view key;
    ValueView doc;
    float score = 0.0f;
};

/// The materialized result cursor. Input-iterable: `for (const Row& r :
/// rows)`, or pull with `next()` returning std::nullopt at exhaustion.
class Rows {
public:
    Rows(Rows&& other) noexcept;
    Rows& operator=(Rows&& other) noexcept;
    ~Rows();
    Rows(const Rows&) = delete;
    Rows& operator=(const Rows&) = delete;

    std::optional<Row> next();

    class iterator {
    public:
        using value_type = Row;
        using reference = const Row&;
        using difference_type = std::ptrdiff_t;

        const Row& operator*() const noexcept { return *row_; }
        iterator& operator++() {
            row_ = rows_->next();
            return *this;
        }
        bool operator==(const iterator& other) const noexcept {
            return present() == other.present();
        }

    private:
        friend class Rows;
        explicit iterator(Rows* rows, std::optional<Row> row)
            : rows_(rows), row_(std::move(row)) {}
        bool present() const noexcept { return row_.has_value(); }
        Rows* rows_;
        std::optional<Row> row_;
    };

    iterator begin() { return iterator(this, next()); }
    iterator end() { return iterator(this, std::nullopt); }

private:
    friend struct detail::Access;
    friend class Collection;
    friend class Query;
    explicit Rows(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

/// One grouped aggregate row: key borrowed until the next pull.
struct GroupRow {
    std::string_view key;
    double value;
};

/// The grouped-aggregate cursor (group count/sum/avg terminals).
class GroupIter {
public:
    GroupIter(GroupIter&& other) noexcept;
    GroupIter& operator=(GroupIter&& other) noexcept;
    ~GroupIter();
    GroupIter(const GroupIter&) = delete;
    GroupIter& operator=(const GroupIter&) = delete;

    std::optional<GroupRow> next();

    class iterator {
    public:
        using value_type = GroupRow;
        using reference = const GroupRow&;
        using difference_type = std::ptrdiff_t;

        const GroupRow& operator*() const noexcept { return *row_; }
        iterator& operator++() {
            row_ = it_->next();
            return *this;
        }
        bool operator==(const iterator& other) const noexcept {
            return present() == other.present();
        }

    private:
        friend class GroupIter;
        explicit iterator(GroupIter* it, std::optional<GroupRow> row)
            : it_(it), row_(std::move(row)) {}
        bool present() const noexcept { return row_.has_value(); }
        GroupIter* it_;
        std::optional<GroupRow> row_;
    };

    iterator begin() { return iterator(this, next()); }
    iterator end() { return iterator(this, std::nullopt); }

private:
    friend struct detail::Access;
    friend class Query;
    explicit GroupIter(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

/// A string cursor (collections list, neighbors, traverse, map keys).
class Strs {
public:
    Strs(Strs&& other) noexcept;
    Strs& operator=(Strs&& other) noexcept;
    ~Strs();
    Strs(const Strs&) = delete;
    Strs& operator=(const Strs&) = delete;

    std::optional<std::string_view> next();
    /// Materialize every remaining item (owned copies).
    std::vector<std::string> to_vector();

    class iterator {
    public:
        using value_type = std::string_view;
        using reference = std::string_view;
        using difference_type = std::ptrdiff_t;

        std::string_view operator*() const noexcept { return *item_; }
        iterator& operator++() {
            item_ = strs_->next();
            return *this;
        }
        bool operator==(const iterator& other) const noexcept {
            return present() == other.present();
        }

    private:
        friend class Strs;
        explicit iterator(Strs* strs, std::optional<std::string_view> item)
            : strs_(strs), item_(std::move(item)) {}
        bool present() const noexcept { return item_.has_value(); }
        Strs* strs_;
        std::optional<std::string_view> item_;
    };

    iterator begin() { return iterator(this, next()); }
    iterator end() { return iterator(this, std::nullopt); }

private:
    friend struct detail::Access;
    friend class Collection;
    friend class Db;
    explicit Strs(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

/// One geo/weighted-graph hit: the key and (for geo queries) the document
/// are borrowed until the next pull; weighted-neighbor hits carry no
/// document (their `doc` is an invalid view).
struct GeoHit {
    std::string_view key;
    double distance_km;
    ValueView doc;
};

class GeoHits {
public:
    GeoHits(GeoHits&& other) noexcept;
    GeoHits& operator=(GeoHits&& other) noexcept;
    ~GeoHits();
    GeoHits(const GeoHits&) = delete;
    GeoHits& operator=(const GeoHits&) = delete;

    std::optional<GeoHit> next();

    class iterator {
    public:
        using value_type = GeoHit;
        using reference = const GeoHit&;
        using difference_type = std::ptrdiff_t;

        const GeoHit& operator*() const noexcept { return *hit_; }
        iterator& operator++() {
            hit_ = hits_->next();
            return *this;
        }
        bool operator==(const iterator& other) const noexcept {
            return present() == other.present();
        }

    private:
        friend class GeoHits;
        explicit iterator(GeoHits* hits, std::optional<GeoHit> hit)
            : hits_(hits), hit_(std::move(hit)) {}
        bool present() const noexcept { return hit_.has_value(); }
        GeoHits* hits_;
        std::optional<GeoHit> hit_;
    };

    iterator begin() { return iterator(this, next()); }
    iterator end() { return iterator(this, std::nullopt); }

private:
    friend struct detail::Access;
    friend class Collection;
    explicit GeoHits(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

/// The fluent query builder, mirroring the engine's Rust builder:
///
///     for (const Row& r : docs.query()
///              .filter(pred::eq("kind", "doc"))
///              .vector("v", probe, 3, Metric::Cosine)
///              .fuse_rrf(60.0f)
///              .limit(3)
///              .run()) { … }
///
/// Every method throws on failure and returns *this. The terminal calls
/// — run(), count(), sum(), … — CONSUME the builder; a builder destroyed
/// without a terminal is simply freed (the ABI's abandoned-builder path).
class Query {
public:
    Query(Query&& other) noexcept;
    Query& operator=(Query&& other) noexcept;
    ~Query();
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    Query& filter(Predicate&& pred);  ///< consumes the predicate
    Query& vector(std::string_view field, std::span<const float> probe,
                  std::size_t k, Metric metric = Metric::Cosine);
    Query& text(std::string_view field, std::string_view query, std::size_t k);
    Query& approx();
    /// k defaults to the engine's canonical 60 (its RRF default).
    Query& fuse_rrf(float k = 60.0f);
    Query& rerank_mmr(float lambda);
    Query& limit(std::size_t n);
    Query& offset(std::size_t n);
    Query& order_by(std::string_view field, bool descending);
    Query& select(std::span<const std::string_view> fields);
    Query& select(std::initializer_list<std::string_view> fields);

    // ---- terminals (consume the builder) ----
    Rows run();
    std::size_t count();
    std::size_t count_distinct(std::string_view field);
    double sum(std::string_view field);
    std::optional<double> avg(std::string_view field);
    std::optional<Value> min(std::string_view field);
    std::optional<Value> max(std::string_view field);
    GroupIter group_count(std::string_view key_field);
    GroupIter group_sum(std::string_view key_field, std::string_view value_field);
    GroupIter group_avg(std::string_view key_field, std::string_view value_field);

private:
    friend struct detail::Access;
    friend class Collection;
    explicit Query(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

/// The accepted value type of a declared field (§ schema rules).
enum class FieldType : std::uint8_t {
    Any,
    Bool,
    Int,
    Float,
    Text,
    Bytes,
    Vector,
    Array,
    Map,
};

/// One field DECLARATION (input of `Collection::set_schema`); `name`
/// borrows — keep the storage alive for the call.
struct FieldDef {
    std::string_view name;
    FieldType type = FieldType::Any;
    bool required = false;
    bool unique = false;
};

/// One field read back via `Collection::schema()` — owned.
struct Field {
    std::string name;
    FieldType type = FieldType::Any;
    bool required = false;
    bool unique = false;
};

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

/// One page of `Collection::page()`: the rows plus the next cursor (no
/// cursor when the walk reached the end).
struct Page {
    Rows rows;
    std::optional<std::string> next;
};

/// One (key, document) pair for `Collection::put_many()`; the document is
/// borrowed for the call (the engine clones it).
struct Kv {
    std::string_view key;
    const Value* val;
};

/// A derived collection handle — move-only RAII. Freeing it (scope exit)
/// releases the database's derived-handle count, which is what lets
/// `Db::close()` succeed; declare collections so they die before their Db
/// (natural stack order does exactly that).
class Collection {
public:
    Collection(Collection&& other) noexcept;
    Collection& operator=(Collection&& other) noexcept;
    ~Collection();
    Collection(const Collection&) = delete;
    Collection& operator=(const Collection&) = delete;

    std::string_view name() const noexcept;

    // ---- mutations ----
    void insert(std::string_view key, const Value& doc);
    void insert_with_ttl(std::string_view key, const Value& doc, std::int64_t ttl);
    /// The generated key (owned string).
    std::string insert_auto(const Value& doc);
    void put_many(std::span<const Kv> items);
    void put_many(std::initializer_list<Kv> items);
    /// Read-modify-write: the callback receives the current document (or
    /// nullopt when the key is absent) and returns the replacement.
    /// Throwing from the callback aborts the update with the engine's
    /// argument error and rethrows after the call returns.
    void update(std::string_view key,
                std::function<Value(std::optional<ValueView>)> fn);
    void patch(std::string_view key, const Value& patch);
    /// expected/replacement may be nullptr (absence). Returns whether the
    /// swap was applied.
    bool compare_and_set(std::string_view key, const Value* expected,
                         const Value* replacement);
    /// Delete one key; returns whether it existed.
    bool erase(std::string_view key);
    /// Delete every match; returns the removed count. Consumes the predicate.
    std::size_t erase_where(Predicate&& pred);
    std::size_t erase_batch(std::span<const std::string_view> keys);

    // ---- TTL ----
    void set_ttl(std::string_view key, std::int64_t ttl);
    std::optional<std::int64_t> ttl(std::string_view key);
    std::size_t purge_expired(std::int64_t now);

    // ---- reads ----
    std::optional<Value> get(std::string_view key);
    std::size_t len();
    /// Full scan; return false from the callback to stop early.
    void scan(std::function<bool(std::string_view key, ValueView doc)> fn);
    /// Keyset pagination strictly after `after` (ABI §4.9). `nullopt` is
    /// the ONLY start form — it begins at the very first key, the legal
    /// empty key `""` included; a valued `after` of ANY length — including
    /// the empty string, the zero-length cursor a page boundary on `""`
    /// produces — continues strictly after it (feed `Page::next` back; a
    /// re-walk never restarts). Caveat: build the optional from
    /// `Page::next` or a `std::string`-backed view — a default-constructed
    /// `std::string_view{}` has `data() == nullptr`, which reads as START,
    /// not as the empty cursor.
    Page page(std::optional<std::string_view> after, std::size_t limit);
    /// DIRECT positional (phrase) text search — no query builder. The
    /// phrase must appear as a consecutive, in-order run of analyzed
    /// tokens; k == 0 yields empty. Scores are BM25 phrase sums.
    Rows phrase_search(std::string_view field, std::string_view phrase,
                       std::size_t k);

    // ---- queries ----
    Query query();

    // ---- graph ----
    void link(std::string_view from, std::string_view rel, std::string_view to);
    void link_weighted(std::string_view from, std::string_view rel,
                       std::string_view to, double weight);
    bool unlink(std::string_view from, std::string_view rel, std::string_view to);
    Strs neighbors(std::string_view key, std::string_view rel);
    Strs in_neighbors(std::string_view key, std::string_view rel);
    /// Weighted out-neighbors: distance_km carries the edge weight; no docs.
    GeoHits neighbors_weighted(std::string_view key, std::string_view rel);
    Strs traverse(std::string_view key, std::string_view rel, std::size_t depth);

    // ---- geo ----
    GeoHits geo_within_radius(std::string_view field, double lat, double lon,
                              double km);
    GeoHits geo_within_bbox(std::string_view field, double min_lat,
                            double min_lon, double max_lat, double max_lon);
    GeoHits geo_nearest(std::string_view field, double lat, double lon,
                        std::size_t k);

    // ---- indexes ----
    void create_scalar_index(std::string_view field);
    void create_compound_index(std::span<const std::string_view> fields);
    void create_compound_index(std::initializer_list<std::string_view> fields);
    void create_text_index(std::string_view field);
    void create_text_index_ondisk(std::string_view field);
    void create_geo_index(std::string_view field);
    void create_vector_index(std::string_view field, Metric metric);
    void create_vector_index_quantized(std::string_view field, Metric metric,
                                       Quant quant);
    void create_vector_index_ondisk(std::string_view field, Metric metric);
    void create_vector_index_ondisk_quantized(std::string_view field,
                                              Metric metric, Quant quant);
    void create_vector_index_pq(std::string_view field, Metric metric,
                                std::size_t subspaces, std::size_t bits);
    void create_vector_index_ondisk_pq(std::string_view field, Metric metric,
                                       std::size_t subspaces, std::size_t bits);

    // ---- schema ----
    void set_schema(std::span<const FieldDef> defs);
    void set_schema(std::initializer_list<FieldDef> defs);
    /// The declared schema, materialized (empty when none is declared).
    std::vector<Field> schema();

private:
    friend struct detail::Access;
    friend class Db;
    explicit Collection(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

// ---------------------------------------------------------------------------
// The database
// ---------------------------------------------------------------------------

/// An owned database handle — move-only RAII. `close()` is explicit when
/// you want failures thrown; the destructor closes best-effort and never
/// throws. A file database at `path` is created or opened; the parent
/// directory must exist.
class Db {
public:
    static Db open(std::string_view path);
    static Db open_memory();

    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    /// Explicit close (throws on failure, e.g. Busy while collection
    /// handles are still alive); idempotent.
    void close();

    Collection collection(std::string_view name);
    Strs collections();
    /// True when any data moved out.
    bool compact();
    void backup(std::string_view path);
    void dump_to(std::string_view path);
    void load_from(std::string_view path);
    void load_from_with_renames(std::string_view path,
                                std::initializer_list<std::pair<std::string_view,
                                                                std::string_view>>
                                    renames);

private:
    friend struct detail::Access;
    explicit Db(void* h) noexcept : h_(h) {}
    void* h_ = nullptr;
};

/// The shared-library ABI revision this library is built against (must be
/// 1 at engine v0.3.0; checked at load time by the golden suite).
std::uint32_t ffi_version() noexcept;

}  // namespace corvid

#endif  // CORVID_CPP_CORVID_HPP
