#include "TSLuaRuntime.h"

#include "Log.h"
#include "LuaEngine.h"
#include "TSAll.h"
#include "TSEvents.h"
#include "TSLuaDatabase.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::unique_ptr<sol::state_view> Lua;
std::unique_ptr<sol::environment> Environment;
lua_State* AttachedState = nullptr;
bool ReloadRequested = false;
std::filesystem::path LuaRoot;
std::vector<std::filesystem::path> FileStack;
std::map<std::filesystem::path, sol::object> Modules;
std::map<void*, std::map<std::string, sol::object>> EntityState;

struct LuaTimer
{
    TSMap Owner;
    std::uint32_t Interval;
    std::int64_t Remaining;
    std::int32_t Repeats;
    bool Stopped = false;
    sol::protected_function Callback;

    void Stop() { Stopped = true; }
};

std::vector<std::shared_ptr<LuaTimer>> Timers;

std::filesystem::path Normalize(std::filesystem::path const& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

bool LogLuaError(sol::protected_function_result const& result)
{
    if (result.valid())
        return true;
    sol::error error = result;
    LOG_ERROR("tswow.lua", "{}", error.what());
    return false;
}

template <typename... Args>
std::function<void(Args...)> LuaCallback(sol::protected_function function)
{
    return [function = std::move(function)](Args... args) mutable
    {
        LOCK_ALE;
        LogLuaError(function(std::forward<Args>(args)...));
    };
}

template <typename Add>
void AddMapped(sol::object const& value, Add&& add)
{
    if (value.get_type() == sol::type::number)
    {
        add(static_cast<std::uint32_t>(value.as<double>()));
        return;
    }
    if (value.get_type() != sol::type::table)
        throw std::runtime_error("TSWoW mapped events require a numeric id or an array of ids");

    sol::table ids = value.as<sol::table>();
    for (std::size_t index = 1; index <= ids.size(); ++index)
        add(static_cast<std::uint32_t>(ids.get<double>(index)));
}

std::filesystem::path FindModule(std::string name)
{
    std::replace(name.begin(), name.end(), '.', '/');
    std::filesystem::path relative = name + ".lua";
    if (!FileStack.empty())
    {
        std::filesystem::path local = FileStack.back().parent_path() / relative;
        if (std::filesystem::is_regular_file(local))
            return Normalize(local);
    }

    std::filesystem::path direct = LuaRoot / relative;
    if (std::filesystem::is_regular_file(direct))
        return Normalize(direct);

    for (auto const& entry : std::filesystem::recursive_directory_iterator(LuaRoot))
    {
        if (!entry.is_regular_file())
            continue;
        std::string candidate = entry.path().generic_string();
        std::string suffix = relative.generic_string();
        if (candidate.size() >= suffix.size() &&
            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) == 0)
            return Normalize(entry.path());
    }
    return {};
}

sol::object ExecuteFile(std::filesystem::path const& input)
{
    std::filesystem::path path = Normalize(input);
    auto found = Modules.find(path);
    if (found != Modules.end())
        return found->second;
    if (std::find(FileStack.begin(), FileStack.end(), path) != FileStack.end())
        throw std::runtime_error("Circular Lua dependency involving " + path.string());

    FileStack.push_back(path);
    sol::protected_function_result result =
        Lua->safe_script_file(path.string(), *Environment, sol::script_pass_on_error);
    FileStack.pop_back();
    if (!result.valid())
    {
        LogLuaError(result);
        throw std::runtime_error("Could not load Lua module " + path.string());
    }

    sol::object value = result.get_type() == sol::type::nil
        ? sol::make_object(*Lua, Lua->create_table())
        : result.get<sol::object>();
    Modules.emplace(path, value);
    return value;
}

