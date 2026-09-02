// hybrid.cpp — the flagship hybrid-retrieval query, the fluent-builder
// tour: filter + vector + BM25 text, reciprocal-rank-fusion, MMR
// rerank, and a limit — one chained expression over the collection.

#include <cstdio>

#include "corvid/corvid.hpp"

namespace {

// docs:begin:hybrid
void put_doc(corvid::Collection& docs, std::string_view key,
             std::string_view kind, const char* body, const float* v) {
    using namespace corvid;
    Value doc = Value::map({{"kind", lit::text(kind)}});
    if (body != nullptr) doc.put("body", lit::text(body));
    if (v != nullptr) doc.put("v", lit::vec(std::span<const float>(v, 2)));
    docs.insert(key, doc);
}

int run() {
    using namespace corvid;
    const float v1[]{1.0f, 0.0f}, v2[]{0.0f, 1.0f}, v3[]{0.9f, 0.1f};

    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    put_doc(docs, "s1", "doc", "rust embedded database", v1);
    put_doc(docs, "s2", "doc", "python web frameworks", v2);
    put_doc(docs, "s3", "doc", "rust again database", v3);
    put_doc(docs, "m1", "meta", nullptr, nullptr);  // filtered out below

    // The flagship query: filter + vector + text, RRF + MMR + limit.
    const float probe[]{1.0f, 0.0f};
    Rows rows = docs.query()
                    .filter(pred::eq("kind", "doc"))
                    .vector("v", probe, 2, Metric::Cosine)
                    .text("body", "rust database", 2)
                    .fuse_rrf(60.0f)
                    .rerank_mmr(1.0f)
                    .limit(2)
                    .run();  // consumes the builder AND the predicate

    int rank = 0;
    for (const Row& r : rows) {
        auto body = r.doc.get("body").as_text();
        std::printf("%d. %.*s score=%.6f %.*s\n", ++rank,
                    static_cast<int>(r.key.size()), r.key.data(),
                    static_cast<double>(r.score),
                    static_cast<int>(body ? body->size() : 1),
                    body ? body->data() : "?");
    }
    return 0;
}
// docs:end:hybrid

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "hybrid: corvid error: %s\n", e.what());
        return 1;
    }
}
