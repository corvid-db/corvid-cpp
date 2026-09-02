// graph.cpp — the document graph: directed labeled edges (optionally
// weighted), outbound/inbound neighbors, transitive traversal, and the
// delete cascade (deleting a document drops its edges).

#include <cstdio>
#include <cstdlib>

#include "corvid/corvid.hpp"

namespace {

using namespace corvid;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "graph: expected %s\n", what);
        std::exit(1);
    }
}

int run() {
    Db db = Db::open_memory();
    Collection docs = db.collection("docs");

    for (const char* key : {"a", "b", "c", "d"})
        docs.insert(key, Value::map({{"name", lit::text(key)}}));

    // Directed, labeled edges.
    docs.link("a", "knows", "b");
    docs.link("b", "knows", "c");
    docs.link("c", "knows", "d");
    docs.link("a", "knows", "c");   // a shortcut edge
    docs.link("d", "knows", "a");   // a cycle

    std::printf("a knows     :");
    for (const auto& n : docs.neighbors("a", "knows").to_vector())
        std::printf(" %s", n.c_str());
    std::printf("\nc in-edges  :");
    for (const auto& n : docs.in_neighbors("c", "knows").to_vector())
        std::printf(" %s", n.c_str());
    std::printf("\n");

    // Transitive traversal to depth 2 (a -> {b, c} -> {c, d}).
    std::printf("traverse(a,2):");
    for (const auto& n : docs.traverse("a", "knows", 2).to_vector())
        std::printf(" %s", n.c_str());
    std::printf("\n");

    // Weighted edges + the weighted-neighbor cursor (distance_km
    // carries the edge weight; no documents ride along).
    docs.link_weighted("a", "rated", "b", 0.9);
    docs.link_weighted("a", "rated", "c", 0.5);
    std::printf("a rated     :");
    for (const GeoHit& h : docs.neighbors_weighted("a", "rated"))
        std::printf(" %.*s=%.1f", static_cast<int>(h.key.size()), h.key.data(),
                    h.distance_km);
    std::printf("\n");

    // The delete cascade: removing c drops its edges on both sides.
    expect(docs.erase("c"), "erase(c) existed");
    std::printf("after erasing c, a knows:");
    for (const auto& n : docs.neighbors("a", "knows").to_vector())
        std::printf(" %s", n.c_str());
    std::printf("\n");

    // unlink removes one specific edge.
    expect(docs.unlink("a", "knows", "b"), "unlink(a->b) removed");
    std::printf("after unlink(a->b), a knows:");
    for (const auto& n : docs.neighbors("a", "knows").to_vector())
        std::printf(" %s", n.c_str());
    std::printf("\n");
    return 0;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "graph: corvid error: %s\n", e.what());
        return 1;
    }
}