sol::object Require(std::string const& name)
{
    if (name == "lualib_bundle")
    {
        std::filesystem::path bundle = FindModule(name);
        if (bundle.empty())
            throw std::runtime_error("Could not find lualib_bundle.lua under " + LuaRoot.string());
        return ExecuteFile(bundle);
    }
    std::filesystem::path path = FindModule(name);
    if (path.empty())
        throw std::runtime_error("Could not find Lua module " + name + " under " + LuaRoot.string());
    return ExecuteFile(path);
}

void BindMutables(sol::state_view& lua)
{
    lua.new_usertype<TSMutable<bool, bool>>("TSMutableBool", sol::no_constructor,
        "get", &TSMutable<bool, bool>::get, "set", &TSMutable<bool, bool>::set,
        "stringify", &TSMutable<bool, bool>::stringify);
    lua.new_usertype<TSMutableString>("TSMutableString", sol::no_constructor,
        "get", &TSMutableString::get, "set", &TSMutableString::set,
        "stringify", &TSMutableString::stringify);
    lua.new_usertype<TSMutable<std::uint32_t, double>>("TSMutableUInt32", sol::no_constructor,
        "get", &TSMutable<std::uint32_t, double>::get, "set", &TSMutable<std::uint32_t, double>::set);
}

void BindObjects(sol::state_view& lua, sol::environment& environment)
{
    lua.new_usertype<TSMutex>("TSMutex", sol::constructors<TSMutex()>(),
        "lock", &TSMutex::lock, "unlock", &TSMutex::unlock, "try_lock", &TSMutex::try_lock,
        "stringify", &TSMutex::stringify);
    environment.set_function("CreateMutex", [] { return TSMutex(); });
    environment.set_function("CreateMutexLock", [] { return TSMutex(); });
    environment.set_function("CreateArray", [](sol::table table) { return table; });
    environment.set_function("CreateDictionary", [](sol::table table) { return table; });
    environment.set_function("CreateGUID", sol::overload(
        [](double high, double counter)
        { return CreateGUID(static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(counter)); },
        [](double high, double entry, double counter)
        {
            return CreateGUID(static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(entry),
                static_cast<std::uint32_t>(counter));
        }));
    environment.set_function("EmptyGUID", [] { return EmptyGUID(); });

    lua.new_usertype<TSGUID>("TSGUID", sol::no_constructor,
        "GetCounter", &TSGUID::GetCounter, "GetLow", &TSGUID::GetLow, "GetType", &TSGUID::GetType,
        "GetEntry", &TSGUID::GetEntry, "IsEmpty", &TSGUID::IsEmpty, "IsPlayer", &TSGUID::IsPlayer,
        "IsCreature", &TSGUID::IsCreature, "IsGameObject", &TSGUID::IsGameObject,
        "stringify", [](TSGUID const& guid) { return guid.stringify(); });

    lua.new_usertype<TSMap>("TSMap", sol::no_constructor,
        "IsNull", &TSMap::IsNull, "IsBG", &TSMap::IsBG, "ToBG", &TSMap::ToBG,
        "GetObject", [](TSMap const& map, std::string const& key, sol::object defaultValue)
        {
            auto& values = EntityState[map.GetNativeHandle()];
            auto found = values.find(key);
            if (found != values.end())
                return found->second;
            return values.emplace(key, std::move(defaultValue)).first->second;
        });
    lua.new_usertype<TSUnit>("TSUnit", sol::no_constructor,
        "IsNull", &TSUnit::IsNull, "IsPlayer", &TSUnit::IsPlayer, "ToPlayer", &TSUnit::ToPlayer,
        "GetEffectiveOwner", &TSUnit::GetEffectiveOwner, "GetGUIDLow", &TSUnit::GetGUIDLow,
        "GetMapID", &TSUnit::GetMapID, "GetMap", &TSUnit::GetMap);
    lua.new_usertype<TSCreature>("TSCreature", sol::no_constructor, sol::base_classes, sol::bases<TSUnit>(),
        "Respawn", &TSCreature::Respawn, "RemoveCorpse", &TSCreature::RemoveCorpse);
    lua.new_usertype<TSPlayer>("TSPlayer", sol::no_constructor, sol::base_classes, sol::bases<TSUnit>(),
        "GetGUID", &TSPlayer::GetGUID, "SendBroadcastMessage", &TSPlayer::SendBroadcastMessage,
        "GetClass", &TSPlayer::GetClass, "GetMapID", &TSPlayer::GetMapID,
        "GetQuestRewardTempTalentPoints", &TSPlayer::GetQuestRewardTempTalentPoints,
        "GetTeam", &TSPlayer::GetTeam, "GossipMenuAddItem", &TSPlayer::GossipMenuAddItem,
        "GossipComplete", &TSPlayer::GossipComplete, "GossipSendTextMenu", &TSPlayer::GossipSendTextMenu,
        "GossipClearMenu", &TSPlayer::GossipClearMenu, "SetTaxiCheat", &TSPlayer::SetTaxiCheat,
        "SendUpdateWorldState", &TSPlayer::SendUpdateWorldState, "SendAddonMessage", &TSPlayer::SendAddonMessage,
        "Teleport", &TSPlayer::Teleport,
        "GetObject", [](TSPlayer const& player, std::string const& key, sol::object defaultValue)
        {
            auto& values = EntityState[player.GetNativeHandle()];
            auto found = values.find(key);
            if (found != values.end())
                return found->second;
            return values.emplace(key, std::move(defaultValue)).first->second;
        });
    lua.new_usertype<TSSpell>("TSSpell", sol::no_constructor,
        "IsNull", &TSSpell::IsNull, "GetCaster", &TSSpell::GetCaster);
    lua.new_usertype<TSBattleground>("TSBattleground", sol::no_constructor, sol::base_classes, sol::bases<TSMap>(),
        "UpdateWorldState", &TSBattleground::UpdateWorldState, "EndBG", &TSBattleground::EndBG,
        "RewardHonor", &TSBattleground::RewardHonor, "GetPlayers", &TSBattleground::GetPlayers,
        "GetScore", &TSBattleground::GetScore,
        "GetObject", [](TSBattleground const& map, std::string const& key, sol::object defaultValue)
        {
            auto& values = EntityState[map.GetNativeHandle()];
            auto found = values.find(key);
            if (found != values.end())
                return found->second;
            return values.emplace(key, std::move(defaultValue)).first->second;
        },
        "AddTimer", [](TSBattleground const& owner, double interval, double repeats,
            sol::protected_function callback)
        {
            auto timer = std::make_shared<LuaTimer>();
            timer->Owner = owner;
            timer->Interval = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(interval));
            timer->Remaining = timer->Interval;
            timer->Repeats = static_cast<std::int32_t>(repeats);
            timer->Callback = std::move(callback);
            Timers.push_back(timer);
            return timer;
        });
    lua.new_usertype<LuaTimer>("TSTimer", sol::no_constructor, "Stop", &LuaTimer::Stop);
    lua.new_usertype<TSBattlegroundScore>("TSBattlegroundScore", sol::no_constructor,
        "ApplyBaseToPacket", &TSBattlegroundScore::ApplyBaseToPacket,
        "GetCustomAttr", &TSBattlegroundScore::GetCustomAttr, "SetCustomAttr", &TSBattlegroundScore::SetCustomAttr,
        "ModCustomAttr", &TSBattlegroundScore::ModCustomAttr);
    lua.new_usertype<TSWorldPacket>("TSWorldPacket", sol::no_constructor,
        "WriteUInt32", &TSWorldPacket::WriteUInt32, "WriteString", &TSWorldPacket::WriteString);
    lua.new_usertype<TSPacketRead>("TSPacketRead", sol::no_constructor,
        "ReadUInt32", &TSPacketRead::ReadUInt32, "ReadString", &TSPacketRead::ReadString,
        "Size", &TSPacketRead::Size, "Reset", &TSPacketRead::Reset);
    lua.new_usertype<TSPacketWrite>("TSPacketWrite", sol::no_constructor,
        "WriteUInt32", &TSPacketWrite::WriteUInt32, "WriteString", &TSPacketWrite::WriteString,
        "Size", &TSPacketWrite::Size, "SendToPlayer", &TSPacketWrite::SendToPlayer);
    environment.set_function("CreateCustomPacket", &CreateCustomPacket);
}

