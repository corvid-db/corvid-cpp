// vector_index.cpp — the vector-index families: the exact scan vs the
// in-memory HNSW index, binary quantization, the on-disk family, and a
// close/reopen proving the index persists with the file database.

#include <cstdio>

#include "corvid/corvid.hpp"

namespace {

using namespace corvid;

// docs:begin:vector_index
void put_doc(Collection& docs, std::string_view key, float a, float b,
             std::string_view tag) {
    const float v[]{a, b};
    docs.insert(key,
                Value::map({{"v", lit::vec(v)}, {"tag", lit::text(tag)}}));
}

std::string run_vector(Rows rows) {
    std::string out;
    for (const Row& r : rows) {
        if (!out.empty()) out += ",";
        out += std::string(r.key);
    }
    return out;
}

int run() {
    const float probe[]{0.99f, 0.05f};
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    put_doc(docs, "a", 1.0f, 0.0f, "alpha");
    put_doc(docs, "b", 0.95f, 0.05f, "beta");
    put_doc(docs, "c", 0.0f, 1.0f, "gamma");
    put_doc(docs, "d", -1.0f, 0.0f, "delta");

    // 1. The exact scan: no index, the query walks every vector.
    std::printf("exact    : %s\n",
                run_vector(docs.query().vector("v", probe, 3).run()).c_str());

    // 2. The in-memory HNSW index: same answer, index-backed.
    docs.create_vector_index("v", Metric::Cosine);
    std::printf("hnsw     : %s\n",
                run_vector(docs.query().vector("v", probe, 3).run()).c_str());

    // 3. Binary-quantized: same family, compressed codes.
    docs.create_vector_index_quantized("v", Metric::Cosine, Quant::Binary);
    std::printf("binary   : %s\n",
                run_vector(docs.query().vector("v", probe, 3).run()).c_str());

    // 4. approx(): the index-first posture — the engine may answer
    //    from the ANN structure directly.
    std::printf("approx   : %s\n", run_vector(docs.query()
                                                  .vector("v", probe, 3)
                                                  .approx()
                                                  .run())
                                       .c_str());

    // 5. The on-disk family + persistence: file db, index, close,
    //    reopen — the index rides along in the file.
    const char* path = "vector-index-tmp.redb";
    {
        Db file = Db::open(path);
        Collection fdocs = file.collection("docs");
        put_doc(fdocs, "a", 1.0f, 0.0f, "alpha");
        put_doc(fdocs, "c", 0.0f, 1.0f, "gamma");
        fdocs.create_vector_index_ondisk("v", Metric::Cosine);
    }  // fdocs freed, file closed — index persisted
    {
        Db file = Db::open(path);
        Collection fdocs = file.collection("docs");
        std::printf("ondisk   : %s\n", run_vector(fdocs.query()
                                                     .vector("v", probe, 2)
                                                     .run())
                                            .c_str());
    }
    std::remove(path);
    return 0;
}
// docs:end:vector_index

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "vector_index: corvid error: %s\n", e.what());
        return 1;
    }
}
