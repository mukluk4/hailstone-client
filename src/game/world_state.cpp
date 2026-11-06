#include "game/world_state.h"

#include "game/mechanics/remote_spawns.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "core/log.h"
#include "game/boats.h"
#include "game/coords.h"
#include "game/race_models.h"

namespace nmsc::game {

std::string display_name(const std::string& name) {
    // Hailstone fork: a leading '#' (EQEmu's mark for a named NPC, "#The_Fabled_Vulak`Aerr") is left out, as the
    // original's trimName does.
    std::string out = name.substr(std::min(name.find_first_not_of('#'), name.size()));
    while (!out.empty() && std::isdigit(static_cast<unsigned char>(out.back()))) out.pop_back();
    for (char& c : out)
        if (c == '_') c = ' ';
    return out;
}

namespace {
// How far a spawn at rest may settle onto the floor (see WorldState::smooth).
constexpr float kRestSettleReach = 5.0f;
// Hailstone (WorldState::ground_npcs): how far over its floor an NPC may stand and still be drawn on it. Higher
// up, the server more likely means it (a ledge or a floor the zone files do not have), so it is left there.
constexpr float kGroundNpcReach = 50.0f;
// Lower than this over its floor it counts as standing on it. The landing check settles a spawn only while its last
// update is at most 12 s old, so one standing still longer keeps a smaller gap the server gave it.
constexpr float kGroundNpcSlack = 0.25f;

// How far below its position to draw a spawn that WorldState::ground_npcs puts on the floor (0: where it is);
// nothing when there is no floor under it to measure against.
std::optional<float> ground_drop(const Spawn& s, const WorldState::FloorFn& floor, const WorldState::RegionFn& region) {
    // NPCs and corpses on the ground only: not flying or levitating ones, a rider, or one riding a boat. Flymode 3
    // is EQEmu's GravityBehavior::Water, which it gives NPCs that may swim (a spiderling in East Commonlands, seen
    // live): out of the water they stand on the ground, and the region test below leaves those in the water alone.
    if (!(s.is_npc || s.is_corpse) || (s.fly_mode != 0 && s.fly_mode != 3) || s.mount_id || s.on_vehicle) return 0.0f;
    auto f = floor(s.position);
    if (!f) return std::nullopt;
    const float z = *f + s.heights.avatar_height;
    const float drop = s.position.z - z;
    if (!(drop > kGroundNpcSlack && drop <= kGroundNpcReach)) return 0.0f;
    // Not one in water or lava, nor one over it (a fish, or a spawn on a dock the zone files leave out).
    if (region && (region(s.position) != 0 || region(glm::vec3(s.position.x, s.position.y, z)) != 0)) return 0.0f;
    return drop;
}
} // namespace

void WorldState::smooth_vehicles(float dt, double now) {
    const float one = dt * 1000.0f;
    const std::span<const float> steps = physics_steps ? std::span<const float>(*physics_steps) : std::span<const float>(&one, 1);
    for (auto& [id, s] : spawns) {
        if (id == player_id || id == controlled_id || id == driven_id || !is_vehicle_race(s.race, s.gender)) continue;
        step_vehicle(s, dt, now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0, steps);
    }
    vehicles_smoothed = true;
}

void WorldState::smooth(float dt, double now, const FloorFn& floor, const RegionFn& region,
                        const VehicleFn& vehicle_under) {
    if (!vehicles_smoothed) smooth_vehicles(dt, now);
    vehicles_smoothed = false;
    float k = 1.0f - std::exp(-dt * 12.0f);
    const float one = dt * 1000.0f;
    const std::span<const float> steps = physics_steps ? std::span<const float>(*physics_steps) : std::span<const float>(&one, 1);
    for (auto& [id, s] : spawns) {
        if (id != player_id && id != controlled_id && is_vehicle_race(s.race, s.gender)) continue;
        // A spawn an update put on a vehicle's deck rides it (game/boats.h) until an update puts it elsewhere.
        if (id != player_id && id != controlled_id && !s.mount_id) {
            if (vehicle_under && s.vehicle_checked != s.update_time) {
                s.vehicle_checked = s.update_time;
                std::uint32_t v = vehicle_under(s.position);
                auto vit = v ? spawns.find(v) : spawns.end();
                if (vit != spawns.end()) attach_rider(s, v, vit->second);
                else s.on_vehicle = 0;
            }
            if (s.on_vehicle) {
                auto vit = spawns.find(s.on_vehicle);
                if (vit != spawns.end()) {
                    step_rider(s, vit->second, dt, now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0, steps);
                    continue;
                }
                s.on_vehicle = 0;
            }
        }
        // The original steps other spawns while their last update is at most 12 s old (AdvanceSpawn), but not the
        // local or the controlled player (the local player's mount, which the player's controls move).
        if (id != player_id && id != controlled_id && now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0) {
            // Each step of the frame (core/step_clock.h) steps the body and lands it, as the original's frames do.
            mechanics::RemoteBody b{s.position, s.velocity, s.heading, s.speed_heading, s.speed_run};
            for (float ms : steps) {
                const glm::vec3 before = b.position;
                mechanics::step_remote_spawn(b, ms, s.is_npc);
                // CollisionPass's landing check (LandingAndFallCheck) runs with every step, so a spawn stands at
                // floor + AvatarHeight (feet = z - AvatarHeight) at rest too: the server places NPCs with its own
                // per-race offsets (EQEmu's GetZOffset: 0.7 x 0.2 x size for a dwarf), not the client's AvatarHeight.
                // Gravity takes a grounded spawn down to the floor; a flying or levitating one is only kept out of it.
                // A spawn at rest settles only onto a floor within kRestSettleReach; farther (an EQEmu map that puts it
                // in the air) it stays at the server's z, unless ground_npcs draws it lower.
                if (floor) {
                    const bool moved = b.position != before;
                    if (auto f = floor(b.position)) {
                        float z = *f + s.heights.avatar_height;
                        if (moved) b.position.z = z;
                        else if (s.fly_mode == 0 ? std::fabs(z - b.position.z) <= kRestSettleReach : z > b.position.z)
                            b.position.z = z;
                    }
                }
            }
            s.velocity = b.velocity;
            s.heading = b.heading;
            s.position = b.position;
            // CollisionPass after the step: PlayerClient::sub_00506d20 @0x506d20 takes the region at the spawn's z
            // as its body environment (+0xa2, BodyEnvironmentChange) and CheckRegionChange @0x508e10 the region at
            // the feet + 0.25 (z + 0.25 - (the collision height - AvatarHeight)) as its feet environment (+0xa1,
            // FeetEnvironmentChange), so other spawns swim and tread water as the local player does.
            if (region) {
                s.body_wet = region(s.position);
                s.feet_wet = region(s.position - glm::vec3(0.0f, 0.0f, s.heights.avatar_height - 0.25f));
            }
        }
        glm::vec3 target = s.position;
        // Hailstone (ground_npcs): measured again when the position changes, after the option is turned on and after
        // a zone's floor is loaded (reground). With no floor under it yet, it is measured again next frame.
        if (id != player_id && id != controlled_id && ground_npcs && floor) {
            if (s.ground_probe_at != s.position) {
                const auto drop = ground_drop(s, floor, region);
                s.ground_drop = drop.value_or(0.0f);
                s.ground_probe_at = drop ? s.position : glm::vec3(std::numeric_limits<float>::quiet_NaN());
            }
            target.z -= s.ground_drop;
        } else {
            s.ground_drop = 0.0f;
            s.ground_probe_at = glm::vec3(std::numeric_limits<float>::quiet_NaN());
        }
        // A position that is not a number (a bad update) is not followed: the spawn stays drawn where it last was.
        if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) continue;
        // Teleports and new spawns snap, as does a drawn place or heading that is not a number (the test is written so
        // NaN fails it), so it recovers at the next good update.
        if (!s.display_valid || !(glm::distance(s.display_position, target) <= 50.0f) ||
            !std::isfinite(s.display_heading)) {
            s.display_position = target;
            s.display_heading = s.heading;
            s.display_valid = true;
            continue;
        }
        s.display_position += (target - s.display_position) * k;
        float dh = std::fmod(s.heading - s.display_heading + 768.0f, 512.0f) - 256.0f; // shortest way round
        s.display_heading = std::fmod(s.display_heading + dh * k + 512.0f, 512.0f);
    }
    // PhysicsObject::vf8 @0x59e170 with each step of a rider: it takes its mount's position and heading, SpeedRun 0
    // and (unless +0x163) no velocity. The rider is drawn where its mount is.
    for (auto& [id, s] : spawns) {
        auto m = s.mount_id ? spawns.find(s.mount_id) : spawns.end();
        if (m == spawns.end() || id == player_id) continue;
        s.position = m->second.position;
        s.heading = m->second.heading;
        s.speed_run = 0.0f;
        s.velocity = glm::vec3(0.0f);
        s.display_position = m->second.display_position;
        s.display_heading = m->second.display_heading;
        s.display_valid = m->second.display_valid;
    }
}