void BindEvents(sol::state_view& lua)
{
    using Player = TSEvents::PlayerEvents;
    auto player = lua.new_usertype<Player>("TSPlayerEvents", sol::no_constructor);
    player.set_function("OnLogin", [](Player& e, sol::protected_function f)
        { e.OnLogin(LuaCallback<TSPlayer, bool>(std::move(f))); });
    player.set_function("OnWhisper", [](Player& e, sol::protected_function f)
        { e.OnWhisper(LuaCallback<TSPlayer, TSPlayer, TSMutableString, double, double>(std::move(f))); });
    player.set_function("OnCommand", [](Player& e, sol::protected_function f)
        { e.OnCommand(LuaCallback<TSPlayer, TSMutableString, TSMutable<bool, bool>>(std::move(f))); });
    player.set_function("OnSave", [](Player& e, sol::protected_function f)
        { e.OnSave(LuaCallback<TSPlayer>(std::move(f))); });
    player.set_function("OnDelete", [](Player& e, sol::protected_function f)
        { e.OnDelete(LuaCallback<double, double>(std::move(f))); });
    player.set_function("OnCalcTalentPoints", [](Player& e, sol::protected_function f)
        { e.OnCalcTalentPoints(LuaCallback<TSPlayer, TSMutable<std::uint32_t, double>>(std::move(f))); });

    using Creature = TSEvents::CreatureEvents;
    auto creature = lua.new_usertype<Creature>("TSCreatureEvents", sol::no_constructor);
    creature.set_function("OnGossipHello", sol::overload(
        [](Creature& e, sol::protected_function f)
        { e.OnGossipHello(LuaCallback<TSCreature, TSPlayer, TSMutable<bool, bool>>(std::move(f))); },
        [](Creature& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnGossipHello(id,
            LuaCallback<TSCreature, TSPlayer, TSMutable<bool, bool>>(f)); }); }));
    creature.set_function("OnGossipSelect", sol::overload(
        [](Creature& e, sol::protected_function f)
        { e.OnGossipSelect(LuaCallback<TSCreature, TSPlayer, double, double, TSMutable<bool, bool>>(std::move(f))); },
        [](Creature& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnGossipSelect(id,
            LuaCallback<TSCreature, TSPlayer, double, double, TSMutable<bool, bool>>(f)); }); }));
    creature.set_function("OnDeath", sol::overload(
        [](Creature& e, sol::protected_function f) { e.OnDeath(LuaCallback<TSCreature, TSUnit>(std::move(f))); },
        [](Creature& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnDeath(id, LuaCallback<TSCreature, TSUnit>(f)); }); }));

    using Map = TSEvents::MapEvents;
    auto map = lua.new_usertype<Map>("TSMapEvents", sol::no_constructor);
    map.set_function("OnPlayerEnter", sol::overload(
        [](Map& e, sol::protected_function f) { e.OnPlayerEnter(LuaCallback<TSMap, TSPlayer>(std::move(f))); },
        [](Map& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnPlayerEnter(id, LuaCallback<TSMap, TSPlayer>(f)); }); }));

    using Battleground = TSEvents::BattlegroundEvents;
    auto battleground = lua.new_usertype<Battleground>("TSBattlegroundEvents", sol::no_constructor);
    battleground.set_function("OnAddPlayer", [](Battleground& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnAddPlayer(id, LuaCallback<TSBattleground, TSPlayer>(f)); }); });
    battleground.set_function("OnOpenDoors", [](Battleground& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnOpenDoors(id, LuaCallback<TSBattleground>(f)); }); });
    battleground.set_function("OnSendScore", [](Battleground& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnSendScore(id,
            LuaCallback<TSBattleground, TSBattlegroundScore, TSWorldPacket, TSMutable<bool, bool>>(f)); }); });

    using Spell = TSEvents::SpellEvents;
    auto spell = lua.new_usertype<Spell>("TSSpellEvents", sol::no_constructor);
    spell.set_function("OnCast", sol::overload(
        [](Spell& e, sol::protected_function f) { e.OnCast(LuaCallback<TSSpell>(std::move(f))); },
        [](Spell& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnCast(id, LuaCallback<TSSpell>(f)); }); }));

    using Packet = TSEvents::CustomPacketEvents;
    auto packet = lua.new_usertype<Packet>("TSCustomPacketEvents", sol::no_constructor);
    packet.set_function("OnReceive", sol::overload(
        [](Packet& e, sol::protected_function f)
        { e.OnReceive(LuaCallback<double, TSPacketRead, TSPlayer>(std::move(f))); },
        [](Packet& e, sol::object ids, sol::protected_function f)
        { AddMapped(ids, [&](std::uint32_t id) { e.OnReceive(id,
            LuaCallback<double, TSPacketRead, TSPlayer>(f)); }); }));

    using World = TSEvents::WorldEvents;
    auto world = lua.new_usertype<World>("TSWorldEvents", sol::no_constructor);
    world.set_function("OnStartup", [](World& e, sol::protected_function f)
        { e.OnStartup(LuaCallback<>(std::move(f))); });
    world.set_function("OnShutdown", [](World& e, sol::protected_function f)
        { e.OnShutdown(LuaCallback<>(std::move(f))); });

    lua.new_usertype<TSEvents>("TSEvents", sol::no_constructor,
        "World", &TSEvents::World, "Player", &TSEvents::Player, "Creature", &TSEvents::Creature,
        "Map", &TSEvents::Map, "Battleground", &TSEvents::Battleground, "Spell", &TSEvents::Spell,
        "CustomPacket", &TSEvents::CustomPacket);
}

