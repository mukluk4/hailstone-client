#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "assets/character.h"
#include "assets/zone.h"
#include "game/collision.h"
#include "game/world_state.h"
#include "game_files.h"

using nmsc::game::CollisionMesh;

namespace {
nmsc::assets::Zone flat_zone() {
    // Two triangles: floor at z=0 over 0..100, a ledge at z=10 over 40..60.
    nmsc::assets::Zone z;
    nmsc::assets::Mesh m;
    auto quad = [&](float x0, float x1, float h) {
        auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (auto [x, y] : {std::pair{x0, x0}, {x1, x0}, {x1, x1}, {x0, x1}}) {
            nmsc::assets::Vertex v;
            v.position = {x, y, h};
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    };
    quad(0, 100, 0);
    quad(40, 60, 10);
    m.materials.push_back({});
    m.materials[0].frames = {"x.bmp"};
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0});
    z.terrain.push_back(m);
    return z;
}
} // namespace

TEST_CASE("floor_below picks the highest floor within the step height") {
    auto mesh = CollisionMesh::build(flat_zone());
    CHECK(mesh.triangle_count() == 4);
    CHECK(mesh.floor_below({10, 10, 5}, 2).value() == doctest::Approx(0));
    CHECK(mesh.floor_below({50, 50, 20}, 2).value() == doctest::Approx(10));
    // Standing below the ledge: the ledge is too high to step onto.
    CHECK(mesh.floor_below({50, 50, 1}, 2).value() == doctest::Approx(0));
    CHECK_FALSE(mesh.floor_below({500, 500, 0}, 2).has_value());
}

TEST_CASE("display_name strips digits and underscores") {
    CHECK(nmsc::game::display_name("a_rat01") == "a rat");
    CHECK(nmsc::game::display_name("Guard_Ahrens000") == "Guard Ahrens");
    CHECK(nmsc::game::display_name("#The_Fabled_Vulak`Aerr00") == "The Fabled Vulak`Aerr");
    CHECK(nmsc::game::display_name("##Lord_Bob") == "Lord Bob");
    CHECK(nmsc::game::display_name("###") == "");
    CHECK(nmsc::game::display_name("a_#rat") == "a #rat");
}

TEST_CASE("qeynos2 has floors under its streets" * doctest::skip(!nmsc::test::game_dir())) {
    auto zone = nmsc::assets::load_classic_zone(*nmsc::test::game_dir(), "qeynos2");
    auto mesh = CollisionMesh::build(zone);
    CHECK(mesh.triangle_count() > 10000);
    CHECK(mesh.floor_below({400, -300, 100}, 4).has_value());
}

TEST_CASE("line_of_sight is blocked by solid geometry") {
    auto mesh = CollisionMesh::build(flat_zone());
    CHECK(mesh.line_of_sight({10, 10, 5}, {90, 90, 5}));    // above the floor, beside the ledge
    CHECK_FALSE(mesh.line_of_sight({50, 50, 20}, {50, 50, -5})); // through the ledge and the floor
}

