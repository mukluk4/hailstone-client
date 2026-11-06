#pragma once

// What the client knows about the zone it is in: the spawns the server has
// told it about and the local player. Filled from zone packets by the app;
// read by the renderer and UI.

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "game/spawn_heights.h"

namespace nmsc::game {

// What a spawn looks like (spawn packet, OP_WearChange, OP_Illusion). Slots
// are the nine visible ones: 0 head, 1 chest, 2 arms, 3 wrists, 4 hands,
// 5 legs, 6 feet, 7 primary, 8 secondary (game/appearance.h).
struct Appearance {
    // TextureType: 0xff = per-slot armour (always for PC-model races, whose
    // TextureType the client folds into slots 1-6), else the NPC body texture.
    std::uint8_t texture = 0xff;
    std::uint8_t helm = 0; // HeadType: NPC head variant
    // Spawn bitfield bit 23: PC-model races show their head armour only
    // with it set (PlayerClient::SwapHead returns early otherwise).
    bool show_helm = true;
    std::uint8_t face = 0;
    std::uint8_t hair_style = 0, hair_color = 0, beard = 0, beard_color = 0;
    std::uint8_t eye_color1 = 0, eye_color2 = 0;
    std::uint32_t drakkin_heritage = 0, drakkin_tattoo = 0, drakkin_details = 0;
    // Per slot: armour material (slots 0-6) or item model number IT<n>
    // (slots 7, 8), and the tint 0xAARRGGBB (a zero high byte = untinted).
    std::array<std::uint32_t, 9> material{};
    std::array<std::uint32_t, 9> tint{};
    std::array<std::uint32_t, 9> hero_forge{};
    // NpcTintIndex (spawn +0x51c; OP_SpawnAppearance 39): npct.ini Value<n>
    // tints new-style models (ActorClient_ApplyNpcTint); 0 none.
    std::uint32_t npc_tint = 0;
    // The class (ActorClient +0x14): SwapBodyMaterial's material rules for
    // monks (kind 7).
    std::uint8_t class_id = 0;

