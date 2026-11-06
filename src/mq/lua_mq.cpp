#include "mq/lua_mq.h"
#include "mq/lua_script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <iterator>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "core/ini_file.h"
#include "core/log.h"
#include "core/sha256.h"
#include "core/windows_names.h"
#include "game/slash_commands.h"
#include "game/world_state.h"
#include "mq/parse.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace nmsc::mq {

namespace fs = std::filesystem;

namespace {

char kExitKey; // mq.exit's error value

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// MQ's event pattern: #*# any text, #n# the n-th argument passed to the callback; everything else literal.
std::string event_regex(std::string_view pattern, std::vector<int>& order) {
    std::string re;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '#') {
            std::size_t close = pattern.find('#', i + 1);
            if (close != std::string_view::npos) {
                std::string_view inner = pattern.substr(i + 1, close - i - 1);
                std::int64_t n = 0;
                if (inner == "*") {
                    re += ".*";
                    i = close;
                    continue;
                }
                if (parse_int(inner, n) && n > 0) {
                    re += "(.*?)";
                    order.push_back(static_cast<int>(n));
                    i = close;
                    continue;
                }
            }
        }
        if (std::string_view(".^$|()[]{}*+?\\/").find(c) != std::string_view::npos) re += '\\';
        re += c;
    }
    return re;
}

} // namespace

Script* ScriptHost::Impl::find(std::string_view name) const {
    for (const auto& s : scripts)
        if (iequals(s->name, name)) return s.get();
    return nullptr;
}

namespace {

void* limited_alloc(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto* s = static_cast<Script*>(ud);
    std::size_t old = ptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        s->memory -= old;
        return nullptr;
    }
    if (new_size > old && s->memory + (new_size - old) > kMemoryLimit) return nullptr;
    void* p = std::realloc(ptr, new_size);
    if (p) s->memory = s->memory - old + new_size;
    return p;
}

void preempt_hook(lua_State* L, lua_Debug*) {
    Script& s = Script::of(L);
    auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.slice_start).count();
    if (ran >= kSliceMs && lua_isyieldable(L)) {
        lua_yield(L, 0);
        return;
    }
    if (ran >= s.runaway_ms) {
        if (s.runaway_ms < 1000) luaL_error(L, "ran longer than %d ms", static_cast<int>(s.runaway_ms));
        luaL_error(L, "ran %d seconds without a chance to pause", static_cast<int>(ran / 1000));
    }
}

void watch(lua_State* thread) { lua_sethook(thread, preempt_hook, LUA_MASKCOUNT, 1000); }

const Context& ctx_of(lua_State* L) { return Script::of(L).host->env.context(); }

const Registry& reg_of(lua_State* L) { return *Script::of(L).host->env.registry; }

void need(lua_State* L, std::uint32_t cap, const char* what) {
    if ((Script::of(L).caps & cap) != cap)
        luaL_error(L, "%s needs the server's '%s' permission", what, plugin::caps_text(cap).c_str());
}

// ---------------------------------------------------------------- mq.TLO values

struct TloRef {
    enum class Kind : std::uint8_t { Tlo, Member, Value } kind = Kind::Tlo;
    std::string name;
    Value base;
};

constexpr const char* kTloMeta = "nmsc.mq.tlo";

void push_ref(lua_State* L, TloRef ref) {
    void* mem = lua_newuserdatauv(L, sizeof(TloRef), 0);
    new (mem) TloRef(std::move(ref));
    luaL_setmetatable(L, kTloMeta);
}

TloRef& check_ref(lua_State* L, int i) { return *static_cast<TloRef*>(luaL_checkudata(L, i, kTloMeta)); }

Value resolve(lua_State* L, const TloRef& r, std::string_view index) {
    need(L, plugin::kCapRead, "mq.TLO");
    const Context& ctx = ctx_of(L);
    switch (r.kind) {
    case TloRef::Kind::Tlo: return reg_of(L).tlo(ctx, r.name, index);
    case TloRef::Kind::Member: return reg_of(L).member(ctx, r.base, r.name, index);
    case TloRef::Kind::Value: return r.base;
    }
    return Value::nil();
}

void push_plain(lua_State* L, const Value& v) {
    Value p = reg_of(L).plain(ctx_of(L), v);
    switch (p.kind) {
    case Value::Kind::Nil: lua_pushnil(L); return;
    case Value::Kind::Bool: lua_pushboolean(L, p.b); return;
    case Value::Kind::Int: lua_pushinteger(L, p.i); return;
    case Value::Kind::Float: lua_pushnumber(L, p.f); return;
    case Value::Kind::String: lua_pushlstring(L, p.s.data(), p.s.size()); return;
    case Value::Kind::Object: lua_pushstring(L, p.type ? p.type->name.c_str() : ""); return;
    }
}

// Arguments from `first` on, as MQ's index text ("a,b").
// (Lua errors are longjmps here: the text is built in a Lua buffer, so no C++ object is left half alive.)
void push_index_text(lua_State* L, int first, int last) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = first; i <= last; ++i) {
        if (i > first) luaL_addchar(&b, ',');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
}

int tlo_gc(lua_State* L) {
    check_ref(L, 1).~TloRef();
    return 0;
}

