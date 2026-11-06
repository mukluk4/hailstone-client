#pragma once

// The script host's insides (mq/lua_mq.h), shared by its files: lua_mq.cpp (the mq module, running scripts) and
// lua_hailstone.cpp (the hailstone module: scripts as plugins). Not for use elsewhere.

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <lua.hpp>

#include "core/log.h"
#include "core/sha256.h"
#include "game/server_ext.h"
#include "mq/lua_mq.h"

namespace nmsc::mq {

namespace fs = std::filesystem;

// The mq.web table (mq/lua_mq_web.cpp), pushed onto the stack.
void push_web_module(lua_State* L);
using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kMemoryLimit = 256u << 20; // a script's Lua heap
inline constexpr std::int64_t kSliceMs = 3;             // run this long, then wait for the next frame
inline constexpr std::int64_t kRunawayMs = 5000;        // this long without a chance to yield: the script is stopped
inline constexpr std::size_t kMaxQueuedEvents = 1000;
inline constexpr std::int64_t kPressMs = 50;            // a Lua social in press mode runs to its end within this
inline constexpr std::int64_t kFaceMs = 5;              // a social's face
inline constexpr std::int64_t kFaceEveryMs = 200;       // how often a face runs

struct Script;
class LuaExtension;
// The hailstone module (mq/lua_hailstone.cpp): the global `hailstone` in a new script state, and what a stopping
// script gave the client (its server extension, its UI) taken back.
void register_hailstone(lua_State* L);
void release_plugin(Script& s);
// The script's mq.configDir: env.config_dir for a script with every capability, config/<name>/ for another.
fs::path script_config_dir(const Script& s);

struct ScriptHost::Impl {
    ScriptHost* self = nullptr;
    ScriptEnv env;
    plugin::Hooks::Owner owner = 0;
    std::vector<std::unique_ptr<Script>> scripts;
    std::set<std::string, std::less<>> blocked; // Hailstone fork: ScriptHost::set_blocked's names (lower case)
    Clock::time_point start = Clock::now();

    std::int64_t now() const {
        return env.now_ms ? env.now_ms()
                          : std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }
    void say(const std::string& text) const {
        if (env.print) env.print(text);
    }
    Script* find(std::string_view name) const;
    // Where scripts are looked for: the server's scripts/ (set_server_dir) first, then env.dirs.
    fs::path server_dir;
    std::vector<fs::path> dirs() const {
        std::vector<fs::path> out;
        if (!server_dir.empty()) out.push_back(server_dir);
        out.insert(out.end(), env.dirs.begin(), env.dirs.end());
        return out;
    }
    // One of the server's own scripts under its hailstone.cfg: the player cannot stop or pause it.
    bool kept(const Script& s) const;
    void end(Script& s, const std::string& why);
    std::optional<fs::path> resolve(const std::string& name) const;
    void lua_command(std::string_view args);
    void reap();
    // A new script's Lua state with the libraries, the mq module and ImGui; false with the reason.
    bool open_state(Script& sc, std::string& why);
    // The character's autorun list (set_autorun_file): the file and its names, lower case, in file order.
    fs::path autorun_file;
    std::vector<std::string> autorun;
    // Starts those of `names` that are not running and the rules allow, quietly (not asked for by a command).
    void start_allowed(const std::vector<std::string>& names);
    // found()'s hashes, by file, kept while the file's size and time stay the same (the window asks each frame).
    struct HashEntry {
        std::uintmax_t size = 0;
        fs::file_time_type time{};
        Sha256Digest hash{};
    };
    mutable std::map<fs::path, HashEntry> hashes;
    std::optional<Sha256Digest> file_hash(const fs::path& file) const;

    // The Lua socials' states (press_button), by social.
    std::map<int, std::unique_ptr<Script>> buttons;
    std::map<int, std::pair<Sha256Digest, std::string>> broken; // socials whose code does not compile, by hash
    Script* button(const ButtonPress& p, std::string& why);
    void stop_threads(Script& s);
    void resume_threads(Script& s);
    void draw_windows(Script& s);
    plugin::ButtonRules rules(const game::LuaSocial& lua) const;
};

struct Script {
    ScriptHost::Impl* host = nullptr;
    std::string name;
    fs::path entry, root;
    Sha256Digest hash{};
    std::uint32_t caps = 0;
    lua_State* L = nullptr;
    lua_State* co = nullptr;
    int co_ref = LUA_NOREF;
    std::vector<std::string> args;
    bool started = false;
    enum class Status { Running, Paused, Exited, Failed } status = Status::Running;
    std::string error;
    bool stop_requested = false;
    int busy = 0; // entered (resuming, a bind, a window): closing waits
    std::size_t memory = 0;
    plugin::Hooks::Owner owner = 0;
    Clock::time_point slice_start{};

    struct Event {
        std::string name;
        std::regex re;
        std::vector<int> order;
        int fn = LUA_NOREF;
    };
    std::vector<Event> events;
    bool chat_hooked = false;
    std::deque<std::pair<std::string, std::vector<std::string>>> queued; // event name, {line, args...}
    std::vector<std::pair<std::string, int>> windows;                    // mq.imgui: name, draw function
    std::vector<std::pair<lua_State*, int>> threads;                     // binds still running: thread, its ref
    std::map<std::string, int> binds;                                    // command, function ref

    // A Lua social's state (ScriptHost::press_button).
    int social = -1;
    int press_ref = LUA_NOREF, face_ref = LUA_NOREF;
    std::int64_t runaway_ms = kRunawayMs;                // no chance to yield this long: an error
    int commands_left = -1;                              // press mode: commands this press may still send
    int press_commands = 0;
    bool face_eval = false;                              // the face runs: no commands
    std::vector<std::string>* dry = nullptr;             // a dry run: the commands are noted here, not sent
    std::uint32_t presses = 0;
    ButtonFace face;
    std::int64_t face_at = 0;
    bool face_ran = false;

    // The script as a plugin (the hailstone module): its server extension and its member of the chain, the hook
    // table, what it set, and whether it gave the client anything (then it stays after its main chunk returns).
    std::shared_ptr<LuaExtension> extension;
    int chain_id = 0;
    int hooks_ref = LUA_NOREF;
    bool active = true;
    std::uint32_t flags = 0; // NMSC_FLAG_* (sdk/nmsc_plugin.h)
    std::optional<game::CharacterVitals> vitals;
    bool registered = false;
    bool main_done = false;

    static Script& of(lua_State* L) { return **static_cast<Script**>(lua_getextraspace(L)); }

    void fail(std::string why) {
        if (social >= 0) { // a Lua social reports and stays: the next press tries again
            host->say(std::format("[Lua] {}: {}", name, why));
            log::warn("lua {}: {}", name, why);
            return;
        }
        if (status == Status::Failed || status == Status::Exited) return;
        status = Status::Failed;
        error = std::move(why);
        host->say(std::format("[Lua] {} ended with an error: {}", name, error));
        log::warn("lua script {}: {}", name, error);
        stop_requested = true;
    }

    void close() {
        release_plugin(*this);
        if (host->env.hooks && owner) host->env.hooks->remove(owner);
        owner = 0;
        if (L) lua_close(L);
        L = co = nullptr;
    }
};

} // namespace nmsc::mq
