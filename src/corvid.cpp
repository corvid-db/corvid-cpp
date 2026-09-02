// corvid.cpp — the single translation unit where corvid-cpp meets the
// engine's C ABI. Everything ABI-shaped lives here; the public header
// (include/corvid/corvid.hpp) never names an ABI type or function
// (scripts/idiom-gate.sh enforces that in CI).
//
// Conventions:
//   - a returning-status call that answers CORVID_ERR throws
//     corvid::Error built from the thread's last recorded error;
//   - a handle-returning call that answers NULL throws the same way
//     (NULL + recorded error is the ABI's failure shape), EXCEPT where
//     the ABI defines NULL as "absent, not an error" (map/array child
//     lookups, min/max aggregation absence);
//   - every wrapper frees its handle exactly once, on the path that
//     created it: the destructor (the moved-from wrapper is empty), the
//     consuming engine call (query/predicate consumption), or an
//     explicit release into a C out-parameter.

#include "corvid/corvid.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

// The published corvid.h is a C header by contract, and its enum idiom
// needs help under C++: for pre-C23 compilers it emits BOTH
// `enum corvid_status` and `typedef uint32_t corvid_status` — two types
// under one name, which C++ rejects (in C they live in different
// namespaces). Presenting C23 to the preprocessor selects the header's
// fixed-underlying-type branch (`enum X : uint32_t` + a same-name
// typedef), which is plain valid C++ — so the published artifact
// compiles VERBATIM, unpatched (docs/PLAN.md: artifacts are findings,
// never edits). The C-standard wrappers above are pre-included so the
// engine header's own includes resolve to already-seen no-ops inside
// the extern "C" block. Scoped to this one include, restored after.
#if defined(__cplusplus) && !defined(__STDC_VERSION__)
#define CORVIDPP_PRESENTED_STDC 1
#define __STDC_VERSION__ 202311L
#endif

extern "C" {
#include "corvid.h"
}

#ifdef CORVIDPP_PRESENTED_STDC
#undef __STDC_VERSION__
#undef CORVIDPP_PRESENTED_STDC
#endif

namespace corvid {
namespace {

// ---- error plumbing -------------------------------------------------------

[[noreturn]] void throw_last_error() {
    ErrorCode code = static_cast<ErrorCode>(corvid_last_error_code());
    throw Error(code);
}

void check(corvid_status st) {
    if (st != CORVID_OK) throw_last_error();
}

// NULL out of a handle-returning ABI call is its failure shape.
void check_not_null(void* h) {
    if (h == nullptr) throw_last_error();
}

// ---- enum translations ------------------------------------------------------

corvid_cmp to_abi(Cmp op) {
    switch (op) {
        case Cmp::Eq: return CORVID_CMP_EQ;
        case Cmp::Ne: return CORVID_CMP_NE;
        case Cmp::Lt: return CORVID_CMP_LT;
        case Cmp::Le: return CORVID_CMP_LE;
        case Cmp::Gt: return CORVID_CMP_GT;
        case Cmp::Ge: return CORVID_CMP_GE;
    }
    throw Error(ErrorCode::InvalidArgument);
}

corvid_metric to_abi(Metric m) {
    switch (m) {
        case Metric::Cosine: return CORVID_METRIC_COSINE;
        case Metric::Dot: return CORVID_METRIC_DOT;
        case Metric::L2: return CORVID_METRIC_L2;
    }
    throw Error(ErrorCode::InvalidArgument);
}

corvid_quant to_abi(Quant q) {
    switch (q) {
        case Quant::None: return CORVID_QUANT_NONE;
        case Quant::Binary: return CORVID_QUANT_BINARY;
        case Quant::Scalar: return CORVID_QUANT_SCALAR;
    }
    throw Error(ErrorCode::InvalidArgument);
}

corvid_field_type to_abi(FieldType t) {
    switch (t) {
        case FieldType::Any: return CORVID_FIELD_ANY;
        case FieldType::Bool: return CORVID_FIELD_BOOL;
        case FieldType::Int: return CORVID_FIELD_INT;
        case FieldType::Float: return CORVID_FIELD_FLOAT;
        case FieldType::Text: return CORVID_FIELD_TEXT;
        case FieldType::Bytes: return CORVID_FIELD_BYTES;
        case FieldType::Vector: return CORVID_FIELD_VECTOR;
        case FieldType::Array: return CORVID_FIELD_ARRAY;
        case FieldType::Map: return CORVID_FIELD_MAP;
    }
    throw Error(ErrorCode::InvalidArgument);
}

Type from_abi(corvid_value_type_t t) {
    switch (t) {
        case CORVID_TYPE_NULL: return Type::Null;
        case CORVID_TYPE_BOOL: return Type::Bool;
        case CORVID_TYPE_INT: return Type::Int;
        case CORVID_TYPE_FLOAT: return Type::Float;
        case CORVID_TYPE_TEXT: return Type::Text;
        case CORVID_TYPE_BYTES: return Type::Bytes;
        case CORVID_TYPE_ARRAY: return Type::Array;
        case CORVID_TYPE_MAP: return Type::Map;
        case CORVID_TYPE_VECTOR: return Type::Vector;
    }
    throw Error(ErrorCode::Decode);
}

FieldType field_type_from_abi(corvid_field_type t) {
    switch (t) {
        case CORVID_FIELD_BOOL: return FieldType::Bool;
        case CORVID_FIELD_INT: return FieldType::Int;
        case CORVID_FIELD_FLOAT: return FieldType::Float;
        case CORVID_FIELD_TEXT: return FieldType::Text;
        case CORVID_FIELD_BYTES: return FieldType::Bytes;
        case CORVID_FIELD_VECTOR: return FieldType::Vector;
        case CORVID_FIELD_ARRAY: return FieldType::Array;
        case CORVID_FIELD_MAP: return FieldType::Map;
        case CORVID_FIELD_ANY: return FieldType::Any;
    }
    throw Error(ErrorCode::Decode);
}

corvid_value* borrow_value(const Value& v);  // defined after Access, below

}  // namespace

// ---- detail::Access: the private handle bridge -----------------------------

struct detail::Access {
    static void* steal(Value& v) noexcept { return std::exchange(v.h_, nullptr); }
    static void* handle(const Value& v) noexcept { return v.h_; }
    static Value own_value(void* h) noexcept { return Value(h); }
    static const void* handle(const ValueView& v) noexcept { return v.h_; }
    static ValueView view(const void* h) noexcept {
        ValueView vv;
        vv.h_ = h;
        return vv;
    }
    static void* steal(Predicate& p) noexcept { return std::exchange(p.h_, nullptr); }
    static Predicate own_pred(void* h) noexcept { return Predicate(h); }
    static void* steal(Query& q) noexcept { return std::exchange(q.h_, nullptr); }
    static void* handle(Query& q) noexcept { return q.h_; }
    static void* steal(Rows& r) noexcept { return std::exchange(r.h_, nullptr); }
    static void* handle(const Rows& r) noexcept { return r.h_; }
    static void* steal(Strs& s) noexcept { return std::exchange(s.h_, nullptr); }
    static void* steal(GeoHits& g) noexcept { return std::exchange(g.h_, nullptr); }
    static void* steal(GroupIter& g) noexcept { return std::exchange(g.h_, nullptr); }
    static void* steal(Collection& c) noexcept { return std::exchange(c.h_, nullptr); }
    static void* handle(const Collection& c) noexcept { return c.h_; }
    static void* steal(Db& d) noexcept { return std::exchange(d.h_, nullptr); }