int tlo_index(lua_State* L) {
    TloRef& r = check_ref(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    Value base = resolve(L, r, {});
    push_ref(L, TloRef{TloRef::Kind::Member, lua_tostring(L, 2), std::move(base)});
    return 1;
}

int tlo_call(lua_State* L) {
    check_ref(L, 1);
    need(L, plugin::kCapRead, "mq.TLO");
    int top = lua_gettop(L);
    if (top < 2) {
        push_plain(L, resolve(L, *static_cast<TloRef*>(lua_touserdata(L, 1)), {}));
        return 1;
    }
    push_index_text(L, 2, top);
    std::size_t n = 0;
    const char* index = lua_tolstring(L, -1, &n);
    Value v = resolve(L, *static_cast<TloRef*>(lua_touserdata(L, 1)), std::string_view(index, n));
    push_ref(L, TloRef{TloRef::Kind::Value, {}, std::move(v)});
    return 1;
}

int tlo_tostring(lua_State* L) {
    TloRef& r = check_ref(L, 1);
    std::string text = reg_of(L).text(ctx_of(L), resolve(L, r, {}));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// An operand's plain value: a TLO value's, or the Lua value itself.
void push_operand(lua_State* L, int i) {
    if (luaL_testudata(L, i, kTloMeta)) {
        push_plain(L, resolve(L, *static_cast<TloRef*>(lua_touserdata(L, i)), {}));
    } else {
        lua_pushvalue(L, i);
    }
}

int tlo_concat(lua_State* L) {
    for (int i = 1; i <= 2; ++i) {
        if (luaL_testudata(L, i, kTloMeta)) {
            std::string t = reg_of(L).text(ctx_of(L), resolve(L, *static_cast<TloRef*>(lua_touserdata(L, i)), {}));
            lua_pushlstring(L, t.data(), t.size());
        } else {
            luaL_tolstring(L, i, nullptr);
        }
    }
    lua_concat(L, 2);
    return 1;
}

int tlo_compare(lua_State* L, int op) {
    push_operand(L, 1);
    push_operand(L, 2);
    bool numbers = lua_type(L, -1) == LUA_TNUMBER && lua_type(L, -2) == LUA_TNUMBER;
    bool strings = lua_type(L, -1) == LUA_TSTRING && lua_type(L, -2) == LUA_TSTRING;
    if (op != LUA_OPEQ && !numbers && !strings) return luaL_error(L, "attempt to compare a TLO value that is not a number");
    lua_pushboolean(L, lua_compare(L, -2, -1, op));
    return 1;
}
int tlo_eq(lua_State* L) { return tlo_compare(L, LUA_OPEQ); }
int tlo_lt(lua_State* L) { return tlo_compare(L, LUA_OPLT); }
int tlo_le(lua_State* L) { return tlo_compare(L, LUA_OPLE); }

// mq.TLO.<name>
int tlo_root_index(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    push_ref(L, TloRef{TloRef::Kind::Tlo, lua_tostring(L, 2), {}});
    return 1;
}

// ---------------------------------------------------------------- mq functions

int l_print(lua_State* L) {
    int top = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= top; ++i) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    Script::of(L).host->say(lua_tostring(L, -1));
    return 0;
}

int l_cmd(lua_State* L) {
    Script& s = Script::of(L);
    // (Raised at the caller of mq.cmd / mq.cmdf: level 2, past the prelude's wrapper.)
    if (s.face_eval) {
        luaL_where(L, 2);
        lua_pushstring(L, "a button's face cannot run commands");
        lua_concat(L, 2);
        return lua_error(L);
    }
    if (s.commands_left == 0) {
        luaL_where(L, 2);
        lua_pushfstring(L, "this server lets one press send at most %d commands", s.press_commands);
        lua_concat(L, 2);
        return lua_error(L);
    }
    if (s.commands_left > 0) --s.commands_left;
    std::string line = luaL_checkstring(L, 1);
    if (s.dry) {
        if ((s.caps & plugin::kCapRead) && !lower(line).starts_with("/noparse")) line = parse_macro_data(ctx_of(L), line);
        std::uint32_t needs = plugin::command_caps(line);
        if ((s.caps & needs) != needs) line += "    (blocked: needs '" + plugin::caps_text(needs & ~s.caps) + "')";
        s.dry->push_back(std::move(line));
        return 0;
    }
    if (!s.host->env.run) return 0;
    plugin::CallerScope scope(s.caps, s.name);
    s.host->env.run(line);
    return 0;
}

int l_gettime(lua_State* L) {
    lua_pushinteger(L, Script::of(L).host->now());
    return 1;
}

// mq.delay's argument in ms: a number, or text with ms / s / m / h.
int l_delay_ms(lua_State* L) {
    if (lua_type(L, 1) == LUA_TNUMBER) {
        lua_pushinteger(L, static_cast<lua_Integer>(lua_tonumber(L, 1)));
        return 1;
    }
    const char* given = luaL_checkstring(L, 1);
    double v = 0;
    bool ok = false;
    {
        std::string t = lower(given);
        double scale = 1;
        auto ends = [&](std::string_view suffix) {
            return t.size() >= suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends("ms")) t.resize(t.size() - 2);
        else if (ends("s")) { t.pop_back(); scale = 1000; }
        else if (ends("m")) { t.pop_back(); scale = 60000; }
        else if (ends("h")) { t.pop_back(); scale = 3600000; }
        ok = parse_float(t, v);
        v *= scale;
    }
    if (!ok) return luaL_error(L, "mq.delay: bad time '%s'", given);
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    return 1;
}

int l_parse(lua_State* L) {
    need(L, plugin::kCapRead, "mq.parse");
    std::string out = parse_macro_data(ctx_of(L), luaL_checkstring(L, 1));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// mq._dry(ms): true in a dry run, which notes the wait instead of waiting.
int l_dry(lua_State* L) {
    Script& s = Script::of(L);
    if (!s.dry) {
        lua_pushboolean(L, 0);
        return 1;
    }
    long long ms = luaL_checkinteger(L, 1);
    s.dry->push_back(std::format("(wait {} ms)", ms));
    lua_pushboolean(L, 1);
    return 1;
}

int l_exit(lua_State* L) {
    lua_pushlightuserdata(L, &kExitKey);
    return lua_error(L);
}

bool add_event(lua_State* L, Script& s, const std::string& name, const std::string& pattern);

int l_event(lua_State* L) {
    need(L, plugin::kCapEvents, "mq.event");
    Script& s = Script::of(L);
    const char* name_text = luaL_checkstring(L, 1);
    const char* pattern = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!add_event(L, s, name_text, pattern)) return luaL_error(L, "mq.event: bad pattern '%s'", pattern);
    return 0;
}

// The callback is argument 3 on L, the calling thread's stack.
bool add_event(lua_State* L, Script& s, const std::string& name, const std::string& pattern) {
    Script::Event e;
    e.name = name;
    try {
        e.re = std::regex(event_regex(pattern, e.order));
    } catch (const std::regex_error&) {
        return false;
    }
    lua_pushvalue(L, 3);
    e.fn = luaL_ref(L, LUA_REGISTRYINDEX);
    for (auto& old : s.events)
        if (old.name == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, old.fn);
            old = std::move(e);
            return true;
        }
    s.events.push_back(std::move(e));
    if (!s.chat_hooked) {
        s.chat_hooked = true;
        Script* sp = &s;
        s.host->env.hooks->on_chat(s.owner, [sp](plugin::ChatLine& line) {
            if (sp->status != Script::Status::Running && sp->status != Script::Status::Paused) return plugin::Flow::Continue;
            for (const auto& ev : sp->events) {
                std::smatch m;
                if (!std::regex_match(line.text, m, ev.re)) continue;
                std::vector<std::string> args{line.text};
                std::vector<std::pair<int, std::string>> numbered;
                for (std::size_t g = 0; g < ev.order.size() && g + 1 < m.size(); ++g)
                    numbered.emplace_back(ev.order[g], m[g + 1].str());
                std::sort(numbered.begin(), numbered.end());
                for (auto& [n, text] : numbered) args.push_back(std::move(text));
                if (sp->queued.size() < kMaxQueuedEvents) sp->queued.emplace_back(ev.name, std::move(args));
            }
            return plugin::Flow::Continue;
        });
    }
    return true;
}

int l_unevent(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    for (auto it = s.events.begin(); it != s.events.end();)
        if (it->name == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->fn);
            it = s.events.erase(it);
        } else {
            ++it;
        }
    std::erase_if(s.queued, [&](const auto& q) { return q.first == name; });
    return 0;
}

int l_flushevents(lua_State* L) {
    Script& s = Script::of(L);
    if (lua_isnoneornil(L, 1)) {
        s.queued.clear();
    } else {
        std::string name = luaL_checkstring(L, 1);
        std::erase_if(s.queued, [&](const auto& q) { return q.first == name; });
    }
    return 0;
}