void BindGlobals(sol::state_view& lua, sol::environment& environment)
{
    environment.set_function("require", &Require);
    BindMutables(lua);
    BindObjects(lua, environment);
    BindEvents(lua);

    auto unresolvedTag = [](std::string const&, std::string const&) -> double
    {
        throw std::runtime_error("Unresolved TSWoW tag reached Lua runtime; run the datascript/tag generation pipeline first");
    };
    environment.set_function("TAG", unresolvedTag);
    environment.set_function("UTAG", unresolvedTag);
    environment.set_function("GetIDTagUnique", unresolvedTag);
    environment.set_function("GetIDTag", [](std::string const&, std::string const&) -> sol::object
    {
        throw std::runtime_error("Unresolved TSWoW ID tag reached Lua runtime; run the datascript/tag generation pipeline first");
    });

    sol::object bundle = Require("lualib_bundle");
    environment["TSClass"] = lua.safe_script(
        "local c = require('lualib_bundle').__TS__Class(); c.name = 'TSClass'; "
        "function c.prototype.____constructor(self) end; return c",
        environment, sol::script_pass_on_error);
    environment["DBEntry"] = lua.safe_script(
        "local c = require('lualib_bundle').__TS__Class(); c.name = 'DBEntry'; "
        "function c.prototype.____constructor(self) end; return c",
        environment, sol::script_pass_on_error);
    environment["DBArrayEntry"] = lua.safe_script(
        "local c = require('lualib_bundle').__TS__Class(); c.name = 'DBArrayEntry'; "
        "function c.prototype.____constructor(self) self.__index = 0; self.__dirty = true; "
        "self.__deleted = false end; function c.prototype.MarkDirty(self) self.__dirty = true end; "
        "function c.prototype.IsDirty(self) return self.__dirty end; "
        "function c.prototype.Delete(self) self.__deleted = true; self.__dirty = true end; "
        "function c.prototype.IsDeleted(self) return self.__deleted end; "
        "function c.prototype.Index(self) return self.__index end; return c",
        environment, sol::script_pass_on_error);
    BindLuaDatabaseCompatibility(lua, environment);
    (void)bundle;
}