    // Literal materialization: Lit's payload is private to Value (and to
    // this bridge); the conversion lives here so it is shareable by the
    // predicate builders below.
    static corvid_value* materialize(const Lit& l) {
        corvid_value* v = nullptr;
        switch (l.kind_) {
            case Lit::Kind::Null: v = corvid_value_null(); break;
            case Lit::Kind::Bool: v = corvid_value_bool(l.bool_); break;
            case Lit::Kind::Int: v = corvid_value_int(l.int_); break;
            case Lit::Kind::Float: v = corvid_value_float(l.float_); break;
            case Lit::Kind::Text:
                // Same null-data() normalization as Value's string_view
                // ctor: a default-constructed lit::Text is the legal
                // empty text, not the ABI's null-pointer failure shape.
                v = corvid_value_text(l.text_.data() != nullptr ? l.text_.data() : "",
                                      l.text_.size());
                break;
            case Lit::Kind::Bytes:
                v = corvid_value_bytes(l.bytes_.data(), l.bytes_.size());
                break;
            case Lit::Kind::Vector:
                v = corvid_value_vector(l.vec_.data(), l.vec_.size());
                break;
            case Lit::Kind::Value:
                // A borrowed owned Value: cloned here (the sanctioned
                // deep copy), never consumed.
                v = corvid_value_clone(
                    static_cast<const corvid_value*>(handle(*l.value_)));
                break;
        }
        check_not_null(v);
        return v;
    }
};

namespace {

corvid_value* borrow_value(const Value& v) {
    return static_cast<corvid_value*>(detail::Access::handle(v));
}

// RAII sweep for the raw corvid_value handles the pred builders
// materialize ahead of their engine call: a LATER literal can fail to
// materialize (invalid UTF-8 Text throws mid-list), and the ones
// already live must not leak out of the builder. The ABI CLONES the
// values into the predicate tree (a failed build leaves them ours
// too), so the sweep frees unconditionally — there is no release path.
class ValueSweep {
public:
    ValueSweep() = default;
    ValueSweep(const ValueSweep&) = delete;
    ValueSweep& operator=(const ValueSweep&) = delete;
    ~ValueSweep() {
        for (corvid_value* v : held_) corvid_value_free(v);
    }
    void reserve(std::size_t n) { held_.reserve(n); }
    corvid_value* push(corvid_value* v) {
        held_.push_back(v);
        return v;
    }
    std::size_t size() const noexcept { return held_.size(); }
    const corvid_value* const* data() const noexcept { return held_.data(); }

private:
    std::vector<corvid_value*> held_;
};

}  // namespace

// ---- Error -------------------------------------------------------------------

Error::Error(ErrorCode code) : code_(code) {
    size_t len = 0;
    const char* msg = corvid_last_error_message(&len);
    message_.assign(msg ? msg : "", msg ? len : 0);
    what_ = "corvid: [" + std::to_string(static_cast<unsigned>(code)) + "] " + message_;
}

// ---- Value -------------------------------------------------------------------

Value::Value() noexcept : h_(nullptr) {}

Value::Value(std::nullptr_t) noexcept : h_(nullptr) {}

Value::Value(bool b) {
    h_ = corvid_value_bool(b);
    check_not_null(h_);
}

Value::Value(std::int64_t i) {
    h_ = corvid_value_int(i);
    check_not_null(h_);
}

Value::Value(int i) : Value(static_cast<std::int64_t>(i)) {}

Value::Value(double d) {
    h_ = corvid_value_float(d);
    check_not_null(h_);
}

Value::Value(std::string_view s) {
    // A default-constructed string_view carries a null data() (size 0);
    // the ABI reads a null pointer as its failure shape at ANY length,
    // so normalize to the legal empty text — the same treatment the
    // const char* ctor below gives nullptr.
    h_ = corvid_value_text(s.data() != nullptr ? s.data() : "", s.size());
    check_not_null(h_);
}

Value::Value(const char* s) : Value(std::string_view(s ? s : "")) {}

Value::Value(lit::Text t) : Value(t.s) {}

Value::Value(lit::Bytes b) {
    h_ = corvid_value_bytes(b.b.data(), b.b.size());
    check_not_null(h_);
}

Value::Value(lit::Vec v) {
    h_ = corvid_value_vector(v.v.data(), v.v.size());
    check_not_null(h_);
}

Value::Value(Lit l) {
    h_ = detail::Access::materialize(l);
}

Value Value::text(std::string_view s) { return Value(s); }

Value Value::bytes(std::span<const std::uint8_t> b) { return Value(lit::Bytes{b}); }

Value Value::vec(std::span<const float> v) { return Value(lit::Vec{v}); }

Value Value::empty_array() {
    Value v;
    v.h_ = corvid_value_array_new();
    check_not_null(v.h_);
    return v;
}

Value Value::empty_map() {
    Value v;
    v.h_ = corvid_value_map_new();
    check_not_null(v.h_);
    return v;
}

Value Value::array(std::initializer_list<Lit> items) {
    Value v = empty_array();
    for (const Lit& l : items) v.push(l);
    return v;
}

Value Value::map(std::initializer_list<std::pair<std::string_view, Lit>> entries) {
    Value v = empty_map();
    for (const auto& [key, l] : entries) v.put(key, l);
    return v;
}

Value::~Value() {
    if (h_ != nullptr) corvid_value_free(static_cast<corvid_value*>(h_));
}

Value::Value(Value&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_value_free(static_cast<corvid_value*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Value Value::clone() const {
    corvid_value* c = corvid_value_clone(static_cast<const corvid_value*>(h_));
    check_not_null(c);
    return detail::Access::own_value(c);
}

Value& Value::push(Lit item) {
    corvid_value* v = detail::Access::materialize(item);
    // Consumption is UNCONDITIONAL (the ABI's §8: a failed push has still
    // dropped the item) — never free v after the call, on any status.
    check(corvid_value_array_push(static_cast<corvid_value*>(h_), v));
    return *this;
}

Value& Value::put(std::string_view key, Lit item) {
    corvid_value* v = detail::Access::materialize(item);
    // Same unconditional-consumption rule as push().
    check(corvid_value_map_put(static_cast<corvid_value*>(h_), key.data(),
                               key.size(), v));
    return *this;
}

Type Value::type() const noexcept { return ValueView(detail::Access::view(h_)).type(); }
bool Value::is_null() const noexcept { return type() == Type::Null; }
std::size_t Value::len() const noexcept { return corvid_value_len(static_cast<const corvid_value*>(h_)); }

std::optional<bool> Value::as_bool() const noexcept { return ValueView(detail::Access::view(h_)).as_bool(); }
std::optional<std::int64_t> Value::as_int() const noexcept { return ValueView(detail::Access::view(h_)).as_int(); }
std::optional<double> Value::as_float() const noexcept { return ValueView(detail::Access::view(h_)).as_float(); }
std::optional<std::string_view> Value::as_text() const noexcept { return ValueView(detail::Access::view(h_)).as_text(); }
std::span<const std::uint8_t> Value::as_bytes() const noexcept { return ValueView(detail::Access::view(h_)).as_bytes(); }
std::span<const float> Value::as_vector() const noexcept { return ValueView(detail::Access::view(h_)).as_vector(); }

ValueView Value::get(std::string_view key) const noexcept {
    return ValueView(detail::Access::view(h_)).get(key);
}

ValueView Value::at(std::size_t index) const noexcept {
    return ValueView(detail::Access::view(h_)).at(index);
}

std::vector<std::string> Value::map_keys() const {
    return ValueView(detail::Access::view(h_)).map_keys();
}

// ---- ValueView -----------------------------------------------------------------

Type ValueView::type() const noexcept {
    if (h_ == nullptr) return Type::Null;
    return from_abi(corvid_value_type(static_cast<const corvid_value*>(h_)));
}

bool ValueView::is_null() const noexcept { return type() == Type::Null; }

std::size_t ValueView::len() const noexcept {
    if (h_ == nullptr) return 0;
    return corvid_value_len(static_cast<const corvid_value*>(h_));
}

std::optional<bool> ValueView::as_bool() const noexcept {
    if (h_ == nullptr) return std::nullopt;
    int ok = 0;
    int v = corvid_value_as_bool(static_cast<const corvid_value*>(h_), &ok);
    if (!ok) return std::nullopt;
    return v != 0;
}

std::optional<std::int64_t> ValueView::as_int() const noexcept {
    if (h_ == nullptr) return std::nullopt;
    int ok = 0;
    std::int64_t v = corvid_value_as_int(static_cast<const corvid_value*>(h_), &ok);
    if (!ok) return std::nullopt;
    return v;
}

std::optional<double> ValueView::as_float() const noexcept {
    if (h_ == nullptr) return std::nullopt;
    int ok = 0;
    double v = corvid_value_as_float(static_cast<const corvid_value*>(h_), &ok);
    if (!ok) return std::nullopt;
    return v;
}

std::optional<std::string_view> ValueView::as_text() const noexcept {
    if (h_ == nullptr) return std::nullopt;
    size_t len = 0;
    const char* p = corvid_value_text_ref(static_cast<const corvid_value*>(h_), &len);
    if (p == nullptr) return std::nullopt;
    return std::string_view(p, len);
}

std::span<const std::uint8_t> ValueView::as_bytes() const noexcept {
    if (h_ == nullptr) return {};
    size_t len = 0;
    const std::uint8_t* p =
        corvid_value_bytes_ref(static_cast<const corvid_value*>(h_), &len);
    if (p == nullptr) return {};
    return std::span<const std::uint8_t>(p, len);
}

std::span<const float> ValueView::as_vector() const noexcept {
    if (h_ == nullptr) return {};
    size_t dim = 0;
    const float* p =
        corvid_value_vector_ref(static_cast<const corvid_value*>(h_), &dim);
    if (p == nullptr) return {};
    return std::span<const float>(p, dim);
}

ValueView ValueView::get(std::string_view key) const noexcept {
    if (h_ == nullptr) return ValueView();
    return detail::Access::view(corvid_value_map_get(
        static_cast<const corvid_value*>(h_), key.data(), key.size()));
}

ValueView ValueView::at(std::size_t index) const noexcept {
    if (h_ == nullptr) return ValueView();
    return detail::Access::view(
        corvid_value_array_get(static_cast<const corvid_value*>(h_), index));
}

std::vector<std::string> ValueView::map_keys() const {
    if (h_ == nullptr) return {};
    corvid_strs* s = corvid_value_map_keys(static_cast<const corvid_value*>(h_));
    check_not_null(s);
    std::vector<std::string> keys;
    for (;;) {
        const char* item = nullptr;
        size_t len = 0;
        if (corvid_strs_next(s, &item, &len) != 1) break;
        keys.emplace_back(item, len);
    }
    corvid_strs_free(s);
    return keys;
}

// ---- Predicate ------------------------------------------------------------------

namespace {

corvid_pred* build_compare(std::string_view path, Cmp op, const Lit& value) {
    corvid_value* v = detail::Access::materialize(value);
    corvid_pred* p = corvid_pred_compare(path.data(), path.size(), to_abi(op), v);
    corvid_value_free(v);  // CLONED into the tree
    check_not_null(p);
    return p;
}

}  // namespace

Predicate::Predicate(Predicate&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Predicate& Predicate::operator=(Predicate&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_pred_free(static_cast<corvid_pred*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Predicate::~Predicate() {
    if (h_ != nullptr) corvid_pred_free(static_cast<corvid_pred*>(h_));
}

namespace pred {

Predicate eq(std::string_view path, Lit value) { return compare(path, Cmp::Eq, std::move(value)); }
Predicate ne(std::string_view path, Lit value) { return compare(path, Cmp::Ne, std::move(value)); }
Predicate lt(std::string_view path, Lit value) { return compare(path, Cmp::Lt, std::move(value)); }
Predicate le(std::string_view path, Lit value) { return compare(path, Cmp::Le, std::move(value)); }
Predicate gt(std::string_view path, Lit value) { return compare(path, Cmp::Gt, std::move(value)); }
Predicate ge(std::string_view path, Lit value) { return compare(path, Cmp::Ge, std::move(value)); }

Predicate compare(std::string_view path, Cmp op, Lit value) {
    return detail::Access::own_pred(build_compare(path, op, value));
}

Predicate exists(std::string_view path) {
    corvid_pred* p = corvid_pred_exists(path.data(), path.size());
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate in(std::string_view path, std::initializer_list<Lit> values) {
    // The whole list is materialized before the engine call, and a later
    // Lit can throw there (invalid UTF-8 Text) — ValueSweep keeps every
    // already-materialized handle freed on every path, including the
    // throw (the ABI clones the values into the tree; a failed call
    // leaves them ours).
    ValueSweep owned;
    owned.reserve(values.size());
    for (const Lit& l : values) owned.push(detail::Access::materialize(l));
    corvid_pred* p =
        corvid_pred_in(path.data(), path.size(), owned.data(), owned.size());
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate between(std::string_view path, Lit lo, Lit hi) {
    // `hi` can fail to materialize (invalid UTF-8 Text) after `lo` is
    // already live — the sweep frees both on every path, including the
    // throw (the ABI clones the bounds; a failed call leaves them ours).
    ValueSweep bounds;
    corvid_value* a = bounds.push(detail::Access::materialize(lo));
    corvid_value* b = bounds.push(detail::Access::materialize(hi));
    corvid_pred* p = corvid_pred_between(path.data(), path.size(), a, b);
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate starts_with(std::string_view path, std::string_view prefix) {
    corvid_pred* p =
        corvid_pred_starts_with(path.data(), path.size(), prefix.data(), prefix.size());
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate contains(std::string_view path, std::string_view needle) {
    corvid_pred* p =
        corvid_pred_contains(path.data(), path.size(), needle.data(), needle.size());
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate geo_within(std::string_view path, double lat, double lon, double km) {
    corvid_pred* p = corvid_pred_geo_within(path.data(), path.size(), lat, lon, km);
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate all(Predicate a, Predicate b) {
    corvid_pred* p = corvid_pred_and(static_cast<corvid_pred*>(detail::Access::steal(a)),
                                     static_cast<corvid_pred*>(detail::Access::steal(b)));
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate any(Predicate a, Predicate b) {
    corvid_pred* p = corvid_pred_or(static_cast<corvid_pred*>(detail::Access::steal(a)),
                                    static_cast<corvid_pred*>(detail::Access::steal(b)));
    check_not_null(p);
    return detail::Access::own_pred(p);
}

Predicate none(Predicate a) {
    corvid_pred* p = corvid_pred_not(static_cast<corvid_pred*>(detail::Access::steal(a)));
    check_not_null(p);
    return detail::Access::own_pred(p);
}

}  // namespace pred

// ---- Rows ----------------------------------------------------------------------

Rows::Rows(Rows&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Rows& Rows::operator=(Rows&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_rows_free(static_cast<corvid_rows*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Rows::~Rows() {
    if (h_ != nullptr) corvid_rows_free(static_cast<corvid_rows*>(h_));
}

std::optional<Row> Rows::next() {
    if (h_ == nullptr) return std::nullopt;
    const std::uint8_t* key = nullptr;
    size_t key_len = 0;
    const corvid_value* doc = nullptr;
    float score = 0.0f;
    if (corvid_rows_next(static_cast<corvid_rows*>(h_), &key, &key_len, &doc,
                         &score) != 1)
        return std::nullopt;
    return Row{std::string_view(reinterpret_cast<const char*>(key), key_len),
               detail::Access::view(doc), score};
}

// ---- GroupIter -------------------------------------------------------------------

GroupIter::GroupIter(GroupIter&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

GroupIter& GroupIter::operator=(GroupIter&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_groupiter_free(static_cast<corvid_groupiter*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

GroupIter::~GroupIter() {
    if (h_ != nullptr) corvid_groupiter_free(static_cast<corvid_groupiter*>(h_));
}

std::optional<GroupRow> GroupIter::next() {
    if (h_ == nullptr) return std::nullopt;
    const char* key = nullptr;
    size_t key_len = 0;
    double value = 0;
    if (corvid_groupiter_next(static_cast<corvid_groupiter*>(h_), &key, &key_len,
                              &value) != 1)
        return std::nullopt;
    return GroupRow{std::string_view(key, key_len), value};
}

// ---- Strs --------------------------------------------------------------------------

Strs::Strs(Strs&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Strs& Strs::operator=(Strs&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_strs_free(static_cast<corvid_strs*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Strs::~Strs() {
    if (h_ != nullptr) corvid_strs_free(static_cast<corvid_strs*>(h_));
}

std::optional<std::string_view> Strs::next() {
    if (h_ == nullptr) return std::nullopt;
    const char* item = nullptr;
    size_t len = 0;
    if (corvid_strs_next(static_cast<corvid_strs*>(h_), &item, &len) != 1)
        return std::nullopt;
    return std::string_view(item, len);
}

std::vector<std::string> Strs::to_vector() {
    std::vector<std::string> out;
    for (;;) {
        auto item = next();
        if (!item) break;
        out.emplace_back(*item);
    }
    return out;
}

// ---- GeoHits ------------------------------------------------------------------------

GeoHits::GeoHits(GeoHits&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

GeoHits& GeoHits::operator=(GeoHits&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_geohits_free(static_cast<corvid_geohits*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

GeoHits::~GeoHits() {
    if (h_ != nullptr) corvid_geohits_free(static_cast<corvid_geohits*>(h_));
}

std::optional<GeoHit> GeoHits::next() {
    if (h_ == nullptr) return std::nullopt;
    corvid_geohit hit{nullptr, 0, 0};
    const corvid_value* doc = nullptr;
    if (corvid_geohits_next(static_cast<corvid_geohits*>(h_), &hit, &doc) != 1)
        return std::nullopt;
    return GeoHit{std::string_view(reinterpret_cast<const char*>(hit.key), hit.key_len),
                  hit.distance_km, detail::Access::view(doc)};
}

// ---- Query ----------------------------------------------------------------------------

Query::Query(Query&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Query& Query::operator=(Query&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_query_free(static_cast<corvid_query*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Query::~Query() {
    if (h_ != nullptr) corvid_query_free(static_cast<corvid_query*>(h_));
}

Query& Query::filter(Predicate&& p) {
    check(corvid_query_filter(static_cast<corvid_query*>(h_),
                              static_cast<corvid_pred*>(detail::Access::steal(p))));
    return *this;
}

Query& Query::vector(std::string_view field, std::span<const float> probe,
                     std::size_t k, Metric metric) {
    check(corvid_query_vector(static_cast<corvid_query*>(h_), field.data(),
                              field.size(), probe.data(), probe.size(), k,
                              to_abi(metric)));
    return *this;
}

Query& Query::text(std::string_view field, std::string_view query, std::size_t k) {
    check(corvid_query_text(static_cast<corvid_query*>(h_), field.data(),
                            field.size(), query.data(), query.size(), k));
    return *this;
}

Query& Query::approx() {
    check(corvid_query_approx(static_cast<corvid_query*>(h_)));
    return *this;
}

Query& Query::fuse_rrf(float k /* = 60.0f, the engine's RRF default */) {
    check(corvid_query_fuse_rrf(static_cast<corvid_query*>(h_), k));
    return *this;
}

Query& Query::rerank_mmr(float lambda) {
    check(corvid_query_rerank_mmr(static_cast<corvid_query*>(h_), lambda));
    return *this;
}

Query& Query::limit(std::size_t n) {
    check(corvid_query_limit(static_cast<corvid_query*>(h_), n));
    return *this;
}

Query& Query::offset(std::size_t n) {
    check(corvid_query_offset(static_cast<corvid_query*>(h_), n));
    return *this;
}

Query& Query::order_by(std::string_view field, bool descending) {
    check(corvid_query_order_by(static_cast<corvid_query*>(h_), field.data(),
                                field.size(), descending ? 1 : 0));
    return *this;
}

Query& Query::select(std::span<const std::string_view> fields) {
    std::vector<const char*> names;
    std::vector<size_t> lens;
    names.reserve(fields.size());
    lens.reserve(fields.size());
    for (std::string_view f : fields) {
        names.push_back(f.data());
        lens.push_back(f.size());
    }
    check(corvid_query_select(static_cast<corvid_query*>(h_), names.data(),
                              lens.data(), names.size()));
    return *this;
}

Query& Query::select(std::initializer_list<std::string_view> fields) {
    return select(std::span<const std::string_view>(fields.begin(), fields.end()));
}

Rows Query::run() {
    corvid_rows* rows = corvid_query_run(static_cast<corvid_query*>(h_));
    h_ = nullptr;  // consumed either way (the ABI's §8 contract)
    check_not_null(rows);
    return Rows(rows);
}

std::size_t Query::count() {
    size_t n = 0;
    corvid_status st = corvid_query_count(static_cast<corvid_query*>(h_), &n);
    h_ = nullptr;
    check(st);
    return n;
}

std::size_t Query::count_distinct(std::string_view field) {
    size_t n = 0;
    corvid_status st = corvid_query_count_distinct(static_cast<corvid_query*>(h_),
                                                   field.data(), field.size(), &n);
    h_ = nullptr;
    check(st);
    return n;
}

double Query::sum(std::string_view field) {
    double out = 0;
    corvid_status st =
        corvid_query_sum(static_cast<corvid_query*>(h_), field.data(), field.size(), &out);
    h_ = nullptr;
    check(st);
    return out;
}

std::optional<double> Query::avg(std::string_view field) {
    double out = 0;
    int has = 0;
    corvid_status st = corvid_query_avg(static_cast<corvid_query*>(h_),
                                        field.data(), field.size(), &out, &has);
    h_ = nullptr;
    check(st);
    if (!has) return std::nullopt;
    return out;
}

std::optional<Value> Query::min(std::string_view field) {
    corvid_value* out = nullptr;
    corvid_status st =
        corvid_query_min(static_cast<corvid_query*>(h_), field.data(), field.size(), &out);
    h_ = nullptr;
    check(st);
    if (out == nullptr) return std::nullopt;
    return detail::Access::own_value(out);
}

std::optional<Value> Query::max(std::string_view field) {
    corvid_value* out = nullptr;
    corvid_status st =
        corvid_query_max(static_cast<corvid_query*>(h_), field.data(), field.size(), &out);
    h_ = nullptr;
    check(st);
    if (out == nullptr) return std::nullopt;
    return detail::Access::own_value(out);
}

GroupIter Query::group_count(std::string_view key_field) {
    corvid_groupiter* it = corvid_query_group_count(
        static_cast<corvid_query*>(h_), key_field.data(), key_field.size());
    h_ = nullptr;
    check_not_null(it);
    return GroupIter(it);
}

GroupIter Query::group_sum(std::string_view key_field, std::string_view value_field) {
    corvid_groupiter* it =
        corvid_query_group_sum(static_cast<corvid_query*>(h_), key_field.data(),
                               key_field.size(), value_field.data(), value_field.size());
    h_ = nullptr;
    check_not_null(it);
    return GroupIter(it);
}

GroupIter Query::group_avg(std::string_view key_field, std::string_view value_field) {
    corvid_groupiter* it =
        corvid_query_group_avg(static_cast<corvid_query*>(h_), key_field.data(),
                               key_field.size(), value_field.data(), value_field.size());
    h_ = nullptr;
    check_not_null(it);
    return GroupIter(it);
}

// ---- Collection -------------------------------------------------------------------------

namespace {

corvid_coll* coll_of(const Collection& c) {
    return static_cast<corvid_coll*>(detail::Access::handle(c));
}

// The scan thunk: bridges the ABI callback onto std::function.
struct ScanCtx {
    std::function<bool(std::string_view, ValueView)>* fn;
    std::exception_ptr pending;
};

// The update thunk: current -> replacement, exceptions deferred.
struct UpdateCtx {
    std::function<Value(std::optional<ValueView>)>* fn;
    std::exception_ptr pending;
};

}  // namespace

// The thunks carry C language linkage: the engine calls them through
// C-linkage function-pointer typedefs.
extern "C" {

static int corvidpp_scan_thunk(void* ctx, const std::uint8_t* key, size_t key_len,
                               const corvid_value* doc) {
    auto* c = static_cast<ScanCtx*>(ctx);
    try {
        return (*c->fn)(std::string_view(reinterpret_cast<const char*>(key), key_len),
                        detail::Access::view(doc))
                   ? 1
                   : 0;
    } catch (...) {
        c->pending = std::current_exception();
        return 0;  // stop the walk; rethrown after the call returns
    }
}

static corvid_status corvidpp_update_thunk(void* ctx, const corvid_value* current,
                                           corvid_value** out) {
    auto* c = static_cast<UpdateCtx*>(ctx);
    try {
        Value next =
            (*c->fn)(current != nullptr
                         ? std::optional<ValueView>(detail::Access::view(current))
                         : std::nullopt);
        *out = static_cast<corvid_value*>(detail::Access::steal(next));
        return CORVID_OK;
    } catch (...) {
        c->pending = std::current_exception();
        *out = nullptr;
        return CORVID_ERR;  // the aborting-callback contract
    }
}

}  // extern "C"

Collection::Collection(Collection&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Collection& Collection::operator=(Collection&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_collection_free(static_cast<corvid_coll*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Collection::~Collection() {
    if (h_ != nullptr) corvid_collection_free(static_cast<corvid_coll*>(h_));
}

std::string_view Collection::name() const noexcept {
    size_t len = 0;
    const char* n = corvid_collection_name(coll_of(*this), &len);
    if (n == nullptr) return {};
    return std::string_view(n, len);
}

void Collection::insert(std::string_view key, const Value& doc) {
    check(corvid_insert(coll_of(*this),
                        reinterpret_cast<const std::uint8_t*>(key.data()),
                        key.size(), borrow_value(doc)));
}

void Collection::insert_with_ttl(std::string_view key, const Value& doc,
                                 std::int64_t ttl) {
    check(corvid_insert_with_ttl(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        borrow_value(doc), ttl));
}

std::string Collection::insert_auto(const Value& doc) {
    size_t klen = 0;
    std::uint8_t* key =
        corvid_insert_auto(coll_of(*this), borrow_value(doc), &klen);
    check_not_null(key);
    std::string out(reinterpret_cast<const char*>(key), klen);
    corvid_free(key);
    return out;
}

void Collection::put_many(std::span<const Kv> items) {
    std::vector<corvid_kv> kvs;
    kvs.reserve(items.size());
    for (const Kv& kv : items) {
        kvs.push_back(corvid_kv{
            reinterpret_cast<const std::uint8_t*>(kv.key.data()), kv.key.size(),
            borrow_value(*kv.val)});
    }
    check(corvid_put_many(coll_of(*this), kvs.data(), kvs.size()));
}

void Collection::put_many(std::initializer_list<Kv> items) {
    put_many(std::span<const Kv>(items.begin(), items.end()));
}

void Collection::update(std::string_view key,
                        std::function<Value(std::optional<ValueView>)> fn) {
    UpdateCtx ctx{&fn, nullptr};
    corvid_status st = corvid_update(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        corvidpp_update_thunk, &ctx);
    if (ctx.pending) std::rethrow_exception(ctx.pending);
    check(st);
}

void Collection::patch(std::string_view key, const Value& patch) {
    check(corvid_patch(coll_of(*this),
                       reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                       borrow_value(patch)));
}

bool Collection::compare_and_set(std::string_view key, const Value* expected,
                                 const Value* replacement) {
    std::int32_t applied = 0;
    check(corvid_compare_and_set(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        expected != nullptr ? borrow_value(*expected) : nullptr,
        replacement != nullptr ? borrow_value(*replacement) : nullptr, &applied));
    return applied != 0;
}

bool Collection::erase(std::string_view key) {
    std::int32_t existed = 0;
    check(corvid_delete(coll_of(*this),
                        reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                        &existed));
    return existed != 0;
}

std::size_t Collection::erase_where(Predicate&& pred) {
    size_t removed = 0;
    corvid_status st = corvid_delete_where(
        coll_of(*this), static_cast<corvid_pred*>(detail::Access::steal(pred)),
        &removed);
    check(st);
    return removed;
}

std::size_t Collection::erase_batch(std::span<const std::string_view> keys) {
    std::vector<const std::uint8_t*> raw;
    std::vector<size_t> lens;
    raw.reserve(keys.size());
    lens.reserve(keys.size());
    for (std::string_view k : keys) {
        raw.push_back(reinterpret_cast<const std::uint8_t*>(k.data()));
        lens.push_back(k.size());
    }
    size_t removed = 0;
    check(corvid_delete_batch(coll_of(*this), raw.data(), lens.data(), raw.size(),
                              &removed));
    return removed;
}

void Collection::set_ttl(std::string_view key, std::int64_t ttl) {
    check(corvid_set_ttl(coll_of(*this),
                         reinterpret_cast<const std::uint8_t*>(key.data()),
                         key.size(), ttl));
}

std::optional<std::int64_t> Collection::ttl(std::string_view key) {
    std::int64_t exp = 0;
    std::int32_t has = 0;
    check(corvid_get_ttl(coll_of(*this),
                         reinterpret_cast<const std::uint8_t*>(key.data()),
                         key.size(), &exp, &has));
    if (!has) return std::nullopt;
    return exp;
}

std::size_t Collection::purge_expired(std::int64_t now) {
    size_t purged = 0;
    check(corvid_purge_expired(coll_of(*this), now, &purged));
    return purged;
}

std::optional<Value> Collection::get(std::string_view key) {
    corvid_value* out = nullptr;
    corvid_status st =
        corvid_get(coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()),
                   key.size(), &out);
    check(st);
    if (out == nullptr) return std::nullopt;
    return detail::Access::own_value(out);
}

std::size_t Collection::len() {
    size_t n = 0;
    check(corvid_len(coll_of(*this), &n));
    return n;
}

void Collection::scan(std::function<bool(std::string_view key, ValueView doc)> fn) {
    ScanCtx ctx{&fn, nullptr};
    corvid_status st = corvid_scan(coll_of(*this), corvidpp_scan_thunk, &ctx);
    if (ctx.pending) std::rethrow_exception(ctx.pending);
    check(st);
}

Page Collection::page(std::optional<std::string_view> after, std::size_t limit) {
    const std::uint8_t* after_p =
        after.has_value()
            ? reinterpret_cast<const std::uint8_t*>(after->data())
            : nullptr;
    size_t after_len = after.has_value() ? after->size() : 0;
    corvid_rows* rows = nullptr;
    std::uint8_t* next = nullptr;
    size_t next_len = 0;
    check(corvid_page(coll_of(*this), after_p, after_len, limit, &rows, &next,
                      &next_len));
    Page page{Rows(rows), std::nullopt};
    if (next != nullptr) {
        page.next.emplace(reinterpret_cast<const char*>(next), next_len);
        corvid_free(next);
    }
    return page;
}

Rows Collection::phrase_search(std::string_view field, std::string_view phrase,
                               std::size_t k) {
    corvid_rows* rows = corvid_phrase_search(
        coll_of(*this), field.data(), field.size(), phrase.data(), phrase.size(), k);
    check_not_null(rows);
    return Rows(rows);
}

Query Collection::query() {
    corvid_query* q = corvid_query_new(coll_of(*this));
    check_not_null(q);
    return Query(q);
}

void Collection::link(std::string_view from, std::string_view rel,
                      std::string_view to) {
    check(corvid_link(coll_of(*this),
                      reinterpret_cast<const std::uint8_t*>(from.data()), from.size(),
                      rel.data(), rel.size(),
                      reinterpret_cast<const std::uint8_t*>(to.data()), to.size()));
}

void Collection::link_weighted(std::string_view from, std::string_view rel,
                               std::string_view to, double weight) {
    check(corvid_link_weighted(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(from.data()),
        from.size(), rel.data(), rel.size(),
        reinterpret_cast<const std::uint8_t*>(to.data()), to.size(), weight));
}

bool Collection::unlink(std::string_view from, std::string_view rel,
                        std::string_view to) {
    std::int32_t removed = 0;
    check(corvid_unlink(coll_of(*this),
                        reinterpret_cast<const std::uint8_t*>(from.data()),
                        from.size(), rel.data(), rel.size(),
                        reinterpret_cast<const std::uint8_t*>(to.data()), to.size(),
                        &removed));
    return removed != 0;
}

Strs Collection::neighbors(std::string_view key, std::string_view rel) {
    corvid_strs* s = corvid_neighbors(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        rel.data(), rel.size());
    check_not_null(s);
    return Strs(s);
}

Strs Collection::in_neighbors(std::string_view key, std::string_view rel) {
    corvid_strs* s = corvid_in_neighbors(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        rel.data(), rel.size());
    check_not_null(s);
    return Strs(s);
}

GeoHits Collection::neighbors_weighted(std::string_view key, std::string_view rel) {
    corvid_geohits* h = corvid_neighbors_weighted(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        rel.data(), rel.size());
    check_not_null(h);
    return GeoHits(h);
}

Strs Collection::traverse(std::string_view key, std::string_view rel,
                          std::size_t depth) {
    corvid_strs* s = corvid_traverse(
        coll_of(*this), reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
        rel.data(), rel.size(), depth);
    check_not_null(s);
    return Strs(s);
}

GeoHits Collection::geo_within_radius(std::string_view field, double lat, double lon,
                                      double km) {
    corvid_geohits* h = corvid_geo_within_radius(coll_of(*this), field.data(),
                                                 field.size(), lat, lon, km);
    check_not_null(h);
    return GeoHits(h);
}

GeoHits Collection::geo_within_bbox(std::string_view field, double min_lat,
                                    double min_lon, double max_lat,
                                    double max_lon) {
    corvid_geohits* h = corvid_geo_within_bbox(coll_of(*this), field.data(),
                                               field.size(), min_lat, min_lon,
                                               max_lat, max_lon);
    check_not_null(h);
    return GeoHits(h);
}

GeoHits Collection::geo_nearest(std::string_view field, double lat, double lon,
                                std::size_t k) {
    corvid_geohits* h =
        corvid_geo_nearest(coll_of(*this), field.data(), field.size(), lat, lon, k);
    check_not_null(h);
    return GeoHits(h);
}

void Collection::create_scalar_index(std::string_view field) {
    check(corvid_create_scalar_index(coll_of(*this), field.data(), field.size()));
}

void Collection::create_compound_index(std::span<const std::string_view> fields) {
    std::vector<const char*> names;
    std::vector<size_t> lens;
    for (std::string_view f : fields) {
        names.push_back(f.data());
        lens.push_back(f.size());
    }
    check(corvid_create_compound_index(coll_of(*this), names.data(), lens.data(),
                                       names.size()));
}

void Collection::create_compound_index(std::initializer_list<std::string_view> fields) {
    create_compound_index(
        std::span<const std::string_view>(fields.begin(), fields.end()));
}

void Collection::create_text_index(std::string_view field) {
    check(corvid_create_text_index(coll_of(*this), field.data(), field.size()));
}

void Collection::create_text_index_ondisk(std::string_view field) {
    check(corvid_create_text_index_ondisk(coll_of(*this), field.data(),
                                          field.size()));
}

void Collection::create_geo_index(std::string_view field) {
    check(corvid_create_geo_index(coll_of(*this), field.data(), field.size()));
}

void Collection::create_vector_index(std::string_view field, Metric metric) {
    check(corvid_create_vector_index(coll_of(*this), field.data(), field.size(),
                                     to_abi(metric)));
}

void Collection::create_vector_index_quantized(std::string_view field, Metric metric,
                                               Quant quant) {
    check(corvid_create_vector_index_quantized(coll_of(*this), field.data(),
                                               field.size(), to_abi(metric),
                                               to_abi(quant)));
}

void Collection::create_vector_index_ondisk(std::string_view field, Metric metric) {
    check(corvid_create_vector_index_ondisk(coll_of(*this), field.data(),
                                            field.size(), to_abi(metric)));
}

void Collection::create_vector_index_ondisk_quantized(std::string_view field,
                                                      Metric metric, Quant quant) {
    check(corvid_create_vector_index_ondisk_quantized(coll_of(*this),
                                                      field.data(), field.size(),
                                                      to_abi(metric), to_abi(quant)));
}

void Collection::create_vector_index_pq(std::string_view field, Metric metric,
                                        std::size_t subspaces, std::size_t bits) {
    check(corvid_create_vector_index_pq(coll_of(*this), field.data(), field.size(),
                                        to_abi(metric), subspaces, bits));
}

void Collection::create_vector_index_ondisk_pq(std::string_view field, Metric metric,
                                               std::size_t subspaces,
                                               std::size_t bits) {
    check(corvid_create_vector_index_ondisk_pq(coll_of(*this), field.data(),
                                               field.size(), to_abi(metric),
                                               subspaces, bits));
}

void Collection::set_schema(std::span<const FieldDef> defs) {
    std::vector<corvid_field_def> raw;
    raw.reserve(defs.size());
    for (const FieldDef& d : defs) {
        raw.push_back(corvid_field_def{d.name.data(), d.name.size(),
                                       to_abi(d.type), d.required ? 1 : 0,
                                       d.unique ? 1 : 0});
    }
    check(corvid_set_schema(coll_of(*this), raw.data(), raw.size()));
}

void Collection::set_schema(std::initializer_list<FieldDef> defs) {
    set_schema(std::span<const FieldDef>(defs.begin(), defs.end()));
}

std::vector<Field> Collection::schema() {
    corvid_schemaiter* it = nullptr;
    check(corvid_schema(coll_of(*this), &it));
    if (it == nullptr) return {};  // no schema declared
    std::vector<Field> out;
    for (;;) {
        corvid_field_def f{nullptr, 0, CORVID_FIELD_ANY, 0, 0};
        if (corvid_schemaiter_next(it, &f) != 1) break;
        out.push_back(Field{std::string(f.name, f.name_len),
                            field_type_from_abi(f.type), f.required != 0,
                            f.unique != 0});
    }
    corvid_schemaiter_free(it);
    return out;
}

// ---- Db --------------------------------------------------------------------------------

Db Db::open(std::string_view path) {
    corvid_db* db = corvid_open(path.data(), path.size());
    check_not_null(db);
    return Db(db);
}

Db Db::open_memory() {
    corvid_db* db = corvid_open_memory();
    check_not_null(db);
    return Db(db);
}

Db::Db(Db&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        if (h_ != nullptr) corvid_close(static_cast<corvid_db*>(h_));
        h_ = std::exchange(other.h_, nullptr);
    }
    return *this;
}

Db::~Db() {
    if (h_ != nullptr) corvid_close(static_cast<corvid_db*>(h_));  // best-effort
}

void Db::close() {
    if (h_ == nullptr) return;
    corvid_status st = corvid_close(static_cast<corvid_db*>(h_));
    h_ = nullptr;  // consumed either way (the ABI's §8 contract)
    check(st);
}

Collection Db::collection(std::string_view name) {
    corvid_coll* c = corvid_collection(static_cast<corvid_db*>(h_), name.data(),
                                       name.size());
    check_not_null(c);
    return Collection(c);
}

Strs Db::collections() {
    corvid_strs* s = corvid_collections(static_cast<corvid_db*>(h_));
    check_not_null(s);
    return Strs(s);
}

bool Db::compact() {
    int moved = 0;
    check(corvid_compact(static_cast<corvid_db*>(h_), &moved));
    return moved != 0;
}

void Db::backup(std::string_view path) {
    check(corvid_backup(static_cast<corvid_db*>(h_), path.data(), path.size()));
}

void Db::dump_to(std::string_view path) {
    check(corvid_dump_to_path(static_cast<corvid_db*>(h_), path.data(), path.size()));
}

void Db::load_from(std::string_view path) {
    check(corvid_load_from_path(static_cast<corvid_db*>(h_), path.data(),
                                path.size()));
}

void Db::load_from_with_renames(
    std::string_view path,
    std::initializer_list<std::pair<std::string_view, std::string_view>> renames) {
    std::vector<const char*> olds;
    std::vector<const char*> news;
    std::vector<size_t> olens;
    std::vector<size_t> nlens;
    for (const auto& [from, to] : renames) {
        olds.push_back(from.data());
        news.push_back(to.data());
        olens.push_back(from.size());
        nlens.push_back(to.size());
    }
    check(corvid_load_from_path_with_renames(static_cast<corvid_db*>(h_),
                                             path.data(), path.size(),
                                             olds.data(), news.data(),
                                             olens.data(), nlens.data(),
                                             olds.size()));
}

std::uint32_t ffi_version() noexcept { return corvid_ffi_version(); }

}  // namespace corvid