// fn, {args} = mq._next_event([name]): the oldest queued event (of that name).
int l_next_event(lua_State* L) {
    Script& s = Script::of(L);
    std::optional<std::string> name;
    if (!lua_isnoneornil(L, 1)) name = luaL_checkstring(L, 1);
    for (auto it = s.queued.begin(); it != s.queued.end(); ++it) {
        if (name && it->first != *name) continue;
        int fn = LUA_NOREF;
        for (const auto& e : s.events)
            if (e.name == it->first) fn = e.fn;
        auto args = std::move(it->second);
        s.queued.erase(it);
        if (fn == LUA_NOREF) return l_next_event(L); // unevented meanwhile
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
        lua_createtable(L, static_cast<int>(args.size()), 0);
        for (std::size_t i = 0; i < args.size(); ++i) {
            lua_pushlstring(L, args[i].data(), args[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

// Runs fn(args...) as a new coroutine of the script (binds): it may mq.delay; pulse() resumes it.
void start_thread(Script& s, int fn_ref, const std::vector<std::string>& args) {
    if (!s.L || s.stop_requested) return;
    ++s.busy;
    lua_State* t = lua_newthread(s.L);
    int ref = luaL_ref(s.L, LUA_REGISTRYINDEX);
    watch(t);
    lua_rawgeti(t, LUA_REGISTRYINDEX, fn_ref);
    for (const auto& a : args) lua_pushlstring(t, a.data(), a.size());
    plugin::CallerScope scope(s.caps, s.name);
    s.slice_start = Clock::now();
    int nres = 0;
    int st = lua_resume(t, nullptr, static_cast<int>(args.size()), &nres);
    if (st == LUA_YIELD) {
        lua_pop(t, nres);
        s.threads.emplace_back(t, ref);
    } else {
        if (st != LUA_OK && !(lua_touserdata(t, -1) == &kExitKey)) s.fail(lua_tostring(t, -1) ? lua_tostring(t, -1) : "error");
        else if (st != LUA_OK) s.stop_requested = true; // mq.exit
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
    }
    --s.busy;
}

// mq.bind(cmd, fn): a command of the script's own (the 'commands' capability). The client's own commands (the game's,
// abbreviations included, and /lua, /plugin, MQ's) cannot be taken (plugin::Hooks::reserved_command); a name another
// script or plugin bound is taken over, as in MQ, until this one unbinds.
int l_bind(lua_State* L) {
    need(L, plugin::kCapCommands, "mq.bind");
    Script& s = Script::of(L);
    const char* given = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::string cmd = given;
    if (cmd.empty() || cmd.front() != '/') cmd.insert(cmd.begin(), '/');
    cmd = lower(cmd);
    if (s.host->env.hooks->reserved_command(cmd))
        return luaL_error(L, "mq.bind: %s is the client's own command", cmd.c_str());
    if (auto it = s.binds.find(cmd); it != s.binds.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        s.binds.erase(it);
        s.host->env.hooks->remove_command(s.owner, cmd);
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s.binds[cmd] = ref;
    Script* sp = &s;
    s.host->env.hooks->add_command(s.owner, cmd, [sp, ref](std::string_view args) {
        std::vector<std::string> words;
        std::string_view rest = args;
        while (!rest.empty()) {
            std::string w = game::next_cmd_arg(rest);
            if (w.empty() && rest.empty()) break;
            words.push_back(std::move(w));
        }
        start_thread(*sp, ref, words);
    });
    return 0;
}

int l_unbind(lua_State* L) {
    Script& s = Script::of(L);
    std::string cmd = lower(luaL_checkstring(L, 1));
    if (cmd.empty() || cmd.front() != '/') cmd.insert(cmd.begin(), '/');
    if (auto it = s.binds.find(cmd); it != s.binds.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        s.binds.erase(it);
        s.host->env.hooks->remove_command(s.owner, cmd);
    }
    return 0;
}

int l_imgui_init(lua_State* L) {
    need(L, plugin::kCapUi, "mq.imgui");
    Script& s = Script::of(L);
    const char* given = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::string name = given;
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    for (auto& w : s.windows)
        if (w.first == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, w.second);
            w.second = ref;
            return 0;
        }
    s.windows.emplace_back(name, ref);
    return 0;
}

int l_imgui_destroy(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    for (auto it = s.windows.begin(); it != s.windows.end(); ++it)
        if (it->first == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->second);
            s.windows.erase(it);
            break;
        }
    return 0;
}

int l_imgui_exists(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    bool found = std::any_of(s.windows.begin(), s.windows.end(), [&](const auto& w) { return w.first == name; });
    lua_pushboolean(L, found);
    return 1;
}

int l_get_all_spawns(lua_State* L) {
    need(L, plugin::kCapRead, "mq.getAllSpawns");
    const Context& ctx = ctx_of(L);
    const Type* spawn = reg_of(L).find_type("spawn");
    lua_newtable(L);
    if (!ctx.conn || !spawn) return 1;
    std::vector<std::uint32_t> ids;
    for (const auto& [id, s] : ctx.conn->world().spawns) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    lua_Integer n = 0;
    for (auto id : ids) {
        push_ref(L, TloRef{TloRef::Kind::Value, {}, Value::object(spawn, id)});
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Whether `file` is in the config directory under the script folder `dir` (mq.configDir is <scripts>/config):
// mq.pickle's data, which a script writes as it likes, is never run as a script or module. Both paths are taken
// canonical (links followed) and compared as Windows and a case-insensitive macOS volume would see them
// (core/windows_names.h: any case, a part's trailing dots and spaces dropped), so "Config", "config." or a link
// into it are the config directory too.
bool in_config_dir(const fs::path& dir, const fs::path& file) {
    std::error_code ec;
    fs::path d = fs::weakly_canonical(dir, ec);
    if (ec) d = dir.lexically_normal();
    fs::path f = fs::weakly_canonical(file, ec);
    if (ec) f = file.lexically_normal();
    std::string dk = windows_file_key(d.generic_string()), fk = windows_file_key(f.generic_string());
    while (!dk.empty() && dk.back() == '/') dk.pop_back();
    if (fk.size() <= dk.size() + 1 || fk.compare(0, dk.size(), dk) != 0 || fk[dk.size()] != '/') return false;
    const std::string_view rest = std::string_view(fk).substr(dk.size() + 1);
    return rest.starts_with("config/");
}

} // namespace

// The script's config directory (mq.configDir): the shared one for a script that may do everything; a folder of its
// own under it (config/<name>/) for any other, so an add-on cannot rewrite or read the settings another script (a
// server's, the player's) keeps there. mq.configDir .. '/MyScript.lua', MQ's usual path, works either way.
fs::path script_config_dir(const Script& s) {
    const fs::path& base = s.host->env.config_dir;
    if (base.empty() || s.caps == plugin::kCapAll) return base;
    std::string folder = s.name;
    for (char& c : folder)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    if (folder.empty() || folder.find_first_not_of('.') == std::string::npos) folder = "_";
    return base / folder;
}

namespace {

// A path inside the script's config directory (relative paths are relative to it); nullopt outside it.
std::optional<fs::path> config_path(lua_State* L, const char* given) {
    const fs::path base = script_config_dir(Script::of(L));
    if (base.empty()) return std::nullopt;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(base, ec);
    fs::path p = fs::path(given);
    if (p.is_relative()) p = root / p;
    p = fs::weakly_canonical(p, ec);
    auto rel = p.lexically_relative(root);
    if (ec || rel.empty() || *rel.begin() == "..") return std::nullopt;
    return p;
}

// mq._write_config(path, text): mq.pickle's file write.
int l_write_config(lua_State* L) {
    const char* given = luaL_checkstring(L, 1);
    std::size_t n = 0;
    const char* text = luaL_checklstring(L, 2, &n);
    int problem = 0; // 1 outside, 2 cannot write
    {
        auto p = config_path(L, given);
        if (!p) {
            problem = 1;
        } else {
            std::error_code ec;
            fs::create_directories(p->parent_path(), ec);
            std::ofstream f(*p, std::ios::binary | std::ios::trunc);
            if (f) f.write(text, static_cast<std::streamsize>(n));
            else problem = 2;
        }
    }
    if (problem == 1) return luaL_error(L, "mq.pickle: '%s' is outside the config directory", given);
    if (problem == 2) return luaL_error(L, "mq.pickle: cannot write '%s'", given);
    return 0;
}

// loadfile(path): a data file under the config directory (mq.pickle's), as text, with an empty environment.
int l_loadfile(lua_State* L) {
    auto p = config_path(L, luaL_checkstring(L, 1));
    if (!p) {
        lua_pushnil(L);
        lua_pushfstring(L, "'%s' is outside the config directory", lua_tostring(L, 1));
        return 2;
    }
    auto text = read_file(*p);
    if (!text) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", p->string().c_str());
        return 2;
    }
    std::string chunk = "@" + p->string();
    if (luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    lua_newtable(L);
    lua_setupvalue(L, -2, 1); // _ENV: nothing to call
    return 1;
}

int l_dofile(lua_State* L) {
    lua_settop(L, 1);
    l_loadfile(L);
    if (lua_isnil(L, -2)) return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

// require(name): mq, ImGui, or a module file next to the script or in the script directories; every file must be
// allowed by the server's rules like the script. find_module leaves the module's chunk on the stack (1), the cached or
// built-in value (2), or an error message (0); C++ objects are gone before anything can raise an error.
int find_module(lua_State* L, const char* given) {
    Script& s = Script::of(L);
    std::string name = given;
    lua_getfield(L, LUA_REGISTRYINDEX, "nmsc.loaded");
    lua_getfield(L, -1, given);
    lua_remove(L, -2);
    if (!lua_isnil(L, -1)) return 2;
    lua_pop(L, 1);
    if (name == "mq") {
        lua_getfield(L, LUA_REGISTRYINDEX, "nmsc.mq");
        return 2;
    }
    if (name == "ImGui") {
        lua_getglobal(L, "ImGui");
        return 2;
    }
    if (name == "hailstone") {
        lua_getglobal(L, "hailstone");
        return 2;
    }
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    // Inside the folders only: no "..", nothing absolute or rooted (operator/ would take it in place of the folder).
    if (const fs::path rel_path(rel); rel.find("..") == std::string::npos && !rel.empty() && rel.front() != '/' &&
                                   rel.front() != '\\' && !rel_path.has_root_name() && !rel_path.is_absolute()) {
        std::vector<fs::path> roots{s.root};
        for (const auto& d : s.host->dirs()) roots.push_back(d);
        for (const auto& r : roots) {
            for (const fs::path& cand : {r / (rel + ".lua"), r / rel / "init.lua"}) {
                const auto& dirs = s.host->dirs();
                if (std::any_of(dirs.begin(), dirs.end(), [&](const fs::path& d) { return in_config_dir(d, cand); }))
                    continue;
                auto text = read_file(cand);
                if (!text) continue;
                Sha256Digest h = sha256(*text);
                const std::uint32_t module_caps =
                    s.host->env.policy ? s.host->env.policy->caps(plugin::FileKind::Script, h) : 0;
                if (module_caps == 0) {
                    std::string msg = std::format("module '{}' ({}) may not run: {}", name, cand.string(),
                                                  s.host->env.policy ? s.host->env.policy->refusal(plugin::FileKind::Script)
                                                                     : std::string("no rules"));
                    lua_pushlstring(L, msg.data(), msg.size());
                    return 0;
                }
                // The module runs in this script's state, with this script's capabilities: its own file must be
                // allowed every one of them. Otherwise an add-on with fewer (an add-on named "mq" shipping
                // mq/Icons.lua) would have its code run with the capabilities of any script that requires it.
                if ((module_caps & s.caps) != s.caps) {
                    std::string msg = std::format(
                        "module '{}' ({}) may not run in {}: the module may do only '{}', the script '{}'", name,
                        cand.string(), s.name, plugin::caps_text(module_caps), plugin::caps_text(s.caps));
                    lua_pushlstring(L, msg.data(), msg.size());
                    return 0;
                }
                std::string chunk = "@" + cand.string();
                return luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") == LUA_OK ? 1 : 0;
            }
        }
    }
    std::string msg = "module '" + name + "' not found";
    lua_pushlstring(L, msg.data(), msg.size());
    return 0;
}

int l_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_settop(L, 1);
    int found = find_module(L, name);
    if (found == 0) return lua_error(L);
    if (found == 2) return 1;
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1); // the module's chunk, given its name
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "nmsc.loaded");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 1;
}

// The Lua half of the mq module and MQ's ImGui table over the client's `ui` (ui/lua/lua_imgui.cpp).
constexpr const char* kPrelude = R"lua(
local mq = ...
local yield = coroutine.yield

function mq.delay(t, cond)
    local ms = mq._delay_ms(t)
    if mq._dry(ms) then return end
    if not coroutine.isyieldable() then
        error('mq.delay: this server lets a social run only on the key press, without waiting', 2)
    end
    local stop = mq.gettime() + ms
    repeat
        yield()
    until mq.gettime() >= stop or (cond ~= nil and cond())
end

function mq.doevents(name)
    while true do
        local fn, args = mq._next_event(name)
        if not fn then return end
        fn(table.unpack(args))
    end
end

local function join(...)
    local n = select('#', ...)
    local t = {}
    for i = 1, n do t[i] = tostring((select(i, ...))) end
    return table.concat(t, ' ')
end

mq.cmd = setmetatable({}, {
    __call = function(_, ...) mq._cmd(join(...)) end,
    __index = function(_, name)
        return function(...) mq._cmd('/' .. name .. (select('#', ...) > 0 and (' ' .. join(...)) or '')) end
    end,
})
function mq.cmdf(fmt, ...) mq._cmd(string.format(fmt, ...)) end

function mq.getFilteredSpawns(pred)
    local out = {}
    for _, s in ipairs(mq.getAllSpawns()) do
        if pred == nil or pred(s) then out[#out + 1] = s end
    end
    return out
end

local function pickle_value(v, indent)
    local t = type(v)
    if t == 'string' then return string.format('%q', v) end
    if t == 'number' or t == 'boolean' or t == 'nil' then return tostring(v) end
    if t ~= 'table' then return 'nil' end
    local pad = string.rep('    ', indent + 1)
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(keys) do
        local key = type(k) == 'string' and string.format('[%q]', k) or ('[' .. tostring(k) .. ']')
        parts[#parts + 1] = pad .. key .. ' = ' .. pickle_value(v[k], indent + 1)
    end
    return '{\n' .. table.concat(parts, ',\n') .. '\n' .. string.rep('    ', indent) .. '}'
end
function mq.pickle(path, tbl) mq._write_config(path, 'return ' .. pickle_value(tbl, 0) .. '\n') end

function printf(fmt, ...) print(string.format(fmt, ...)) end

-- MQ's ImGui table: the client's ui functions with MQ's argument and return orders.
ImGui = setmetatable({}, {__index = ui})
function ImGui.Begin(name, open, flags)
    if open == nil then
        local visible = ui.Begin(name, nil, flags or 0)
        return visible
    end
    local visible, still_open = ui.Begin(name, open, flags or 0)
    return still_open, visible
end
local function fmt_text(fmt, ...)
    if select('#', ...) > 0 then return string.format(fmt, ...) end
    return tostring(fmt)
end
function ImGui.Text(fmt, ...) ui.Text(fmt_text(fmt, ...)) end
function ImGui.TextWrapped(fmt, ...) ui.TextWrapped(fmt_text(fmt, ...)) end
function ImGui.TextDisabled(fmt, ...) ui.TextDisabled(fmt_text(fmt, ...)) end
function ImGui.BulletText(fmt, ...) ui.BulletText(fmt_text(fmt, ...)) end
function ImGui.TextColored(a, b, c, d, e, ...)
    if type(a) == 'table' then
        ui.TextColored(a.x or a[1], a.y or a[2], a.z or a[3], a.w or a[4], fmt_text(b, c, d, e, ...))
    else
        ui.TextColored(a, b, c, d, fmt_text(e, ...))
    end
end
function ImGui.Button(label, w, h)
    if type(w) == 'table' then return ui.Button(label, w.x or w[1], w.y or w[2]) end
    return ui.Button(label, w or 0, h or 0)
end
function ImGui.SetNextWindowSize(w, h, cond)
    if type(w) == 'table' then return ui.SetNextWindowSize(w.x or w[1], w.y or w[2], h) end
    return ui.SetNextWindowSize(w, h, cond)
end
function ImGui.SetNextWindowPos(x, y, cond)
    if type(x) == 'table' then return ui.SetNextWindowPos(x.x or x[1], x.y or x[2], y) end
    return ui.SetNextWindowPos(x, y, cond)
end
function ImVec2(x, y) return {x = x or 0, y = y or 0} end
function ImVec4(x, y, z, w) return {x = x or 0, y = y or 0, z = z or 0, w = w or 0} end
ImGuiWindowFlags = ui.WindowFlags
ImGuiCond = ui.Cond
ImGuiCol = ui.Col
ImGuiStyleVar = ui.StyleVar
ImGuiTableFlags = ui.TableFlags
ImGuiTableColumnFlags = ui.TableColumnFlags
ImGuiInputTextFlags = ui.InputTextFlags
ImGuiComboFlags = ui.ComboFlags
ImGuiSelectableFlags = ui.SelectableFlags
ImGuiMouseButton = ui.Mouse
ImGuiHoveredFlags = ui.HoveredFlags
)lua";

} // namespace

// ---------------------------------------------------------------- the host

std::optional<std::string> script_file_hash(const fs::path& file) {
    auto text = read_file(file);
    if (!text) return std::nullopt;
    return to_hex(sha256(*text));
}

std::optional<fs::path> ScriptHost::Impl::resolve(const std::string& name) const {
    if (name.empty() || name.find("..") != std::string::npos) return std::nullopt;
    // A name is a path inside the script folders: never absolute or rooted ("/x", "C:x", "\\x"), which
    // operator/ would take in place of the folder.
    const fs::path given(name);
    if (given.is_absolute() || given.has_root_name() || given.has_root_directory() || name.front() == '/' ||
        name.front() == '\\')
        return std::nullopt;
    std::string base = name;
    if (base.size() > 4 && iequals(std::string_view(base).substr(base.size() - 4), ".lua")) base.resize(base.size() - 4);
    for (const auto& d : dirs()) {
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(d, ec);
        if (ec) continue;
        for (const fs::path& cand : {d / (base + ".lua"), d / base / "init.lua"}) {
            if (in_config_dir(d, cand) || !fs::is_regular_file(cand, ec)) continue;
            // It must stay in the folder once links are followed.
            const fs::path rel = fs::weakly_canonical(cand, ec).lexically_relative(root);
            if (ec || rel.empty() || *rel.begin() == "..") continue;
            return cand;
        }
    }
    return std::nullopt;
}

ScriptHost::ScriptHost(ScriptEnv env) : impl_(std::make_unique<Impl>()) {
    impl_->env = std::move(env);
    impl_->self = this;
    Impl* m = impl_.get();
    m->owner = m->env.hooks->new_owner("lua");
    m->env.hooks->add_command(m->owner, "/lua", [m](std::string_view args) { m->lua_command(args); },
                              plugin::kCapCommands);
}

ScriptHost::~ScriptHost() {
    for (auto& s : impl_->scripts) s->close();
    for (auto& [social, s] : impl_->buttons) s->close();
    if (impl_->env.hooks) impl_->env.hooks->remove(impl_->owner);
}

bool ScriptHost::Impl::open_state(Script& sc, std::string& why) {
    sc.L = lua_newstate(limited_alloc, &sc);
    if (!sc.L) {
        why = "out of memory";
        return false;
    }
    lua_State* L = sc.L;
    *static_cast<Script**>(lua_getextraspace(L)) = &sc;
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_settop(L, 0);
    // No code from strings or files beyond allowed modules; os only for the clock.
    for (const char* drop : {"load", "loadstring"}) {
        lua_pushnil(L);
        lua_setglobal(L, drop);
    }
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_getglobal(L, "os");
    lua_newtable(L);
    for (const char* keep : {"clock", "time", "date", "difftime"}) {
        lua_getfield(L, -2, keep);
        lua_setfield(L, -2, keep);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"print", l_print}, {"require", l_require}, {"loadfile", l_loadfile}, {"dofile", l_dofile}}) {
        lua_pushcfunction(L, fn);
        lua_setglobal(L, n);
    }
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "nmsc.loaded");
    ui::lua::register_ui_api(L);
    // ui.GetClipboardText / SetClipboardText (ui/lua/lua_code_editor.cpp): the player's clipboard, only for a script
    // that may do everything (asked at each call: the rules may change while it runs).
    lua_pushcfunction(L, [](lua_State* l) -> int {
        lua_pushboolean(l, Script::of(l).caps == plugin::kCapAll);
        return 1;
    });
    lua_setfield(L, LUA_REGISTRYINDEX, "nmsc.clipboard_allowed");

    luaL_newmetatable(L, kTloMeta);
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"__gc", tlo_gc}, {"__index", tlo_index}, {"__call", tlo_call}, {"__tostring", tlo_tostring},
             {"__concat", tlo_concat}, {"__eq", tlo_eq}, {"__lt", tlo_lt}, {"__le", tlo_le}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    lua_pop(L, 1);

    lua_newtable(L); // mq
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"_cmd", l_cmd}, {"gettime", l_gettime}, {"_delay_ms", l_delay_ms}, {"parse", l_parse},
             {"exit", l_exit}, {"event", l_event}, {"unevent", l_unevent}, {"flushevents", l_flushevents},
             {"_next_event", l_next_event}, {"bind", l_bind}, {"unbind", l_unbind},
             {"getAllSpawns", l_get_all_spawns}, {"_write_config", l_write_config}, {"_dry", l_dry}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    lua_newtable(L); // mq.TLO
    lua_newtable(L);
    lua_pushcfunction(L, tlo_root_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "TLO");
    lua_newtable(L); // mq.imgui
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"init", l_imgui_init}, {"destroy", l_imgui_destroy}, {"exists", l_imgui_exists}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    lua_setfield(L, -2, "imgui");
    push_web_module(L); // mq.web (mq/lua_mq_web.cpp)
    lua_setfield(L, -2, "web");
    // The folder the script was found in (the server's or the player's).
    std::string lua_dir = env.dirs.empty() ? std::string() : env.dirs.front().string();
    for (const auto& d : dirs()) {
        fs::path rel = sc.entry.lexically_relative(d);
        if (!sc.entry.empty() && !rel.empty() && *rel.begin() != "..") {
            lua_dir = d.string();
            break;
        }
    }
    lua_pushstring(L, lua_dir.c_str());
    lua_setfield(L, -2, "luaDir");
    lua_pushstring(L, script_config_dir(sc).string().c_str());
    lua_setfield(L, -2, "configDir");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "nmsc.mq");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "mq");
    register_hailstone(L);
    if (luaL_loadbufferx(L, kPrelude, std::strlen(kPrelude), "=mq", "t") != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        why = "internal error: " + err;
        return false;
    }
    lua_insert(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        why = "internal error: " + err;
        return false;
    }
    return true;
}

