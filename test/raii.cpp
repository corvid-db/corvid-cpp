// raii.cpp — the RAII-layer unit tests: the proof that this library's
// public C++ API (include/corvid/corvid.hpp) is the idiomatic surface
// the architecture ruling promises — construction from literals and
// initializer lists, accessors, map keys, predicate builders, the
// fluent query builder, phrase search, exceptions carrying code(),
// move-only handles — all leak-clean (every wrapper frees on its
// creation path; the sanitizer CI leg runs this file too).
//
// No framework: the same discipline as the golden port — CHECK fails
// loudly with file:line and the expectation; main returns nonzero.

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "corvid/corvid.hpp"

namespace {

int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: expected %s\n", __FILE__,  \
                         __LINE__, #cond);                                \
            std::exit(1);                                                 \
        }                                                                 \
        g_checks++;                                                       \
    } while (0)

#define EXPECT_THROWS(want, expr)                          \
    do {                                                   \
        bool threw = false;                                \
        try {                                              \
            (void)(expr);                                  \
        } catch (const corvid::Error& e) {                 \
            threw = true;                                  \
            CHECK(e.code() == (want));                     \
        }                                                  \
        CHECK(threw);                                      \
        g_checks++;                                        \
    } while (0)

// ---- move-only handle wrappers (the ruling: moves not copies) ---------

static_assert(!std::is_copy_constructible_v<corvid::Value>);
static_assert(!std::is_copy_assignable_v<corvid::Value>);
static_assert(!std::is_copy_constructible_v<corvid::Predicate>);
static_assert(!std::is_copy_constructible_v<corvid::Query>);
static_assert(!std::is_copy_constructible_v<corvid::Rows>);
static_assert(!std::is_copy_constructible_v<corvid::Strs>);
static_assert(!std::is_copy_constructible_v<corvid::GeoHits>);
static_assert(!std::is_copy_constructible_v<corvid::GroupIter>);
static_assert(!std::is_copy_constructible_v<corvid::Collection>);
static_assert(!std::is_copy_constructible_v<corvid::Db>);
static_assert(std::is_nothrow_move_constructible_v<corvid::Value>);
static_assert(std::is_nothrow_move_constructible_v<corvid::Db>);

void test_values() {
    using namespace corvid;

    // Literals + initializer-list construction.
    const float v[]{1.0f, 0.0f, 0.5f};
    Value doc = Value::map({{"title", lit::text("rust embedded database")},
                            {"n", 42},
                            {"ratio", 2.5},
                            {"ok", true},
                            {"tags", Value::array({"db", "rust"})},
                            {"v", lit::vec(v)}});
    CHECK(doc.type() == Type::Map);
    CHECK(doc.len() == 6);
    CHECK(doc.get("title").as_text() == std::string("rust embedded database"));
    CHECK(doc.get("n").as_int() == std::int64_t{42});
    CHECK(doc.get("ratio").as_float() == 2.5);
    CHECK(doc.get("ok").as_bool() == true);
    CHECK(!doc.get("missing"));
    CHECK(doc.get("missing").type() == Type::Null);
    CHECK(doc.get("tags").type() == Type::Array);
    CHECK(doc.get("tags").at(0).as_text() == std::string("db"));
    CHECK(doc.get("tags").at(1).as_text() == std::string("rust"));
    CHECK(!doc.get("tags").at(5));
    auto elems = doc.get("v").as_vector();
    CHECK(elems.size() == 3);
    CHECK(elems[0] == 1.0f);
    CHECK(elems[2] == 0.5f);

    // Wrong-type accessors answer empty (the ABI's inert convention).
    CHECK(!doc.get("n").as_text().has_value());
    CHECK(doc.get("n").as_bytes().empty());
    CHECK(!doc.as_bool().has_value());

    // bytes
    const std::uint8_t raw[]{0x00, 0x01, 0xff};
    Value blob = Value::bytes(raw);
    CHECK(blob.type() == Type::Bytes);
    auto span = blob.as_bytes();
    CHECK(span.size() == 3 && span[2] == 0xff);

    // Scalars and null.
    CHECK(Value().type() == Type::Null);
    CHECK(Value(nullptr).is_null());
    CHECK(Value(false).as_bool() == false);
    CHECK(Value(std::int64_t{-7}).as_int() == std::int64_t{-7});
    CHECK(Value(3.25).as_float() == 3.25);
    CHECK(Value("literal").as_text() == std::string("literal"));
    CHECK(Value(lit::text(std::string_view("sv"))).type() == Type::Text);

    // Builder mutations + the v0.3.0 map-key iterator.
    Value m = Value::empty_map();
    m.put("z", 1).put("键", 2).put("A1~B2", 3);
    CHECK(m.len() == 3);
    auto keys = m.map_keys();
    CHECK(keys.size() == 3);
    CHECK(keys[0] == "A1~B2");  // ascending key-BYTE order
    CHECK(keys[1] == "z");
    CHECK(keys[2] == "键");
    CHECK(Value::array({1, 2, 3}).len() == 3);
    CHECK(Value(7).map_keys().empty());  // non-map: inert, empty

    // clone is a deep copy; the original survives independently.
    Value original = Value::map({{"a", 1}});
    Value copy = original.clone();
    original.put("b", 2);
    CHECK(original.len() == 2);
    CHECK(copy.len() == 1);

    // A moved-from wrapper no longer owns; valid() says so.
    Value donor = Value::map({{"a", 1}});
    Value taker = std::move(donor);
    CHECK(!donor.valid());
    CHECK(taker.len() == 1);
}

void test_exceptions() {
    using namespace corvid;
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    // A missing file's directory surfaces as the engine's Io code (1).
    EXPECT_THROWS(ErrorCode::Database, Db::open("/nonexistent-dir-x/y.redb"));

    // Schema violation: 15 (frozen table, proven in the golden suite too).
    docs.set_schema({{"n", FieldType::Int, true, false}});
    docs.insert("ok", Value::map({{"n", 1}}));
    EXPECT_THROWS(ErrorCode::SchemaViolation,
                  docs.insert("bad", Value::map({{"x", 1}})));

    // The message is carried out of the engine's thread-local slot.
    try {
        docs.insert("bad2", Value::map({{"x", 1}}));
        CHECK(false);
    } catch (const Error& e) {
        CHECK(e.code() == ErrorCode::SchemaViolation);
        CHECK(e.message().size() > 0);
        g_checks++;
    }
}

void test_predicates_and_queries() {
    using namespace corvid;
    const float v1[]{1.0f, 0.0f}, v2[]{0.0f, 1.0f}, v3[]{0.9f, 0.1f};
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    docs.insert("s1", Value::map({{"kind", "doc"}, {"n", 1},
                                  {"body", "rust embedded database"},
                                  {"v", lit::vec(v1)}}));
    docs.insert("s2", Value::map({{"kind", "doc"}, {"n", 2},
                                  {"body", "python web frameworks"},
                                  {"v", lit::vec(v2)}}));
    docs.insert("s3", Value::map({{"kind", "doc"}, {"n", 3},
                                  {"body", "rust again database"},
                                  {"v", lit::vec(v3)}}));
    docs.insert("m1", Value::map({{"kind", "meta"}, {"n", 4}}));

    // The flagship fluent builder: filter + vector + text + RRF + MMR.
    Rows rows = docs.query()
                    .filter(pred::eq("kind", "doc"))
                    .vector("v", v1, 2, Metric::Cosine)
                    .text("body", "rust database", 2)
                    .fuse_rrf(60.0f)
                    .rerank_mmr(1.0f)
                    .limit(2)
                    .run();
    int n = 0;
    std::string first;
    for (const Row& r : rows) {
        if (n == 0) first = std::string(r.key);
        CHECK(r.doc.get("kind").as_text() == std::string("doc"));
        n++;
    }
    CHECK(n == 2);
    CHECK(first == "s1");

    // Predicate builders, one per family.
    CHECK(docs.query().filter(pred::ne("kind", "doc")).count() == 1);
    CHECK(docs.query().filter(pred::gt("n", 2)).count() == 2);
    CHECK(docs.query().filter(pred::le("n", 2)).count() == 2);
    CHECK(docs.query().filter(pred::lt("n", 3)).count() == 2);
    CHECK(docs.query().filter(pred::ge("n", 3)).count() == 2);
    CHECK(docs.query().filter(pred::exists("body")).count() == 3);
    CHECK(docs.query().filter(pred::in("n", {1, 3})).count() == 2);
    CHECK(docs.query().filter(pred::between("n", 2, 3)).count() == 2);
    CHECK(docs.query().filter(pred::starts_with("body", "rust")).count() == 2);
    CHECK(docs.query().filter(pred::contains("body", "database")).count() == 2);
    CHECK(docs.query()
              .filter(pred::all(pred::eq("kind", "doc"), pred::gt("n", 2)))
              .count() == 1);
    CHECK(docs.query()
              .filter(pred::any(pred::eq("n", 1), pred::eq("n", 4)))
              .count() == 2);
    CHECK(docs.query().filter(pred::none(pred::eq("kind", "doc"))).count() == 1);
    CHECK(docs.query()
              .filter(pred::compare("n", Cmp::Eq, 4))
              .filter(pred::geo_within("loc", 0.0, 0.0, 1.0))  // composed tree
              .count() == 0);

    // Aggregates (terminal — each consumes its builder).
    CHECK(docs.query().count() == 4);
    CHECK(docs.query().count_distinct("kind") == 2);
    CHECK(docs.query().sum("n") == 10.0);
    CHECK(docs.query().avg("n") == 2.5);
    CHECK(docs.query().min("n")->as_int() == 1);
    CHECK(docs.query().max("n")->as_int() == 4);
    int group_rows = 0;
    for (const GroupRow& g : docs.query().group_count("kind")) {
        CHECK(g.key == "doc" || g.key == "meta");
        group_rows++;
    }
    CHECK(group_rows == 2);

    // select() + order_by + offset: shape and order.
    Rows shaped = docs.query()
                      .select({"n", "kind"})
                      .order_by("n", /*descending=*/true)
                      .offset(1)
                      .limit(2)
                      .run();
    std::vector<std::int64_t> order;
    for (const Row& r : shaped) order.push_back(*r.doc.get("n").as_int());
    CHECK(order.size() == 2);
    CHECK(order[0] == 3);
    CHECK(order[1] == 2);
    for (const Row& r : shaped) CHECK(r.doc.len() == 2);  // projection held

    // A group iterator walks pairs key-by-key.
    GroupIter it = docs.query().group_sum("kind", "n");
    std::int64_t total = 0;
    for (const GroupRow& g : it) total += static_cast<std::int64_t>(g.value);
    CHECK(total == 10);
}

void test_phrase_search() {
    using namespace corvid;
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");
    docs.insert("s1", Value::map({{"body", "the rust embedded database story"}}));
    docs.insert("s2", Value::map({{"body", "python web frameworks"}}));
    docs.insert("s3", Value::map({{"body", "rust and database again"}}));

    // The v0.3.0 direct positional search: consecutive, in order.
    Rows full = docs.phrase_search("body", "embedded database", 10);
    int n = 0;
    for (const Row& r : full) {
        CHECK(r.key == "s1");
        n++;
    }
    CHECK(n == 1);

    // Word order matters for a phrase.
    Rows reversed = docs.phrase_search("body", "database embedded", 10);
    CHECK(!reversed.next().has_value());

    // k == 0 is the inert empty cursor.
    Rows none = docs.phrase_search("body", "embedded database", 0);
    CHECK(none.next().has_value() == false);

    // Pull-style next() walks the same rows.
    Rows pulled = docs.phrase_search("body", "database", 10);
    auto row = pulled.next();
    CHECK(row.has_value());
    CHECK(row->key == "s1" || row->key == "s3");
    row = pulled.next();
    CHECK(row.has_value());
    row = pulled.next();
    CHECK(!row.has_value());
}

void test_mutations_and_reads() {
    using namespace corvid;
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    docs.insert("a", Value::map({{"n", 1}}));
    docs.insert("b", Value::map({{"n", 2}}));

    // get / len / absent
    CHECK(docs.get("a")->get("n").as_int() == 1);
    CHECK(!docs.get("zz"));
    CHECK(docs.len() == 2);

    // update: read-modify-write via callback (absent key inserts).
    docs.update("a", [](std::optional<ValueView> cur) {
        std::int64_t n = 0;
        if (cur) n = *cur->get("n").as_int();
        return Value::map({{"n", n + 1}});
    });
    CHECK(docs.get("a")->get("n").as_int() == 2);
    docs.update("fresh", [](std::optional<ValueView>) {
        return Value::map({{"n", 7}});
    });
    CHECK(docs.get("fresh")->get("n").as_int() == 7);

    // A throwing callback aborts the update and rethrows.
    bool rethrew = false;
    try {
        docs.update("a", [](std::optional<ValueView>) -> Value {
            throw std::runtime_error("abort");
        });
    } catch (const std::runtime_error&) {
        rethrew = true;
    }
    CHECK(rethrew);
    CHECK(docs.get("a")->get("n").as_int() == 2);  // unchanged

    // patch / CAS
    docs.patch("a", Value::map({{"patched", true}}));
    CHECK(docs.get("a")->get("patched").as_bool() == true);
    CHECK(docs.get("a")->get("n").as_int() == 2);  // patch merges
    auto current = docs.get("a");
    Value repl = Value::map({{"n", 99}});
    CHECK(docs.compare_and_set("a", &*current, &repl));
    CHECK(docs.get("a")->get("n").as_int() == 99);

    // insert_auto
    std::string k1 = docs.insert_auto(Value::map({{"n", 1}}));
    std::string k2 = docs.insert_auto(Value::map({{"n", 2}}));
    CHECK(k1.size() == 20);
    CHECK(k2 > k1);  // zero-padded, monotonic

    // put_many
    const Value x = Value::map({{"n", 10}});
    const Value y = Value::map({{"n", 20}});
    docs.put_many({{"pm1", &x}, {"pm2", &y}});
    CHECK(docs.get("pm1")->get("n").as_int() == 10);

    // scan with early stop
    int seen = 0;
    docs.scan([&](std::string_view, ValueView) {
        seen++;
        return seen < 3;  // stop after 3
    });
    CHECK(seen == 3);

    // page
    Page p = docs.page(std::nullopt, 3);
    int from_page = 0;
    for (const Row& r : p.rows) {
        (void)r;
        from_page++;
    }
    CHECK(from_page == 3);
    CHECK(p.next.has_value());
    Page p2 = docs.page(p.next, 100);
    CHECK(p2.next.has_value() == false);

    // erase / erase_where / erase_batch
    CHECK(docs.erase("pm1"));
    CHECK(!docs.erase("pm1"));
    CHECK(docs.erase_where(pred::eq("n", 20)) == 1);
    const std::string_view pair[]{"a", "b"};
    CHECK(docs.erase_batch(pair) == 2);

    // TTL
    docs.insert_with_ttl("t1", Value::map({{"n", 1}}), 1'000'000);
    CHECK(docs.ttl("t1").has_value());
    CHECK(!docs.ttl("a").has_value());
    docs.set_ttl("t1", 5'000'000);
    CHECK(docs.ttl("t1").value() == 5'000'000);
    CHECK(docs.purge_expired(0) == 0);
}

void test_graph_geo_indexes() {
    using namespace corvid;
    Db db = Db::open_memory();
    {
    Collection docs = db.collection("docs");

    docs.insert("n1", Value::map({{"loc", Value::array({37.7749, -122.4194})}}));
    docs.insert("n2", Value::map({{"loc", Value::array({37.7849, -122.4094})}}));
    docs.insert("n3", Value::map({{"loc", Value::array({40.7128, -74.0060})}}));

    // graph
    docs.link("n1", "knows", "n2");
    docs.link("n2", "knows", "n3");
    docs.link_weighted("n1", "rated", "n3", 4.5);
    auto friends = docs.neighbors("n1", "knows").to_vector();
    CHECK(friends.size() == 1 && friends[0] == "n2");
    auto inbound = docs.in_neighbors("n2", "knows").to_vector();
    CHECK(inbound.size() == 1 && inbound[0] == "n1");
    auto reach = docs.traverse("n1", "knows", 2).to_vector();
    CHECK(reach.size() == 2);
    auto weights = docs.neighbors_weighted("n1", "rated");
    auto w = weights.next();
    CHECK(w.has_value());
    CHECK(w->key == "n3");
    CHECK(w->distance_km == 4.5);
    CHECK(!w->doc);  // weighted hits carry no document
    CHECK(docs.unlink("n1", "knows", "n2"));
    CHECK(!docs.unlink("n1", "knows", "n2"));

    // geo (haversine kilometres)
    docs.create_geo_index("loc");
    auto hits = docs.geo_within_radius("loc", 37.7749, -122.4194, 5.0);
    int near = 0;
    for (const GeoHit& h : hits) {
        CHECK(h.doc);  // geo hits carry their document
        near++;
    }
    CHECK(near == 2);
    auto nearest = docs.geo_nearest("loc", 37.7749, -122.4194, 1);
    auto one = nearest.next();
    CHECK(one.has_value() && one->distance_km < 5.0);
    auto box = docs.geo_within_bbox("loc", 37.0, -123.0, 38.0, -122.0);
    int inbox = 0;
    for (const GeoHit& h : box) {
        (void)h;
        inbox++;
    }
    CHECK(inbox == 2);
    EXPECT_THROWS(ErrorCode::InvalidArgument,
                  docs.geo_within_bbox("loc", 40.0, -74.0, 37.0, -123.0));

    // index families + schema
    const float v[]{1.0f, 0.0f};
    docs.insert("v1", Value::map({{"v", lit::vec(v)}, {"t", "body text"},
                                  {"s", 1}}));
    docs.create_scalar_index("s");
    docs.create_compound_index({"kind", "s"});
    docs.create_text_index("t");
    docs.create_text_index_ondisk("t");
    docs.create_vector_index("v", Metric::Cosine);
    docs.create_vector_index_quantized("v", Metric::Dot, Quant::Binary);
    docs.create_vector_index_ondisk("v", Metric::L2);
    docs.create_vector_index_ondisk_quantized("v", Metric::Cosine,
                                              Quant::Scalar);
    docs.create_vector_index_pq("v", Metric::Cosine, 2, 8);

    Collection other = db.collection("other");
    other.set_schema({{"id", FieldType::Text, true, true},
                      {"count", FieldType::Int, false, false}});
    auto fields = other.schema();
    CHECK(fields.size() == 2);
    CHECK(fields[0].name == "id" && fields[0].required && fields[0].unique);
    CHECK(fields[1].name == "count" && fields[1].type == FieldType::Int);
    other.insert("i1", Value::map({{"id", "x"}, {"count", 3}}));
    EXPECT_THROWS(ErrorCode::SchemaViolation,
                  other.insert("bad", Value::map({{"count", 3}})));  // missing id

    // admin surface (the engine lists collections that hold data)
    CHECK(db.collections().to_vector().size() == 2);
    CHECK(other.name() == "other");

    // compact needs every derived handle freed first (§4.13): the
    // Busy path IS the check that the RAII wrappers hold real handles.
    bool busy = false;
    try {
        (void)db.compact();
    } catch (const Error& e) {
        busy = e.code() == ErrorCode::Busy;
    }
    CHECK(busy);
    }  // docs + other freed here (destructors ran)
    // With every derived handle released, compact succeeds.
    (void)db.compact();
}


void test_persistence() {
    using namespace corvid;
    // Idempotent: drop any leftovers from an aborted earlier run first.
    std::remove("raii-tmp.redb");
    std::remove("raii-tmp.backup.redb");
    std::remove("raii-tmp.dump");
    // A file db: open, write, close, reopen (RAII ordering: collection
    // dies before db inside each scope).
    {
        Db db = Db::open("raii-tmp.redb");
        Collection docs = db.collection("docs");
        docs.insert("k", Value::map({{"n", 1}}));
    }  // docs freed, then db closed
    {
        Db db = Db::open("raii-tmp.redb");
        Collection docs = db.collection("docs");
        CHECK(docs.get("k")->get("n").as_int() == 1);
        db.backup("raii-tmp.backup.redb");
        EXPECT_THROWS(ErrorCode::BackupTargetExists,
                      db.backup("raii-tmp.backup.redb"));
        db.dump_to("raii-tmp.dump");
    }
    {
        Db db = Db::open_memory();
        db.load_from("raii-tmp.dump");
        Collection docs = db.collection("docs");
        CHECK(docs.get("k")->get("n").as_int() == 1);
    }
    {
        Db db = Db::open_memory();
        db.load_from_with_renames("raii-tmp.dump", {{"docs", "renamed"}});
        Collection renamed = db.collection("renamed");
        CHECK(renamed.get("k")->get("n").as_int() == 1);
    }
    {
        Db db = Db::open("raii-tmp.redb");
        {
            Collection docs = db.collection("docs");
            CHECK(docs.get("k")->get("n").as_int() == 1);
        }  // docs freed — derived handle count back to zero
        (void)db.compact();  // succeeds now; data may or may not move
    }
    std::remove("raii-tmp.redb");
    std::remove("raii-tmp.backup.redb");
    std::remove("raii-tmp.dump");
}

}  // namespace

int main() {
    test_values();
    test_exceptions();
    test_predicates_and_queries();
    test_phrase_search();
    test_mutations_and_reads();
    test_graph_geo_indexes();
    test_persistence();
    std::printf("raii_tests: ok (%d checks)\n", g_checks);
    return 0;
}