TEST_CASE("line_of_sight: only the cells under the sight line are searched, with the same answers") {
    // Many small walls scattered over 20 x 20 cells, and sight lines in every direction: the answer is the one from
    // testing every triangle (the same test, Moller-Trumbore strictly inside the segment), for each line short enough
    // to be checked at all (a box of 256 cells or fewer).
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> at(0.0f, 1280.0f), z(-20.0f, 40.0f), size(2.0f, 40.0f);
    std::vector<nmsc::game::CollisionTriangle> tris;
    for (int i = 0; i < 3000; ++i) {
        glm::vec3 a(at(rng), at(rng), z(rng));
        glm::vec3 b = a + glm::vec3(size(rng), size(rng) - 20.0f, size(rng) - 20.0f);
        glm::vec3 c = a + glm::vec3(size(rng) - 20.0f, size(rng), size(rng));
        tris.push_back(nmsc::game::CollisionTriangle::make(a, b, c, 0));
    }
    auto mesh = CollisionMesh::from_triangles(tris);
    auto blocked = [&](const glm::vec3& a, const glm::vec3& b) {
        glm::vec3 d = b - a;
        for (const auto& tri : tris) {
            glm::vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
            glm::vec3 pv = glm::cross(d, e2);
            float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-8f) continue;
            float inv = 1.0f / det;
            glm::vec3 tv = a - tri.v0;
            float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            glm::vec3 qv = glm::cross(tv, e1);
            float v = glm::dot(d, qv) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float hit = glm::dot(e2, qv) * inv;
            if (hit > 0.001f && hit < 0.999f) return true;
        }
        return false;
    };
    int checked = 0, seen = 0;
    std::uniform_real_distribution<float> reach(-600.0f, 600.0f);
    for (int i = 0; i < 3000; ++i) {
        glm::vec3 a(at(rng), at(rng), z(rng));
        glm::vec3 b = a + glm::vec3(reach(rng), reach(rng), reach(rng) * 0.1f);
        if (i % 7 == 0) b.y = a.y; // along the grid lines' directions too
        if (i % 11 == 0) b.x = a.x;
        auto cells = [](float p, float q) { return std::floor(std::max(p, q) / 64.0f) - std::floor(std::min(p, q) / 64.0f) + 1.0f; };
        if (cells(a.x, b.x) * cells(a.y, b.y) > 256.0f) continue;
        ++checked;
        const bool clear = mesh.line_of_sight(a, b);
        seen += clear;
        CHECK(clear == !blocked(a, b));
    }
    CHECK(checked > 1000);
    CHECK(seen > 100);           // some lines clear,
    CHECK(checked - seen > 100); // some blocked
}

TEST_CASE("qeynos: the Steel Warriors hall wall blocks line of sight" * doctest::skip(!nmsc::test::game_dir())) {
    auto zone = nmsc::assets::load_classic_zone(*nmsc::test::game_dir(), "qeynos");
    auto mesh = CollisionMesh::build(zone);
    // Server (-544, 58, 32.5) -> world (58, -544); the screenshot showed a wall ahead along +X.
    glm::vec3 eye(58.0f, -544.0f, 33.0f);
    auto floor = mesh.floor_below({58.0f, -544.0f, 32.5f}, 0.5f);
    REQUIRE(floor.has_value());
    MESSAGE("floor " << *floor);
    bool clear = true;
    float hit_at = 0;
    for (float d = 5; d <= 120; d += 5) {
        if (!mesh.line_of_sight(eye, eye + glm::vec3(d, 0, 0))) { clear = false; hit_at = d; break; }
    }
    MESSAGE("blocked within " << hit_at);
    CHECK_FALSE(clear);
}

namespace {
// Floors (near-horizontal solid terrain triangles) whose record normal
// normalize((v0 - v1) x (v2 - v1)) points up, and those pointing down: the
// back-face rule (skip when n . delta > 0) needs floors facing up.
std::pair<int, int> floor_facing(const nmsc::assets::Zone& zone) {
    int up = 0, down = 0;
    for (const auto& mesh : zone.terrain) {
        for (const auto& part : mesh.parts) {
            if (!part.solid) continue;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                const auto& v = mesh.vertices;
                std::uint32_t b = part.first_index + i + (mesh.front_ccw ? 2 : 1), c = part.first_index + i + (mesh.front_ccw ? 1 : 2);
                auto t = nmsc::game::CollisionTriangle::make(v[mesh.indices[part.first_index + i]].position,
                                                             v[mesh.indices[b]].position, v[mesh.indices[c]].position, 0);
                if (t.normal.z > 0.9f) ++up;
                if (t.normal.z < -0.9f) ++down;
            }
        }
    }
    return {up, down};
}
} // namespace

TEST_CASE("zone floors face up by the record normal (WLD and EQG)" * doctest::skip(!nmsc::test::game_dir())) {
    // Outdoor: few ceilings.
    auto [up, down] = floor_facing(nmsc::assets::load_classic_zone(*nmsc::test::game_dir(), "qeytoqrg"));
    MESSAGE("qeytoqrg floors: " << up << " up, " << down << " down");
    CHECK(up > 4 * down);
    for (const char* name : {"arena2", "barren"}) {
        auto [eup, edown] = floor_facing(nmsc::assets::load_eqg_zone(*nmsc::test::game_dir(), name));
        MESSAGE(std::string(name) << " floors: " << eup << " up, " << edown << " down");
        CHECK(eup > 4 * edown);
    }
}