bool ScriptHost::run(const std::string& name, const std::vector<std::string>& args, std::string* why) {
    Impl& m = *impl_;
    auto refuse = [&](std::string text) {
        if (why) *why = text;
        m.say("[Lua] " + text);
        return false;
    };
    m.reap();
    std::string key = lower(name);
    if (m.blocked.contains(key)) return refuse(key + " is turned off in Options");
    if (Script* old = m.find(key); old && !old->stop_requested) return refuse(key + " is already running");
    auto entry = m.resolve(key);
    if (!entry) return refuse("no script named " + name);
    auto text = read_file(*entry);
    if (!text) return refuse("cannot read " + entry->string());
    Sha256Digest hash = sha256(*text);
    if (!m.env.policy) return refuse("scripts are off");
    std::uint32_t caps = m.env.policy->caps(plugin::FileKind::Script, hash);
    if (caps == 0) return refuse(std::format("{} may not run: {}", key, m.env.policy->refusal(plugin::FileKind::Script)));
    // A script another started (mq.cmd("/lua run"), a plugin, a plugin's window) gets no more than its starter may
    // do now; the player's own start gives it all its file allows.
    caps &= plugin::caller_caps();
    if (caps == 0)
        return refuse(std::format("{} may not run: {} may not give it anything it would do", key,
                                  plugin::caller_name()));

    auto s = std::make_unique<Script>();
    Script& sc = *s;
    sc.host = &m;
    sc.name = key;
    sc.entry = *entry;
    sc.root = entry->parent_path();
    sc.hash = hash;
    sc.caps = caps;
    sc.args = args;
    sc.owner = m.env.hooks->new_guest("lua:" + key);
    if (std::string err; !m.open_state(sc, err)) return refuse(err);
    lua_State* L = sc.L;

    std::string chunk = "@" + entry->string();
    if (luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        return refuse(key + ": " + err);
    }
    sc.co = lua_newthread(L);
    lua_insert(L, -2);
    lua_xmove(L, sc.co, 1); // the chunk onto the coroutine
    sc.co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    watch(sc.co);
    m.say(std::format("[Lua] running {} ({})", key, plugin::caps_text(caps)));
    log::info("lua: running {} from {}, sha256 {}, caps {}", key, entry->string(), to_hex(hash), plugin::caps_text(caps));
    m.scripts.push_back(std::move(s));
    return true;
}