void WorldState::reground() {
    for (auto& [id, s] : spawns) s.ground_probe_at = glm::vec3(std::numeric_limits<float>::quiet_NaN());
}

void WorldState::set_heights(Spawn& s, float view_offset, float anim_scale) const {
    game::set_heights(s.heights, height_inputs(height_tables.get(), s.race, s.gender, old_model_scale), view_offset,
                      anim_scale);
}

void WorldState::change_height(Spawn& s, float size, float view_offset, float anim_scale) {
    // The mount chain: the spawn, its mount (EQPlayer::GetMount's vf3c), that one's mount, ... (a cycle stops).
    std::vector<HeightLink> chain;
    for (Spawn* at = &s; at && chain.size() < spawns.size();) {
        HeightLink link;
        link.heights = &at->heights;
        link.inputs = height_inputs(height_tables.get(), at->race, at->gender, old_model_scale);
        link.type = at->is_corpse ? 2 : (at->is_npc ? 1 : 0);
        link.has_actor = at->actor_made;
        link.mount_race = is_mount_race(at->race);
        chain.push_back(link);
        auto next = at->mount_id ? spawns.find(at->mount_id) : spawns.end();
        at = next == spawns.end() ? nullptr : &next->second;
    }
    game::change_height(std::span(chain), size, view_offset, anim_scale);
}