    bool operator==(const Appearance&) const = default;
};

struct Spawn {
    std::uint32_t id = 0;
    std::string name;      // as sent (e.g. "a_rat01"); see display_name()
    std::string last_name;
    std::string title, suffix; // the spawn record's AA title prefix and suffix (OP_SetTitleReply changes them)
    std::uint16_t race = 0;
    std::uint8_t class_id = 0;
    std::uint8_t level = 0;
    std::uint8_t gender = 0;
    bool is_npc = false;
    bool is_corpse = false;
    float size = 0.0f;
    float melee_radius = 0.0f; // the spawn record's MeleeRadius (PlayerClient +0x105c; EQEmu: size - 0.7)
    glm::vec3 position{0.0f};  // world (zone file) coordinates; see game/coords.h
    float heading = 0.0f;      // EQ heading units, 0..512 (game/coords.h)
    // The original's physics block between updates: an update snaps position
    // and heading and sets these; WorldState::smooth() steps them each frame
    // (mechanics::step_remote_spawn) for 12 s after the update.
    glm::vec3 velocity{0.0f};    // world units per physics tick (50 ms), from the update's deltas
    float speed_run = 0.0f;      // the update's animation field x 0.025
    float speed_heading = 0.0f;  // heading units per tick
    double update_time = 0.0;    // when position was last set (seconds, steady clock)
    float snap_error = 0.0f;     // how far the last update moved the stepped position (diagnostics)
    // The spawn record's flymode, then SpawnAppearance 19 (PlayerPhysics_SetFlyMode): 0 on the ground (gravity),
    // 1 flying, 2 levitating.
    std::uint8_t fly_mode = 0;
    // The region types under it (+0xa1 FeetWet, +0xa2 BodyWet: 0 dry, 5 water, ...), probed with each step
    // (WorldState::smooth's `region`); the local player's come from its own controller.
    std::uint8_t feet_wet = 0, body_wet = 0;
    // EQPlayer::SetHeights' results (game/spawn_heights.h): heights.avatar_height is the spawn's z above its
    // feet, heights.model_scale its actor's scale. WorldState::set_heights keeps them.
    SpawnHeights heights;
    bool actor_made = false; // CreatePlayerActor ran (its height fix-up; WorldState::make_actors)
    // What the actor was made with (WorldState::ActorModel): a tunic point (the definition's +0x34), and the
    // HUM / HUF fallback model. Riding asks both (game/mounts.h).
    bool tunic_point = false;
    bool human_actor = false;
    // Riding (game/mounts.h): EQPlayer::GetMount (+0x154) and PlayerBase_GetRider (+0x158) as spawn ids, 0 none.
    std::uint32_t mount_id = 0;
    std::uint32_t rider_id = 0;
    float walk_speed = 0.0f, run_speed = 0.0f; // the spawn record's walkspeed / runspeed (+0x330 / +0x350)
    std::uint8_t hp_percent = 100;
    std::uint8_t stand_state = 100; // SpawnAppearance animation: 100 stand, 110 sit, 111 crouch, 115 lying
    bool targetable = true;         // spawn bitfield bit 27
    bool cyclable = true;           // bit 28 (targetable_with_hotkey; PlayerZoneClient +0x161 bTargetCyclable)
    bool show_name = true;          // bit 29 (PlayerClient +0x1228 bDisplayNameSprite)
    bool click_through = false;     // bit 31: bClickThrough (+0x162), mask bit 8 (game/clicks.h)
    bool mercenary = false;         // the spawn record's IsMercenary (+0x208)
    std::uint32_t body_type = 0;
    // The spawn record's properties (PlayerBase +0x128, PlayerBase::HasProperty's list; [0] is body_type).
    std::vector<std::uint32_t> properties;
    bool summoned = false;          // bSummoned (+0x2e0): the spawn bitfield's bit 2 (EQEmu is_pet)
    std::uint32_t pet_owner_id = 0; // MasterID: the spawn record's petOwnerId, SpawnAppearance 25 (pet)
    // Social flags: the spawn record, then SpawnAppearance 21 (anonymous: 1
    // anonymous, 2 roleplaying), 22 / 23 (guild id / rank), 24 (AFK), 18 (linkdead).
    std::uint32_t guild_id = 0xffffffff;
    std::uint32_t guild_rank = 0;
    // bGuildShowAnim (+0x1f04): the spawn record's guild_show, then SpawnAppearance 52 (EQEmu: the rank's permission
    // 4, Display Guild Name). The name sprite shows the guild only with it.
    bool guild_show = false;
    std::uint8_t anon = 0;
    bool afk = false, lfg = false, linkdead = false, gm = false;
    std::uint8_t pvp = 0;      // PvPFlag +0x349 (the spawn record)
    std::uint32_t deity = 0;   // +0x518 (the spawn record)
    // Bazaar flags: the spawn record's trader bit and OP_BecomeTrader (PlayerClient +800), the
    // buyer bit (other_data bit 0) and OP_Barter's buyer appearance.
    bool trader = false, buyer = false;
    // PlayerClient +0x14c: the spawn record's PlayerState, changed by OP_PlayerStateAdd / Remove (bit 0x2 sheathed,
    // /sheathe; 0x8 aggressive, /aggressive).
    std::uint32_t player_state = 0;
    Appearance appearance;

    // The last one-shot animation played on it (OP_Animation, or one the
    // client starts): an old-style number (game/combat.h animation_code) or
    // a new-style code; action_seq counts up with every packet, so a repeat
    // restarts the clip. action_speed is the packet's speed byte (the clip
    // plays at speed / 10).
    std::uint8_t action = 0;
    std::uint8_t action_speed = 10;
    std::uint32_t action_seq = 0;

    // Where to draw it: eases toward position/heading so the corrections an
    // update makes do not show as jumps. Set by WorldState::smooth().
    glm::vec3 display_position{0.0f};
    float display_heading = 0.0f;
    bool display_valid = false;
    // Hailstone (WorldState::ground_npcs): how far below its position the spawn is drawn, so one the server left
    // standing in the air is drawn on the floor; and the position that was measured at (NaN: not yet).
    float ground_drop = 0.0f;
    glm::vec3 ground_probe_at{std::numeric_limits<float>::quiet_NaN()};
    // A vehicle (game/boats.h): how far the drawn pose is from the stepped one; it fades after each update.
    glm::vec3 display_error{0.0f};
    float display_heading_error = 0.0f;
    // A spawn standing on a vehicle (game/boats.h riders): the vehicle, its stepped and drawn place and heading in
    // the vehicle's frame, and the update the deck test last ran for.
    std::uint32_t on_vehicle = 0;
    glm::vec3 vehicle_local{0.0f}, vehicle_display_local{0.0f};
    float vehicle_local_heading = 0.0f, vehicle_display_local_heading = 0.0f;
    double vehicle_checked = -1.0;
};

// "a_rat01" -> "a rat": leading '#'s and trailing digits stripped, underscores to spaces.
std::string display_name(const std::string& name);

struct WorldState {
    std::string zone_short_name;
    std::string zone_long_name;
    std::uint32_t player_id = 0; // our own spawn id, once known
    std::uint32_t target_id = 0; // the player's current target, 0 = none
    // pinstControlledPlayer: the spawn the local player moves when it is not itself (its mount; game/mounts.h).
    std::uint32_t controlled_id = 0;
    // The small boat the local player has the helm of (OP_ControlBoat, game/boats.h steer_boat): the player moves it,
    // so it is not stepped.
    std::uint32_t driven_id = 0;
    std::unordered_map<std::uint32_t, Spawn> spawns;
    // racedata.txt and moddat.ini for SetHeights (null: the defaults, as without a RaceManager).
    std::shared_ptr<const HeightTables> height_tables;
    // Dword 0xddfba4 (NewZone +0x2b4): 1 from the startup defaults until the first OP_NewZone, which copies it.
    bool old_model_scale = true;