bool ScriptHost::Impl::kept(const Script& s) const {
    return env.policy && env.policy->current() && env.policy->current()->configured &&
           env.policy->server_file(plugin::FileKind::Script, s.hash);
}

void ScriptHost::Impl::end(Script& s, const std::string& why) {
    s.stop_requested = true;
    if (s.status == Script::Status::Running || s.status == Script::Status::Paused) s.status = Script::Status::Exited;
    say("[Lua] " + s.name + " " + why);
}

void ScriptHost::set_blocked(std::string_view name, bool blocked) {
    std::string key = lower(name);
    if (!blocked) {
        impl_->blocked.erase(key);
        return;
    }
    impl_->blocked.insert(key);
    if (Script* s = impl_->find(key); s && !s->stop_requested) {
        impl_->say("[Lua] " + key + " is turned off in Options: stopping it");
        impl_->end(*s, "turned off in Options");
        impl_->reap();
    }
}

bool ScriptHost::blocked(std::string_view name) const { return impl_->blocked.contains(lower(name)); }

bool ScriptHost::stop(std::string_view name) {
    Script* s = impl_->find(lower(name));
    if (!s || s->stop_requested) return false;
    if (impl_->kept(*s)) {
        impl_->say("[Lua] " + s->name + " is this server's own script: it keeps running");
        return true;
    }
    impl_->end(*s, "stopped");
    impl_->reap();
    return true;
}

void ScriptHost::stop_all() {
    for (auto& s : impl_->scripts)
        if (!s->stop_requested) impl_->end(*s, "stopped");
    impl_->reap();
}

void ScriptHost::set_server_dir(const fs::path& dir) { impl_->server_dir = dir; }