void WorldState::set_old_model_scale(bool on) {
    if (on == old_model_scale) return;
    old_model_scale = on;
    for (auto& [id, s] : spawns) set_heights(s, 0.0f, 1.0f);
}

std::vector<std::uint32_t> WorldState::make_actors(const ActorModelFn& actor_model) {
    std::vector<std::uint32_t> made;
    if (!actor_model) return made;
    for (auto& [id, s] : spawns) {
        if (s.actor_made) continue;
        std::optional<ActorModel> m = actor_model(s);
        if (!m) continue;
        s.actor_made = true;
        s.tunic_point = m->tunic_point;
        s.human_actor = m->human_fallback;
        made.push_back(id);
        // BuildActorForSpawn (0x48f837 / 0x48f85e): HUF_ACTORDEF / HUM_ACTORDEF, then SetRace(1) unless a mount
        // race; the gender is kept.
        const std::uint16_t sent_race = s.race;
        if (m->human_fallback && !is_mount_race(s.race)) s.race = 1;
        // The player's and their pets' looks, for reports of models drawn wrong.
        if (id == player_id || (player_id != 0 && s.pet_owner_id == player_id)) {
            const Appearance& a = s.appearance;
            auto list = [](const std::array<std::uint32_t, 9>& v) {
                std::string out;
                for (std::uint32_t x : v) out += (out.empty() ? "" : ",") + std::to_string(x);
                return out;
            };
            log::info("appearance {}{}: race {}{} gender {} class {} size {:.1f} texture {} helm {} face {} hair {} / {} "
                      "beard {} / {} eyes {} / {} heritage {} tattoo {} details {} material [{}] tint [{}] hero_forge [{}] "
                      "npc_tint {}",
                      s.name, id == player_id ? "" : " (pet)", sent_race, m->human_fallback ? " (as human)" : "", s.gender,
                      s.class_id, s.size, a.texture, a.helm, a.face, a.hair_style, a.hair_color, a.beard, a.beard_color,
                      a.eye_color1, a.eye_color2, a.drakkin_heritage, a.drakkin_tattoo, a.drakkin_details,
                      list(a.material), list(a.tint), list(a.hero_forge), a.npc_tint);
        }
        if (!m->tunic_point) continue;
        float view = s.heights.view_height;
        if (auto to = actor_height_fixup(s.race, s.heights.height)) change_height(s, *to, 0.0f, 1.0f);
        s.heights.view_height = view;
    }
    std::sort(made.begin(), made.end());
    return made;
}

} // namespace nmsc::game