TEST_CASE("player models carry their skeleton's bounding radius" * doctest::skip(!nmsc::test::game_dir())) {
    auto lib = nmsc::assets::CharacterLibrary::load(*nmsc::test::game_dir(), "qeynos2");
    for (const char* code : {"HUM", "HUF", "BAM", "DWM", "HOM", "OGM", "TRM", "ELM", "GNM", "IKM", "KEM", "ERM", "HIM", "HAM", "DAM", "FRM"}) {
        auto m = lib.model(code);
        if (!m) continue;
        MESSAGE(code << " radius " << m->bounding_radius << " bounds z " << m->bounds_min.z << ".." << m->bounds_max.z);
    }
}

TEST_CASE("ground_below and roof_above see what is drawn: a tunnel roof is no ground, an invisible lid no roof") {
    using nmsc::game::CollisionTriangle;
    // Up-facing by the record normal ((v0 - v1) x (v2 - v1)): v1 the corner, v0 along +x, v2 along +y.
    auto up = [](float z, std::uint8_t flags) {
        return CollisionTriangle::make({200, -100, z}, {-100, -100, z}, {-100, 200, z}, flags);
    };
    auto down = [](float z, std::uint8_t flags) {
        return CollisionTriangle::make({-100, 200, z}, {-100, -100, z}, {200, -100, z}, flags);
    };
    // North Ro's tunnel to East Commonlands: the floor at 0, the roof's underside at 30 (no top face over it).
    auto tunnel = CollisionMesh::from_triangles({up(0, 0), down(30, 0)});
    CHECK(tunnel.floor_below({0, 0, 70}, 0).value() == doctest::Approx(30)); // the roof, as a floor
    CHECK(tunnel.ground_below({0, 0, 70}).value() == doctest::Approx(0));
    CHECK(tunnel.roof_above({0, 0, 5}).value() == doctest::Approx(30));
    // Qeynos' line to the Hills: an invisible lid at 80 over the open ground.
    auto lid = CollisionMesh::from_triangles({up(0, 0), down(80, nmsc::game::kTriangleInvisible)});
    CHECK(lid.floor_below({0, 0, 100}, 0).value() == doctest::Approx(80));
    CHECK(lid.ground_below({0, 0, 100}).value() == doctest::Approx(0));
    CHECK(lid.ceiling_above({0, 0, 5}).value() == doctest::Approx(80));
    CHECK(!lid.roof_above({0, 0, 5}).has_value());
    // An invisible floor (North Ro's plane under its shore) is no ground either.
    auto plane = CollisionMesh::from_triangles({up(-111, nmsc::game::kTriangleInvisible)});
    CHECK(!plane.ground_below({0, 0, 0}).has_value());

    // From a zone: an Invisible material's triangles carry the flag.
    auto zone = [](nmsc::assets::BlendMode blend) {
        nmsc::assets::Zone z;
        nmsc::assets::Mesh m;
        for (glm::vec3 p : {glm::vec3(0, 0, 0), glm::vec3(0, 100, 0), glm::vec3(100, 0, 0)}) {
            nmsc::assets::Vertex v;
            v.position = p;
            m.vertices.push_back(v);
        }
        m.indices = {0, 1, 2}; // clockwise from above: a WLD floor
        m.materials.push_back({});
        m.materials[0].frames = {"x.bmp"};
        m.materials[0].blend = blend;
        m.parts.push_back({0, 3, 0});
        z.terrain.push_back(m);
        return z;
    };
    auto seen = CollisionMesh::build(zone(nmsc::assets::BlendMode::Opaque));
    CHECK(seen.ground_below({10, 10, 5}).value() == doctest::Approx(0));
    auto hidden = CollisionMesh::build(zone(nmsc::assets::BlendMode::Invisible));
    CHECK(hidden.floor_below({10, 10, 5}, 0).value() == doctest::Approx(0));
    CHECK(!hidden.ground_below({10, 10, 5}).has_value());
}