bool ScriptHost::pause(std::string_view name, std::optional<bool> paused) {
    Script* s = impl_->find(lower(name));
    if (!s || s->stop_requested) return false;
    if (impl_->kept(*s)) {
        impl_->say("[Lua] " + s->name + " is this server's own script: it keeps running");
        return true;
    }
    bool now_paused = paused ? *paused : s->status != Script::Status::Paused;
    s->status = now_paused ? Script::Status::Paused : Script::Status::Running;
    impl_->say(std::format("[Lua] {} {}", s->name, now_paused ? "paused" : "resumed"));
    return true;
}

std::vector<ScriptHost::Info> ScriptHost::list() const {
    std::vector<Info> out;
    for (const auto& s : impl_->scripts) {
        Info i;
        i.name = s->name;
        switch (s->status) {
        case Script::Status::Running: i.status = "RUNNING"; break;
        case Script::Status::Paused: i.status = "PAUSED"; break;
        case Script::Status::Exited: i.status = "EXITED"; break;
        case Script::Status::Failed: i.status = "ERROR"; break;
        }
        i.caps = s->caps;
        i.sha256 = to_hex(s->hash);
        i.error = s->error;
        i.kept = impl_->kept(*s);
        out.push_back(std::move(i));
    }
    return out;
}

bool ScriptHost::running(std::string_view name) const {
    const Script* s = impl_->find(lower(name));
    return s && !s->stop_requested && (s->status == Script::Status::Running || s->status == Script::Status::Paused);
}