    // EQPlayer::SetHeights(view_offset, anim_scale) for a spawn, from s.heights.height.
    void set_heights(Spawn& s, float view_offset = 0.0f, float anim_scale = 1.0f) const;
    // PlayerZoneClient::ChangeHeight @0x5a4ec0 (OP_ChangeSize): the spawn, then each spawn of its mount chain (its
    // mount, that one's mount ...) with the same size and SetHeights(0, 1.0).
    void change_height(Spawn& s, float size, float view_offset, float anim_scale);
    // OP_NewZone's 0xddfba4: when it changes, every spawn gets SetHeights(0, 1.0).
    void set_old_model_scale(bool on);
    // What CreatePlayerActor finds for a spawn's actor.
    struct ActorModel {
        bool tunic_point = false;    // the model has a tunic point (the actor definition's +0x34)
        bool human_fallback = false; // the race's model is not loaded: the actor is HUM / HUF instead
    };
    // CreatePlayerActor for the spawns whose actor can be made now (`actor_model` nullopt while none can, as
    // before the zone is in). A spawn whose model is not loaded becomes a Human (SetRace(1), not for a mount
    // race; its heights stay). With a tunic point, the height fix-up: ChangeHeight(new, 0, 1.0) for the exact
    // sizes, and the ViewHeight from before put back. Runs once per spawn (and again after an illusion makes a
    // new actor).
    using ActorModelFn = std::function<std::optional<ActorModel>(const Spawn&)>;
    // Returns the spawns whose actor was made now, in id order (EQPlayer's constructor and CreateInitialActors then
    // run MakeRiderMountUp on them).
    std::vector<std::uint32_t> make_actors(const ActorModelFn& actor_model);

    // Moves every spawn's display position toward where it is predicted to be
    // now: its last position advanced by its movement since the update. `floor`
    // (optional) gives the floor height under a point, to keep spawns on the
    // ground (the landing check).
    using FloorFn = std::function<std::optional<float>(const glm::vec3&)>;
    // `region` (optional) gives the region type at a point (the low byte of its flags; 0 outside any).
    using RegionFn = std::function<std::uint8_t(const glm::vec3&)>;
    // `vehicle_under` (optional) gives the vehicle whose deck is under a point (0: none): a spawn an update puts
    // there rides it (game/boats.h).
    using VehicleFn = std::function<std::uint32_t(const glm::vec3&)>;
    void smooth(float dt, double now, const FloorFn& floor = {}, const RegionFn& region = {},
                const VehicleFn& vehicle_under = {});
    // The vehicles' part of smooth (game/boats.h), run before the local player moves so it is carried by, and
    // collides with, the boat where it is drawn this frame; smooth then leaves them alone.
    void smooth_vehicles(float dt, double now);
    bool vehicles_smoothed = false;
    // The game clock's steps this frame (core/step_clock.h, docs/mechanics.md "Frame rate"), in ms: the other spawns'
    // and the vehicles' physics (AdvanceSpawn's per-step friction) runs once for each. Unset: one step of the frame's
    // dt (the original's code as written). The drawn place eases over the frame's dt either way.
    std::optional<std::vector<float>> physics_steps;
    // Hailstone (option ground_npcs, a deliberate difference; docs/mechanics.md "Grounding NPCs"): an NPC on the
    // ground (flymode 0, or EQEmu's 3 out of water) that stands over the floor (by more than 0.25, at most 50), as EQEmu servers with a poor map
    // leave some, is drawn on the floor. Only where it is drawn changes (display_position, so picking and the NPC
    // walls too); its position, which range and the other rules use, stays the server's.
    bool ground_npcs = false;
    // The floor smooth is given changed (a zone's collision loaded): every spawn is measured for ground_npcs again.
    // Spawns can come before the zone's floor (character select's scene is still loaded), and one that stands
    // still would keep what it was measured against then.
    void reground();

    const Spawn* player() const {
        auto it = spawns.find(player_id);
        return it == spawns.end() ? nullptr : &it->second;
    }
};

} // namespace nmsc::game
