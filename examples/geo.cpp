// geo.cpp — geospatial queries over [lat, lon] points: radius, bbox,
// and k-nearest, with haversine kilometres, plus the geo predicate for
// filtered queries and the geo index.

#include <cstdio>

#include "corvid/corvid.hpp"

namespace {

using namespace corvid;

void put_place(Collection& docs, std::string_view key, std::string_view name,
               double lat, double lon) {
    docs.insert(key, Value::map({{"name", lit::text(name)},
                                 {"loc", Value::array({lat, lon})}}));
}

int run() {
    Db db = Db::open_memory();
    Collection docs = db.collection("places");

    // San Francisco Bay Area pins, plus one far away.
    put_place(docs, "sfo", "San Francisco", 37.7749, -122.4194);
    put_place(docs, "oak", "Oakland", 37.8044, -122.2712);
    put_place(docs, "sjc", "San Jose", 37.3382, -121.8863);
    put_place(docs, "nyc", "New York", 40.7128, -74.0060);

    docs.create_geo_index("loc");

    // 1. Radius: everything within 30 km of San Francisco.
    std::printf("within 30km of SF:\n");
    for (const GeoHit& h :
         docs.geo_within_radius("loc", 37.7749, -122.4194, 30.0)) {
        auto name = h.doc.get("name").as_text();
        std::printf("  %.*s %.*s %.2fkm\n", static_cast<int>(h.key.size()),
                    h.key.data(), static_cast<int>(name ? name->size() : 1),
                    name ? name->data() : "?", h.distance_km);
    }

    // 2. Nearest: the two closest pins to the Golden Gate.
    std::printf("2 nearest to the Golden Gate:\n");
    for (const GeoHit& h : docs.geo_nearest("loc", 37.8199, -122.4783, 2)) {
        auto name = h.doc.get("name").as_text();
        std::printf("  %.*s %.*s %.2fkm\n", static_cast<int>(h.key.size()),
                    h.key.data(), static_cast<int>(name ? name->size() : 1),
                    name ? name->data() : "?", h.distance_km);
    }

    // 3. Bbox: the South Bay window catches San Jose only.
    std::printf("bbox (South Bay):\n");
    for (const GeoHit& h :
         docs.geo_within_bbox("loc", 37.0, -122.2, 37.5, -121.5)) {
        auto name = h.doc.get("name").as_text();
        std::printf("  %.*s %.*s\n", static_cast<int>(h.key.size()),
                    h.key.data(), static_cast<int>(name ? name->size() : 1),
                    name ? name->data() : "?");
    }

    // 4. The geo PREDICATE: geo filtering composed into any query —
    //    here combined with a scalar filter through pred::all().
    std::size_t near_count = docs.query()
                                 .filter(pred::geo_within("loc", 37.7749,
                                                          -122.4194, 50.0))
                                 .count();
    std::printf("pred geo_within(50km) count: %zu\n", near_count);
    return 0;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const corvid::Error& e) {
        std::fprintf(stderr, "geo: corvid error: %s\n", e.what());
        return 1;
    }
}