void ScriptHost::Impl::reap() {
    for (auto it = buttons.begin(); it != buttons.end();) {
        Script& s = *it->second;
        if (s.stop_requested && s.busy == 0) {
            s.close();
            it = buttons.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = scripts.begin(); it != scripts.end();) {
        Script& s = **it;
        if (s.stop_requested && s.busy == 0) {
            if (s.status == Script::Status::Running || s.status == Script::Status::Paused) s.status = Script::Status::Exited;
            s.close();
            it = scripts.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptHost::pulse() {
    Impl& m = *impl_;
    for (std::size_t i = 0; i < m.scripts.size(); ++i) {
        Script& s = *m.scripts[i];
        if (s.stop_requested || s.status != Script::Status::Running || !s.co) continue;
        if (s.main_done) { // a plugin whose main chunk returned: only its binds' threads
            ++s.busy;
            plugin::CallerScope scope(s.caps, s.name);
            m.resume_threads(s);
            --s.busy;
            continue;
        }
        ++s.busy;
        plugin::CallerScope scope(s.caps, s.name);
        s.slice_start = Clock::now();
        int nargs = 0;
        if (!s.started) {
            s.started = true;
            for (const auto& a : s.args) lua_pushlstring(s.co, a.data(), a.size());
            nargs = static_cast<int>(s.args.size());
        }
        int nres = 0;
        int st = lua_resume(s.co, nullptr, nargs, &nres);
        if (st == LUA_YIELD) {
            lua_pop(s.co, nres);
        } else if (st == LUA_OK && s.registered) {
            s.main_done = true; // a plugin: it stays for what it gave the client (hooks, packets, windows)
        } else if (st == LUA_OK) {
            s.stop_requested = true; // the script's main chunk returned
            s.status = Script::Status::Exited;
            m.say("[Lua] " + s.name + " ended");
        } else if (lua_touserdata(s.co, -1) == &kExitKey) {
            s.stop_requested = true;
            s.status = Script::Status::Exited;
            m.say("[Lua] " + s.name + " ended");
        } else {
            const char* err = lua_tostring(s.co, -1);
            luaL_traceback(s.L, s.co, err ? err : "error", 0);
            std::string trace = lua_tostring(s.L, -1);
            lua_pop(s.L, 1);
            s.fail(trace.substr(0, trace.find('\n')));
            log::warn("lua script {}: {}", s.name, trace);
        }
        m.resume_threads(s);
        --s.busy;
    }
    for (auto& [social, sp] : m.buttons) {
        Script& s = *sp;
        if (s.stop_requested || s.threads.empty()) continue;
        ++s.busy;
        plugin::CallerScope scope(s.caps, s.name);
        m.resume_threads(s);
        --s.busy;
    }
    m.reap();
}

// The binds (and a Lua social's presses) still running.
void ScriptHost::Impl::resume_threads(Script& s) {
    for (std::size_t t = 0; t < s.threads.size() && !s.stop_requested;) {
        auto [thread, ref] = s.threads[t];
        s.slice_start = Clock::now();
        int tn = 0;
        int ts = lua_resume(thread, nullptr, 0, &tn);
        if (ts == LUA_YIELD) {
            lua_pop(thread, tn);
            ++t;
            continue;
        }
        if (ts != LUA_OK) {
            if (lua_touserdata(thread, -1) == &kExitKey) {
                if (s.social < 0) s.stop_requested = true;
            } else {
                s.fail(lua_tostring(thread, -1) ? lua_tostring(thread, -1) : "error");
            }
        }
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
        s.threads.erase(s.threads.begin() + static_cast<std::ptrdiff_t>(t));
    }
}

void ScriptHost::imgui() {
    Impl& m = *impl_;
    for (auto& sp : m.scripts) m.draw_windows(*sp);
    for (auto& [social, sp] : m.buttons) m.draw_windows(*sp);
    m.reap();
}

void ScriptHost::Impl::draw_windows(Script& s) {
    if (s.stop_requested || s.windows.empty() || (s.caps & plugin::kCapUi) == 0) return;
    ++s.busy;
    plugin::CallerScope scope(s.caps, s.name);
    for (std::size_t i = 0; i < s.windows.size() && !s.stop_requested; ++i) {
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.windows[i].second);
        s.slice_start = Clock::now();
        if (lua_pcall(s.L, 0, 0, 0) != LUA_OK) {
            ImGui::ErrorRecoveryTryToRecoverState(&state);
            if (lua_touserdata(s.L, -1) == &kExitKey) {
                if (s.social < 0) s.stop_requested = true;
                else stop_threads(s);
            } else {
                s.fail(std::string("window ") + s.windows[i].first + ": " + (lua_tostring(s.L, -1) ? lua_tostring(s.L, -1) : "error"));
            }
            lua_pop(s.L, 1);
        }
    }
    --s.busy;
}

void ScriptHost::policy_changed(const std::vector<std::string>& autoload) {
    Impl& m = *impl_;
    for (auto& sp : m.scripts) {
        Script& s = *sp;
        if (s.stop_requested) continue;
        std::uint32_t caps = m.env.policy ? m.env.policy->caps(plugin::FileKind::Script, s.hash) : 0;
        if (caps == 0) {
            m.end(s, "stopped: it may not run here");
        } else if (caps != s.caps) {
            s.caps = caps;
            m.say(std::format("[Lua] {} now may: {}", s.name, plugin::caps_text(caps)));
        }
    }
    plugin::ButtonRules r = m.env.policy ? m.env.policy->button_rules() : plugin::ButtonRules{};
    for (auto& [social, sp] : m.buttons) {
        Script& s = *sp;
        if (r.mode != plugin::ButtonMode::Full) {
            if (button_running(social)) m.say("[Lua] " + s.name + " stopped: this server does not let it run on");
            m.stop_threads(s);
        } else {
            s.caps = r.caps;
        }
    }
    m.reap();
    // The server's scripts its hailstone.cfg starts ([scripts] run).
    if (m.env.policy && m.env.policy->current())
        for (const auto& name : m.env.policy->current()->run_scripts)
            if (!running(name)) run(name, {});
    // The player's own the settings and the character's autorun list name.
    m.start_allowed(autoload);
    m.start_allowed(m.autorun);
}

void ScriptHost::Impl::start_allowed(const std::vector<std::string>& names) {
    if (!env.policy || !env.policy->received()) return;
    for (const auto& name : names) {
        if (name.empty() || self->running(name)) continue;
        auto entry = resolve(lower(name));
        auto text = entry ? read_file(*entry) : std::nullopt;
        if (text && env.policy->caps(plugin::FileKind::Script, sha256(*text)) != 0) self->run(name, {});
    }
}

std::optional<Sha256Digest> ScriptHost::Impl::file_hash(const fs::path& file) const {
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    auto time = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    if (auto it = hashes.find(file); it != hashes.end() && it->second.size == size && it->second.time == time)
        return it->second.hash;
    auto text = read_file(file);
    if (!text) return std::nullopt;
    HashEntry& e = hashes[file];
    e = {size, time, sha256(*text)};
    return e.hash;
}

std::vector<ScriptHost::Found> ScriptHost::found() const {
    const Impl& m = *impl_;
    std::vector<Found> out;
    const auto dirs = m.dirs();
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        std::error_code ec;
        std::vector<std::pair<std::string, fs::path>> here;
        for (fs::directory_iterator it(dirs[d], ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            std::error_code fec;
            if (it->is_directory(fec)) {
                // mq.configDir's settings, not a script
                if (windows_file_key(p.filename().generic_string()) == "config") continue;
                if (fs::is_regular_file(p / "init.lua", fec)) here.emplace_back(lower(p.filename().string()), p / "init.lua");
            } else if (it->is_regular_file(fec) && lower(p.extension().string()) == ".lua") {
                here.emplace_back(lower(p.stem().string()), p);
            }
        }
        // <name>.lua before <name>/init.lua, as resolve() looks
        std::sort(here.begin(), here.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second.filename() != "init.lua";
        });
        for (const auto& candidate : here) { // (no structured binding: Apple Clang's lambdas cannot capture one)
            const std::string& name = candidate.first;
            const fs::path& entry = candidate.second;
            if (name.empty() || name.find("..") != std::string::npos) continue;
            if (std::any_of(out.begin(), out.end(), [&name](const Found& f) { return f.name == name; })) continue;
            Found f;
            f.name = name;
            f.entry = entry;
            f.server = !m.server_dir.empty() && d == 0;
            if (!m.env.policy) {
                f.why = "scripts are off";
            } else if (auto hash = m.file_hash(entry)) {
                f.caps = m.env.policy->caps(plugin::FileKind::Script, *hash);
                if (f.caps == 0) f.why = m.env.policy->refusal(plugin::FileKind::Script);
            } else {
                f.why = "cannot read " + entry.string();
            }
            out.push_back(std::move(f));
        }
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
    return out;
}

std::vector<fs::path> ScriptHost::folders() const { return impl_->dirs(); }

void ScriptHost::set_autorun_file(const fs::path& file) {
    Impl& m = *impl_;
    if (file == m.autorun_file) return;
    m.autorun_file = file;
    m.autorun.clear();
    IniFile ini;
    if (!file.empty() && ini.load(file))
        for (const auto& key : ini.keys("Autorun"))
            if (ini.get_int("Autorun", key, 0) != 0) m.autorun.push_back(lower(key));
    m.start_allowed(m.autorun);
}

bool ScriptHost::autorun(std::string_view name) const {
    const auto& a = impl_->autorun;
    return std::find(a.begin(), a.end(), lower(name)) != a.end();
}

std::vector<std::string> ScriptHost::autorun_list() const { return impl_->autorun; }
bool ScriptHost::autorun_available() const { return !impl_->autorun_file.empty(); }

bool ScriptHost::set_autorun(std::string_view name, bool on) {
    Impl& m = *impl_;
    std::string key = lower(name);
    if (m.autorun_file.empty() || key.empty() || key.find_first_of("=[]\r\n") != std::string::npos) return false;
    IniFile ini;
    ini.load(m.autorun_file); // (a missing file: a new one)
    if (on) ini.set_int("Autorun", key, 1);
    else ini.erase("Autorun", key);
    std::error_code ec;
    fs::create_directories(m.autorun_file.parent_path(), ec);
    if (!ini.save(m.autorun_file)) return false;
    std::erase(m.autorun, key);
    if (on) m.autorun.push_back(key);
    return true;
}

void ScriptHost::Impl::lua_command(std::string_view args) {
    std::string_view rest = args;
    std::string verb = lower(game::next_cmd_arg(rest));
    std::vector<std::string> words;
    while (true) {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) break;
        words.push_back(game::next_cmd_arg(rest));
    }
    if (verb == "run" && !words.empty()) {
        std::string name = words.front();
        words.erase(words.begin());
        self->run(name, words);
        return;
    }
    if (verb == "stop") {
        if (words.empty()) {
            for (auto& s : scripts)
                if (!s->stop_requested && !kept(*s)) end(*s, "stopped");
            reap();
        } else if (!self->stop(words.front())) say("[Lua] no running script named " + words.front());
        return;
    }
    if (verb == "pause") {
        if (words.empty()) {
            std::vector<std::string> names;
            for (auto& s : scripts)
                if (!s->stop_requested && !kept(*s)) names.push_back(s->name);
            for (const auto& n : names) self->pause(n);
        } else if (!self->pause(words.front())) {
            say("[Lua] no running script named " + words.front());
        }
        return;
    }
    if (verb == "list" || verb == "ps" || verb == "info") {
        reap();
        if (scripts.empty()) say("[Lua] no scripts are running");
        for (const auto& s : scripts) {
            std::string st = s->status == Script::Status::Paused ? "paused" : "running";
            say(std::format("[Lua] {} ({}) - {}", s->name, st, plugin::caps_text(s->caps)));
        }
        return;
    }
    say("[Lua] usage: /lua run <script> [args] | stop [script] | pause [script] | list");
}

} // namespace nmsc::mq

// ---------------------------------------------------------------- Lua socials

namespace nmsc::mq {

namespace {

std::string button_name(const std::string& name) { return "social '" + name + "'"; }

// The `button` table the social's Lua sees.
void set_button(Script& s, const ButtonPress& p) {
    lua_State* L = s.L;
    lua_getglobal(L, "button");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "button");
    }
    lua_pushinteger(L, p.social + 1);
    lua_setfield(L, -2, "social");
    if (p.bar >= 0) lua_pushinteger(L, p.bar + 1);
    else lua_pushnil(L);
    lua_setfield(L, -2, "bar");
    if (p.button >= 0) lua_pushinteger(L, p.button + 1);
    else lua_pushnil(L);
    lua_setfield(L, -2, "button");
    lua_pushboolean(L, p.shift);
    lua_setfield(L, -2, "shift");
    lua_pushboolean(L, p.ctrl);
    lua_setfield(L, -2, "ctrl");
    lua_pushboolean(L, p.alt);
    lua_setfield(L, -2, "alt");
    lua_pushinteger(L, static_cast<lua_Integer>(s.presses));
    lua_setfield(L, -2, "presses");
    lua_pop(L, 1);
}

// The value of `key` in the table at the top (raw: no metamethods run).
int raw_field(lua_State* L, const char* key) {
    lua_pushstring(L, key);
    return lua_rawget(L, -2);
}

std::string string_field(lua_State* L, const char* key) {
    std::string out;
    int t = raw_field(L, key);
    if (t == LUA_TSTRING || t == LUA_TNUMBER) {
        std::size_t n = 0;
        const char* p = lua_tolstring(L, -1, &n);
        out.assign(p, n);
    }
    lua_pop(L, 1);
    return out;
}

ButtonFace read_face(lua_State* L) {
    ButtonFace f;
    if (lua_type(L, -1) == LUA_TSTRING) {
        f.has = true;
        f.label = lua_tostring(L, -1);
        return f;
    }
    if (!lua_istable(L, -1)) return f;
    f.has = true;
    f.label = string_field(L, "label");
    f.overlay = string_field(L, "overlay");
    f.tooltip = string_field(L, "tooltip");
    if (raw_field(L, "color") == LUA_TNUMBER) {
        f.color = static_cast<std::uint32_t>(lua_tointeger(L, -1)) & 0xffffff;
    } else if (lua_istable(L, -1)) {
        std::uint32_t rgb = 0;
        for (int i = 1; i <= 3; ++i) {
            lua_rawgeti(L, -1, i);
            double v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (v <= 1.0) v *= 255.0;
            rgb = rgb << 8 | static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0));
        }
        f.color = rgb;
    }
    lua_pop(L, 1);
    if (raw_field(L, "cooldown") == LUA_TNUMBER) f.cooldown = std::clamp(static_cast<float>(lua_tonumber(L, -1)), 0.0f, 1.0f);
    lua_pop(L, 1);
    if (raw_field(L, "enabled") != LUA_TNIL) f.enabled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return f;
}

} // namespace

plugin::ButtonRules ScriptHost::Impl::rules(const game::LuaSocial&) const {
    return env.policy ? env.policy->button_rules() : plugin::ButtonRules{};
}