void DetachFromAle(bool requestReload)
{
    Timers.clear();
    EntityState.clear();
    Modules.clear();
    FileStack.clear();
    Environment.reset();
    Lua.reset();
    AttachedState = nullptr;
    LuaRoot.clear();
    ReloadRequested = requestReload;
}
}

bool LoadLuaLivescripts(std::filesystem::path const& root)
{
    LOCK_ALE;
    UnloadLuaLivescripts();
    if (!std::filesystem::is_directory(root))
        return true;
    if (!ALE::IsInitialized() || !sALE || !sALE->HasLuaState())
    {
        LOG_ERROR("tswow.lua", "mod-ale has no active Lua state");
        return false;
    }

    LuaRoot = Normalize(root);
    AttachedState = sALE->L;
    Lua = std::make_unique<sol::state_view>(AttachedState);
    Environment = std::make_unique<sol::environment>(*Lua, sol::create, Lua->globals());
    try
    {
        BindGlobals(*Lua, *Environment);
        sol::protected_function registerServerEvent = (*Lua)["RegisterServerEvent"];
        if (!registerServerEvent.valid() || !LogLuaError(registerServerEvent(16, [](sol::variadic_args)
            {
                LOCK_ALE;
                ts_events.Clear();
                DetachFromAle(true);
            })))
            throw std::runtime_error("Could not register the mod-ale state-close callback");

        for (auto const& entry : std::filesystem::recursive_directory_iterator(LuaRoot))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".lua" ||
                entry.path().filename() == "lualib_bundle.lua")
                continue;
            ExecuteFile(entry.path());
        }

        for (auto const& [path, value] : Modules)
        {
            if (value.get_type() != sol::type::table)
                continue;
            sol::object main = value.as<sol::table>()["Main"];
            if (main.get_type() == sol::type::function)
                if (!LogLuaError(main.as<sol::protected_function>()(&ts_events)))
                    throw std::runtime_error("Lua Main failed in " + path.string());
            sol::object inlineMain = value.as<sol::table>()["__InlineMain"];
            if (inlineMain.get_type() == sol::type::function)
                if (!LogLuaError(inlineMain.as<sol::protected_function>()()))
                    throw std::runtime_error("Lua __InlineMain failed in " + path.string());
        }
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("tswow.lua", "Lua livescript load failed: {}", error.what());
        return false;
    }

    ReloadRequested = false;
    LOG_INFO("tswow.lua", "Loaded Lua livescripts from {}", LuaRoot.string());
    return true;
}

