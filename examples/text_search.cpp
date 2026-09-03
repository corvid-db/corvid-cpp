// text_search.cpp — BM25 text search through the fluent builder,
// English plus CJK (the analyzer handles both, UTF-8 in and out), and
// the v0.3.0 direct phrase search: consecutive, in-order tokens.

#include <cstdio>

#include "corvid/corvid.hpp"

namespace {

using namespace corvid;

// docs:begin:text_search
int run() {
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    docs.insert("s1", Value::map({{"body", "the rust embedded database story"}}));
    docs.insert("s2", Value::map({{"body", "python web frameworks"}}));
    docs.insert("s3", Value::map({{"body", "rust and database, again rust"}}));
    docs.insert("c1", Value::map({{"body", "嵌入式数据库"}}));
    docs.insert("c2", Value::map({{"body", "网络应用框架"}}));

    docs.create_text_index("body");

    // 1. BM25 via the builder: multi-term OR ranking.
    std::printf("bm25 'rust database':\n");
    for (const Row& r :
         docs.query().text("body", "rust database", 3).run()) {
        auto body = r.doc.get("body").as_text();
        std::printf("  %.*s score=%.6f %.*s\n", static_cast<int>(r.key.size()),
                    r.key.data(), static_cast<double>(r.score),
                    static_cast<int>(body ? body->size() : 1),
                    body ? body->data() : "?");
    }

    // 2. CJK: the analyzer tokenizes Han runs; the same builder ranks.
    std::printf("bm25 '数据库':\n");
    for (const Row& r : docs.query().text("body", "数据库", 2).run()) {
        auto body = r.doc.get("body").as_text();
        std::printf("  %.*s score=%.6f %.*s\n", static_cast<int>(r.key.size()),
                    r.key.data(), static_cast<double>(r.score),
                    static_cast<int>(body ? body->size() : 1),
                    body ? body->data() : "?");
    }

    // 3. The v0.3.0 phrase API: direct positional search — the phrase
    //    must appear as a CONSECUTIVE, IN-ORDER token run. "embedded
    //    database" matches s1; the same words reversed do not; k == 0
    //    is the inert empty cursor.
    std::printf("phrase 'embedded database':\n");
    for (const Row& r : docs.phrase_search("body", "embedded database", 10)) {
        std::printf("  %.*s score=%.6f\n", static_cast<int>(r.key.size()),
                    r.key.data(), static_cast<double>(r.score));
    }
    Rows reversed = docs.phrase_search("body", "database embedded", 10);
    std::printf("phrase 'database embedded': %s\n",
                reversed.next() ? "unexpected hit" : "(no hits — order matters)");
    Rows inert = docs.phrase_search("body", "embedded database", 0);
    std::printf("phrase k=0: %s\n",
                inert.next() ? "unexpected hit" : "(empty cursor, inert)");

    // 4. A CJK phrase, too: consecutive Han tokens.
    std::printf("phrase '嵌入式数据库':\n");
    for (const Row& r : docs.phrase_search("body", "嵌入式数据库", 10)) {
        std::printf("  %.*s score=%.6f\n", static_cast<int>(r.key.size()),
                    r.key.data(), static_cast<double>(r.score));
    }
    return 0;
}
// docs:end:text_search

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "text_search: corvid error: %s\n", e.what());
        return 1;
    }
}