// The social's state, made again when its code changed; null with the reason (a syntax error).
Script* ScriptHost::Impl::button(const ButtonPress& p, std::string& why) {
    Sha256Digest hash = game::lua_social_hash(p.lua);
    const int key = p.state >= 0 ? p.state : p.social;
    if (auto b = broken.find(key); b != broken.end()) {
        if (b->second.first == hash) {
            why = b->second.second;
            return nullptr;
        }
        broken.erase(b);
    }
    if (auto it = buttons.find(key); it != buttons.end()) {
        Script& old = *it->second;
        if (old.hash == hash && !old.stop_requested && old.L) {
            old.name = button_name(p.name);
            return &old;
        }
        if (old.busy > 0) {
            why = "it is running";
            return nullptr;
        }
        old.close();
        buttons.erase(it);
    }
    auto sp = std::make_unique<Script>();
    Script& sc = *sp;
    sc.host = this;
    sc.social = p.social;
    sc.name = button_name(p.name);
    sc.hash = hash;
    if (!env.dirs.empty()) sc.root = env.dirs.front();
    sc.owner = env.hooks->new_guest(std::format("social:{}", p.social + 1));
    if (!open_state(sc, why)) {
        env.hooks->remove(sc.owner);
        return nullptr;
    }
    lua_State* L = sc.L;
    watch(L);
    lua_newtable(L);
    lua_setglobal(L, "state");
    auto compile = [&](const std::string& code, const char* part, int& ref) {
        if (code.empty()) return true;
        std::string chunk = std::format("={} ({})", sc.name, part);
        if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) {
            why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
            lua_pop(L, 1);
            return false;
        }
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return true;
    };
    if (!compile(p.lua.press, "press", sc.press_ref) || !compile(p.lua.face, "face", sc.face_ref)) {
        sc.close();
        broken[key] = {hash, why};
        return nullptr;
    }
    Script* out = &sc;
    buttons[key] = std::move(sp);
    return out;
}

void ScriptHost::Impl::stop_threads(Script& s) {
    if (!s.L) return;
    for (auto& [thread, ref] : s.threads) luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
    s.threads.clear();
    for (auto& e : s.events) luaL_unref(s.L, LUA_REGISTRYINDEX, e.fn);
    s.events.clear();
    s.queued.clear();
    for (auto& [cmd, ref] : s.binds) {
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
        env.hooks->remove_command(s.owner, cmd);
    }
    s.binds.clear();
    for (auto& [name, ref] : s.windows) luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
    s.windows.clear();
}

plugin::ButtonRules ScriptHost::button_rules(const game::LuaSocial& lua) const { return impl_->rules(lua); }

bool ScriptHost::button_running(int social) const {
    auto it = impl_->buttons.find(social);
    if (it == impl_->buttons.end()) return false;
    const Script& s = *it->second;
    return !s.stop_requested && (!s.threads.empty() || !s.events.empty() || !s.binds.empty() || !s.windows.empty());
}

bool ScriptHost::stop_button(int social) {
    if (!button_running(social)) return false;
    Script& s = *impl_->buttons[social];
    if (s.busy > 0) {
        // Stopped from inside its own Lua (/stopsocial): the state goes once the Lua returns.
        s.stop_requested = true;
        return true;
    }
    impl_->stop_threads(s);
    impl_->say("[Lua] " + s.name + " stopped");
    return true;
}

void ScriptHost::forget_button(int social) {
    Impl& m = *impl_;
    if (social < 0) {
        m.broken.clear();
        for (auto& [n, s] : m.buttons) s->stop_requested = true;
        m.reap();
        return;
    }
    m.broken.erase(social);
    auto it = m.buttons.find(social);
    if (it == m.buttons.end()) return;
    it->second->stop_requested = true;
    m.reap();
}

bool ScriptHost::press_button(const ButtonPress& p, std::string* why) {
    Impl& m = *impl_;
    auto refuse = [&](const std::string& text) {
        if (why) *why = text;
        m.say(std::format("[Lua] {}: {}", button_name(p.name), text));
        return false;
    };
    if (p.social < 0 || p.lua.press.empty()) return refuse("it has no Lua to run on a press");
    // Pressed again while it runs: it stops (the original's social queue does the same with a right click).
    if (stop_button(p.social)) return true;
    plugin::ButtonRules r = m.rules(p.lua);
    if (r.mode == plugin::ButtonMode::Off)
        return refuse(m.env.policy && m.env.policy->received() ? "this server does not run Lua socials"
                                                                : "Lua socials are off until the zone-in has checked the server's files");
    std::string err;
    Script* s = m.button(p, err);
    if (!s) return refuse(err);
    ++s->presses;
    set_button(*s, p);
    s->caps = r.caps;
    if (r.mode == plugin::ButtonMode::Full) {
        start_thread(*s, s->press_ref, {});
        m.reap();
        return true;
    }
    ++s->busy;
    s->press_commands = r.press_commands;
    s->commands_left = r.press_commands > 0 ? r.press_commands : -1;
    s->runaway_ms = kPressMs;
    bool ok = true;
    {
        plugin::CallerScope scope(s->caps, s->name);
        s->slice_start = Clock::now();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->press_ref);
        if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
            if (lua_touserdata(s->L, -1) != &kExitKey) {
                ok = false;
                s->fail(lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error");
            }
            lua_pop(s->L, 1);
        }
    }
    s->commands_left = -1;
    s->runaway_ms = kRunawayMs;
    --s->busy;
    m.reap();
    return ok;
}

ButtonDryRun ScriptHost::dry_run_button(const ButtonPress& p) {
    Impl& m = *impl_;
    ButtonDryRun out;
    out.rules = m.rules(p.lua);
    if (p.lua.press.empty()) {
        out.error = "it has no Lua to run on a press";
        return out;
    }
    Script* s = m.button(p, out.error);
    if (!s) return out;
    if (s->busy > 0) {
        out.error = "it is running";
        return out;
    }
    set_button(*s, p);
    const std::uint32_t caps = s->caps;
    // On a server without Lua socials the dry run may still read the game state; every command shows as blocked.
    s->caps = out.rules.mode == plugin::ButtonMode::Off ? plugin::kCapRead : out.rules.caps;
    s->press_commands = out.rules.press_commands;
    s->commands_left = out.rules.mode == plugin::ButtonMode::Press && out.rules.press_commands > 0 ? out.rules.press_commands : -1;
    s->runaway_ms = kPressMs;
    s->dry = &out.lines;
    ++s->busy;
    {
        plugin::CallerScope scope(s->caps, s->name);
        s->slice_start = Clock::now();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->press_ref);
        if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
            if (lua_touserdata(s->L, -1) != &kExitKey) out.error = lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error";
            lua_pop(s->L, 1);
        }
    }
    --s->busy;
    s->dry = nullptr;
    s->commands_left = -1;
    s->runaway_ms = kRunawayMs;
    s->caps = caps;
    out.ok = out.error.empty();
    m.reap();
    return out;
}

const ButtonFace& ScriptHost::button_face(int social, const std::string& name, const game::LuaSocial& lua) {
    static const ButtonFace none;
    Impl& m = *impl_;
    if (lua.face.empty() || !m.rules(lua).faces) return none;
    ButtonPress p;
    p.social = social;
    p.name = name;
    p.lua = lua;
    std::string err;
    Script* s = m.button(p, err);
    if (!s) return none;
    if (s->busy > 0) return s->face;
    const std::int64_t now = m.now();
    if (s->face_ran && now - s->face_at < kFaceEveryMs) return s->face;
    s->face_ran = true;
    s->face_at = now;
    const std::uint32_t caps = s->caps;
    s->caps = plugin::kCapRead;
    s->face_eval = true;
    s->runaway_ms = kFaceMs;
    ++s->busy;
    ButtonFace f;
    {
        plugin::CallerScope scope(plugin::kCapRead, s->name);
        s->slice_start = Clock::now();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->face_ref);
        if (lua_pcall(s->L, 0, 1, 0) == LUA_OK) {
            f = read_face(s->L);
        } else {
            f.error = lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error";
            if (f.error != s->face.error) m.say(std::format("[Lua] {} (face): {}", s->name, f.error));
        }
        lua_pop(s->L, 1);
    }
    --s->busy;
    s->face_eval = false;
    s->runaway_ms = kRunawayMs;
    s->caps = caps;
    s->face = std::move(f);
    return s->face;
}

} // namespace nmsc::mq