void UnloadLuaLivescripts()
{
    LOCK_ALE;
    DetachFromAle(false);
}

void UpdateLuaLivescripts(std::uint32_t diff)
{
    LOCK_ALE;
    if (!Lua)
        return;
    for (std::shared_ptr<LuaTimer> const& timer : Timers)
    {
        if (timer->Stopped)
            continue;
        timer->Remaining -= diff;
        while (!timer->Stopped && timer->Remaining <= 0)
        {
            LogLuaError(timer->Callback(timer->Owner, timer));
            if (timer->Repeats == 0)
            {
                timer->Stopped = true;
                break;
            }
            if (timer->Repeats > 0)
                --timer->Repeats;
            timer->Remaining += timer->Interval;
        }
    }
    Timers.erase(std::remove_if(Timers.begin(), Timers.end(),
        [](std::shared_ptr<LuaTimer> const& timer) { return timer->Stopped; }), Timers.end());
}

void ClearLuaEntityState(void* owner)
{
    LOCK_ALE;
    EntityState.erase(owner);
    for (std::shared_ptr<LuaTimer> const& timer : Timers)
        if (timer->Owner.GetNativeHandle() == owner)
            timer->Stopped = true;
}

bool LuaLivescriptsNeedReload()
{
    LOCK_ALE;
    return ReloadRequested;
}
