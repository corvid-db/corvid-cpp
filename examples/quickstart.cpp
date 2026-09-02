// quickstart.cpp — the README tour as a runnable file.
//
// Open an in-memory database, create a collection, insert three small
// documents carrying 2-d embeddings, run a kNN vector query under
// cosine, and print the ranked rows. RAII does the freeing: every
// handle dies on its creation path (the sanitizer CI leg proves it).

#include <cstdio>

#include "corvid/corvid.hpp"

namespace {

// docs:begin:quickstart
void put_doc(corvid::Collection& docs, std::string_view key,
             std::string_view title, std::string_view kind,
             std::span<const float> v) {
    using namespace corvid;
    docs.insert(key, Value::map({{"title", lit::text(title)},
                                 {"kind", lit::text(kind)},
                                 {"v", lit::vec(v)}}));
}

int run() {
    using namespace corvid;
    const float v1[]{1.0f, 0.0f}, v2[]{0.0f, 1.0f}, v3[]{0.9f, 0.1f};

    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    put_doc(docs, "p1", "rust embedded database", "doc", v1);
    put_doc(docs, "p2", "python web frameworks", "doc", v2);
    put_doc(docs, "p3", "rust again database", "doc", v3);

    // kNN: the 3 nearest documents to (1, 0) under cosine.
    const float probe[]{1.0f, 0.0f};
    Rows rows = docs.query()
                    .vector("v", probe, 3, Metric::Cosine)
                    .run();  // consumes the builder

    int rank = 0;
    for (const Row& r : rows) {
        auto title = r.doc.get("title").as_text();
        std::printf("%d. %.*s score=%.6f %.*s\n", ++rank,
                    static_cast<int>(r.key.size()), r.key.data(),
                    static_cast<double>(r.score),
                    static_cast<int>(title ? title->size() : 1),
                    title ? title->data() : "?");
    }
    return 0;
}
// docs:end:quickstart

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "quickstart: corvid error: %s\n", e.what());
        return 1;
    }
}
