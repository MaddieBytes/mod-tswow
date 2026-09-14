/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "Config.h"
#include "ConditionMgr.h"
#include "ALEScript.h"
#include "Chat.h"
#include "Battleground.h"
#include "BattlegroundScore.h"
#include "CustomPacketBuffer.h"
#include "CustomPacketChunk.h"
#include "CustomPacketDefines.h"
#include "CustomPacketRead.h"
#include "CustomPacketWrite.h"
#include "Log.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "Item.h"
#include "InstanceSaveMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "Mail.h"
#include "MySQLConnection.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellAuras.h"
#include "TSAll.h"
#include "Trainer.h"
#include "TSLuaRuntime.h"
#include "TypeContainerVisitor.h"
#include "Vehicle.h"
#include "Weather.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>

#if PLATFORM == PLATFORM_WINDOWS
#include <Windows.h>
using LibraryHandle = HMODULE;
#define TSWOW_LIBRARY_EXTENSION ".dll"
#else
#include <dlfcn.h>
using LibraryHandle = void*;
#define TSWOW_LIBRARY_EXTENSION ".so"
#endif

namespace
{
using AddTsScripts = void (*)(TSEvents*);

std::map<std::filesystem::path, LibraryHandle> Libraries;
std::unordered_map<void*, std::unordered_map<std::string, std::uint32_t>> BattlegroundScoreAttributes;
std::unordered_map<std::uint32_t, std::uint32_t> PermanentTalentQuestRewards;
std::unordered_map<std::uint64_t, std::array<std::uint32_t, MAX_STATS>> PlayerLevelStats;
std::unordered_map<void*, std::unordered_map<std::string, std::shared_ptr<void>>> NativeObjectState;
std::unordered_map<std::uint64_t, std::shared_ptr<TSOutfitData>> NativeOutfits;
std::mutex NativeOutfitsMutex;

struct NativeTimer
{
    void* Owner = nullptr;
    std::string Name;
    std::uint32_t Delay = 1;
    std::int64_t Remaining = 1;
    std::uint64_t Diff = 0;
    std::int32_t Repeats = 1;
    std::uint32_t Flags = 0;
    bool Stopped = false;
    std::function<void(void*, void*)> Callback;
};

std::vector<std::shared_ptr<NativeTimer>> NativeTimers;
std::unordered_map<void*, std::vector<std::function<void(void*)>>> NativeDelayedCallbacks;

TSTimerApi const NativeTimerApi{
    [](void* timer) { static_cast<NativeTimer*>(timer)->Stopped = true; },
    [](void* timer) { return static_cast<NativeTimer*>(timer)->Delay; },
    [](void* timer, std::uint32_t value) { static_cast<NativeTimer*>(timer)->Delay = std::max(1u, value); },
    [](void* timer) { return static_cast<NativeTimer*>(timer)->Diff; },
    [](void* timer) { return static_cast<NativeTimer*>(timer)->Flags; },
    [](void* timer, std::uint32_t value) { static_cast<NativeTimer*>(timer)->Flags = value; },
    [](void* timer) { return static_cast<NativeTimer*>(timer)->Repeats; },
    [](void* timer, std::int32_t value) { static_cast<NativeTimer*>(timer)->Repeats = value; },
    [](void* timer) { return static_cast<NativeTimer*>(timer)->Name; }
};

void AddNativeTimer(void* owner, char const* name, std::uint32_t delay, std::int32_t repeats,
    std::uint32_t flags, std::function<void(void*, void*)> callback)
{
    if (name && *name)
        for (auto const& timer : NativeTimers)
            if (timer->Owner == owner && timer->Name == name)
                timer->Stopped = true;
    auto timer = std::make_shared<NativeTimer>();
    timer->Owner = owner;
    timer->Name = name ? name : "";
    timer->Delay = std::max(1u, delay);
    timer->Remaining = timer->Delay;
    timer->Repeats = repeats;
    timer->Flags = flags;
    timer->Callback = std::move(callback);
    NativeTimers.push_back(std::move(timer));
}

void RemoveNativeTimer(void* owner, char const* name)
{
    for (auto const& timer : NativeTimers)
        if (timer->Owner == owner && timer->Name == name)
            timer->Stopped = true;
}

void AddNativeDelayedCallback(void* owner, std::function<void(void*)> callback)
{
    NativeDelayedCallbacks[owner].push_back(std::move(callback));
}

void UpdateNativeTimers(void* owner, std::uint32_t diff)
{
    std::size_t const count = NativeTimers.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        auto timer = NativeTimers[index];
        if (timer->Stopped || timer->Owner != owner)
            continue;
        timer->Remaining -= diff;
        while (!timer->Stopped && timer->Remaining <= 0)
        {
            timer->Diff = static_cast<std::uint64_t>(timer->Delay - timer->Remaining);
            timer->Callback(owner, timer.get());
            timer->Diff = 0;
            if (timer->Repeats > 0 && --timer->Repeats == 0)
                timer->Stopped = true;
            timer->Remaining += timer->Delay;
            if (!(timer->Flags & static_cast<std::uint32_t>(TimerFlags::AGGREGATE_LOOPS)))
                break;
        }
    }
    NativeTimers.erase(std::remove_if(NativeTimers.begin(), NativeTimers.end(),
        [](auto const& timer) { return timer->Stopped; }), NativeTimers.end());
}

void RunNativeDelayedCallbacks(void* owner)
{
    auto found = NativeDelayedCallbacks.find(owner);
    if (found == NativeDelayedCallbacks.end()) return;
    auto callbacks = std::move(found->second);
    NativeDelayedCallbacks.erase(found);
    for (auto& callback : callbacks) callback(owner);
}

std::shared_ptr<void> GetNativeObjectState(void* owner, std::string const& key,
    std::function<std::shared_ptr<void>()> const& factory)
{
    auto& values = NativeObjectState[owner];
    auto found = values.find(key);
    if (found != values.end())
        return found->second;
    if (!factory)
        return nullptr;
    return values.emplace(key, factory()).first->second;
}

std::shared_ptr<void> SetNativeObjectState(void* owner, std::string const& key, std::shared_ptr<void> value)
{
    NativeObjectState[owner][key] = std::move(value);
    return NativeObjectState[owner][key];
}

bool HasNativeObjectState(void* owner, std::string const& key)
{
    auto values = NativeObjectState.find(owner);
    return values != NativeObjectState.end() && values->second.find(key) != values->second.end();
}

TSObjectStateApi const ObjectStateApi{
    GetNativeObjectState,
    SetNativeObjectState,
    HasNativeObjectState
};

void ClearNativeObjectState(void* owner)
{
    NativeObjectState.erase(owner);
    NativeDelayedCallbacks.erase(owner);
    for (auto const& timer : NativeTimers)
        if (timer->Owner == owner)
            timer->Stopped = true;
}

void DetachNativeOutfitReferences()
{
    std::lock_guard<std::mutex> lock(NativeOutfitsMutex);
    for (auto& [guid, outfit] : NativeOutfits)
    {
        (void)guid;
        if (outfit)
            outfit = std::make_shared<TSOutfitData>(*outfit);
    }
}

struct NativeDatabaseResult
{
    explicit NativeDatabaseResult(QueryResult value) : result(std::move(value)) { }
    QueryResult result;
    Field* fields = nullptr;
};

QueryResult DatabaseQuery(std::uint8_t database, std::string const& sql)
{
    switch (static_cast<TSDatabaseType>(database))
    {
        case TSDatabaseType::WORLD: return WorldDatabase.Query(sql);
        case TSDatabaseType::AUTH: return LoginDatabase.Query(sql);
        case TSDatabaseType::CHARACTERS: return CharacterDatabase.Query(sql);
    }
    throw std::out_of_range("TSWoW database type");
}

void* QueryDatabase(std::uint8_t database, char const* sql)
{
    return new NativeDatabaseResult(DatabaseQuery(database, sql));
}

void QueryDatabaseAsync(std::uint8_t database, char const* sql)
{
    switch (static_cast<TSDatabaseType>(database))
    {
        case TSDatabaseType::WORLD: WorldDatabase.Execute(sql); return;
        case TSDatabaseType::AUTH: LoginDatabase.Execute(sql); return;
        case TSDatabaseType::CHARACTERS: CharacterDatabase.Execute(sql); return;
    }
    throw std::out_of_range("TSWoW database type");
}

NativeDatabaseResult const* AsDatabaseResult(void const* value)
{
    return static_cast<NativeDatabaseResult const*>(value);
}

bool DatabaseResultIsValid(void const* value)
{
    return value && AsDatabaseResult(value)->result != nullptr;
}

bool DatabaseResultGetRow(void* value)
{
    auto* result = static_cast<NativeDatabaseResult*>(value);
    if (!result || !result->result)
        return false;
    if (!result->fields)
    {
        result->fields = result->result->Fetch();
        return true;
    }
    if (!result->result->NextRow())
        return false;
    result->fields = result->result->Fetch();
    return true;
}

Field const& DatabaseField(void const* value, std::uint32_t index)
{
    auto const* result = AsDatabaseResult(value);
    if (!result || !result->fields)
        throw std::runtime_error("TSWoW database field read before GetRow");
    return result->fields[index];
}

std::uint64_t DatabaseResultGetUInt(void const* value, std::uint32_t index, std::uint8_t width)
{
    Field const& field = DatabaseField(value, index);
    switch (width)
    {
        case 1: return field.Get<std::uint8_t>();
        case 2: return field.Get<std::uint16_t>();
        case 4: return field.Get<std::uint32_t>();
        case 8: return field.Get<std::uint64_t>();
        default: throw std::out_of_range("TSWoW unsigned database field width");
    }
}

std::int64_t DatabaseResultGetInt(void const* value, std::uint32_t index, std::uint8_t width)
{
    Field const& field = DatabaseField(value, index);
    switch (width)
    {
        case 1: return field.Get<std::int8_t>();
        case 2: return field.Get<std::int16_t>();
        case 4: return field.Get<std::int32_t>();
        case 8: return field.Get<std::int64_t>();
        default: throw std::out_of_range("TSWoW signed database field width");
    }
}

double DatabaseResultGetDouble(void const* value, std::uint32_t index, bool singlePrecision)
{
    return singlePrecision ? DatabaseField(value, index).Get<float>() : DatabaseField(value, index).Get<double>();
}

std::string DatabaseResultGetString(void const* value, std::uint32_t index)
{
    return DatabaseField(value, index).Get<std::string>();
}

std::vector<std::uint8_t> DatabaseResultGetBinary(void const* value, std::uint32_t index)
{
    return DatabaseField(value, index).Get<Binary>();
}

std::string EscapeDatabaseString(std::uint8_t database, std::string const& input)
{
    std::string value = input;
    switch (static_cast<TSDatabaseType>(database))
    {
        case TSDatabaseType::WORLD: WorldDatabase.EscapeString(value); break;
        case TSDatabaseType::AUTH: LoginDatabase.EscapeString(value); break;
        case TSDatabaseType::CHARACTERS: CharacterDatabase.EscapeString(value); break;
        default: throw std::out_of_range("TSWoW database type");
    }
    return value;
}

MySQLConnectionInfo const* DatabaseInfo(std::uint8_t database)
{
    switch (static_cast<TSDatabaseType>(database))
    {
        case TSDatabaseType::WORLD: return WorldDatabase.GetConnectionInfo();
        case TSDatabaseType::AUTH: return LoginDatabase.GetConnectionInfo();
        case TSDatabaseType::CHARACTERS: return CharacterDatabase.GetConnectionInfo();
    }
    throw std::out_of_range("TSWoW database type");
}

std::string DatabaseConnectionInfo(std::uint8_t database, std::uint8_t field)
{
    MySQLConnectionInfo const* info = DatabaseInfo(database);
    switch (field)
    {
        case 0: return info->user;
        case 1: return info->password;
        case 2: return info->database;
        case 3: return info->host;
        case 4: return info->port_or_socket;
        case 5: return info->ssl;
        default: throw std::out_of_range("TSWoW database info field");
    }
}

std::string QuoteIdentifier(std::string value)
{
    std::size_t position = 0;
    while ((position = value.find('`', position)) != std::string::npos)
    {
        value.insert(position, 1, '`');
        position += 2;
    }
    return "`" + value + "`";
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string NormalizeSqlType(std::string value)
{
    static std::regex const integerDisplayWidth(
        R"(\b(tinyint|smallint|mediumint|int|bigint)\([0-9]+\))"
    );
    return std::regex_replace(Lowercase(std::move(value)), integerDisplayWidth, "$1");
}

void CreateDatabaseTable(std::uint8_t database, std::string const& databaseName,
    std::string const& tableName, std::vector<FieldSpec> const& fields)
{
    std::string sql = "CREATE TABLE " + QuoteIdentifier(databaseName) + "." + QuoteIdentifier(tableName) + " (";
    bool hasPrimaryKey = false;
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        FieldSpec const& field = fields[index];
        if (index) sql += ",";
        sql += QuoteIdentifier(field.m_name) + " " + field.m_typeName;
        if (field.m_autoIncrements) sql += " AUTO_INCREMENT";
        hasPrimaryKey = hasPrimaryKey || field.m_isPrimaryKey;
    }
    if (hasPrimaryKey)
    {
        sql += ", PRIMARY KEY (";
        bool first = true;
        for (FieldSpec const& field : fields)
            if (field.m_isPrimaryKey)
            {
                if (!first) sql += ",";
                sql += QuoteIdentifier(field.m_name);
                first = false;
            }
        sql += ")";
    }
    DatabaseQuery(database, sql + ");");
}

void CreateNativeDatabaseSpec(std::uint8_t database, std::string const& databaseName,
    std::string const& tableName, std::vector<FieldSpec> const& fields)
{
    std::string escapedDatabase = EscapeDatabaseString(database, databaseName);
    std::string escapedTable = EscapeDatabaseString(database, tableName);
    QueryResult exists = DatabaseQuery(database,
        "SELECT COUNT(*) FROM `information_schema`.`TABLES` WHERE `TABLE_SCHEMA`='" + escapedDatabase +
        "' AND `TABLE_NAME`='" + escapedTable + "'");
    if (!exists || exists->Fetch()[0].Get<std::uint32_t>() == 0)
    {
        CreateDatabaseTable(database, databaseName, tableName, fields);
        return;
    }

    std::vector<FieldSpec> oldFields;
    QueryResult columns = DatabaseQuery(database,
        "SELECT `COLUMN_NAME`,`COLUMN_TYPE`,`COLUMN_KEY`,`EXTRA` FROM `information_schema`.`COLUMNS` "
        "WHERE `TABLE_SCHEMA`='" + escapedDatabase + "' AND `TABLE_NAME`='" + escapedTable +
        "' ORDER BY `ORDINAL_POSITION`");
    if (columns)
        do
        {
            Field* row = columns->Fetch();
            oldFields.push_back({ Lowercase(row[0].Get<std::string>()), Lowercase(row[1].Get<std::string>()),
                Lowercase(row[2].Get<std::string>()) == "pri",
                Lowercase(row[3].Get<std::string>()).find("auto_increment") != std::string::npos });
        } while (columns->NextRow());

    auto primaryKeys = [](std::vector<FieldSpec> const& values)
    {
        std::vector<std::pair<std::string, std::string>> result;
        for (FieldSpec const& field : values)
            if (field.m_isPrimaryKey)
                result.emplace_back(Lowercase(field.m_name), NormalizeSqlType(field.m_typeName) +
                    (field.m_autoIncrements ? " auto_increment" : ""));
        return result;
    };
    if (primaryKeys(oldFields) != primaryKeys(fields))
    {
        DatabaseQuery(database, "DROP TABLE " + QuoteIdentifier(databaseName) + "." + QuoteIdentifier(tableName));
        CreateDatabaseTable(database, databaseName, tableName, fields);
        return;
    }

    std::string qualified = QuoteIdentifier(databaseName) + "." + QuoteIdentifier(tableName);
    for (FieldSpec const& oldField : oldFields)
    {
        auto current = std::find_if(fields.begin(), fields.end(), [&](FieldSpec const& field)
            { return Lowercase(field.m_name) == oldField.m_name; });
        if (current == fields.end())
            DatabaseQuery(database, "ALTER TABLE " + qualified + " DROP COLUMN " + QuoteIdentifier(oldField.m_name));
        else if (NormalizeSqlType(current->m_typeName) != NormalizeSqlType(oldField.m_typeName))
            DatabaseQuery(database, "ALTER TABLE " + qualified + " MODIFY COLUMN " + QuoteIdentifier(current->m_name) +
                " " + current->m_typeName);
    }
    for (FieldSpec const& field : fields)
    {
        auto old = std::find_if(oldFields.begin(), oldFields.end(), [&](FieldSpec const& value)
            { return value.m_name == Lowercase(field.m_name); });
        if (old == oldFields.end())
            DatabaseQuery(database, "ALTER TABLE " + qualified + " ADD COLUMN " + QuoteIdentifier(field.m_name) +
                " " + field.m_typeName);
    }
}

TSDatabaseApi const NativeDatabaseApi{
    QueryDatabase,
    QueryDatabaseAsync,
    [](void* result) { delete static_cast<NativeDatabaseResult*>(result); },
    DatabaseResultIsValid,
    DatabaseResultGetRow,
    DatabaseResultGetUInt,
    DatabaseResultGetInt,
    DatabaseResultGetDouble,
    DatabaseResultGetString,
    DatabaseResultGetBinary,
    EscapeDatabaseString,
    DatabaseConnectionInfo,
    CreateNativeDatabaseSpec
};

std::uint64_t PlayerLevelStatKey(std::uint8_t race, std::uint8_t playerClass, std::uint8_t level)
{
    return std::uint64_t(race) | (std::uint64_t(playerClass) << 8) | (std::uint64_t(level) << 16);
}

void LoadPlayerLevelStats()
{
    PlayerLevelStats.clear();
    QueryResult result = WorldDatabase.Query(
        "SELECT `race`, `class`, `level`, `str`, `agi`, `sta`, `inte`, `spi` "
        "FROM `player_levelstats`");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        PlayerLevelStats.emplace(
            PlayerLevelStatKey(fields[0].Get<uint8>(), fields[1].Get<uint8>(), fields[2].Get<uint8>()),
            std::array<std::uint32_t, MAX_STATS>{
                fields[3].Get<uint32>(), fields[4].Get<uint32>(), fields[5].Get<uint32>(),
                fields[6].Get<uint32>(), fields[7].Get<uint32>()});
    } while (result->NextRow());
}

void ApplyPlayerLevelStats(Player* player)
{
    auto const itr = PlayerLevelStats.find(
        PlayerLevelStatKey(player->getRace(), player->getClass(), player->GetLevel()));
    if (itr == PlayerLevelStats.end())
        return;

    for (std::uint8_t stat = 0; stat < MAX_STATS; ++stat)
        player->SetCreateStat(Stats(stat), itr->second[stat]);
    player->UpdateAllStats();
}

void LoadPermanentTalentQuestRewards()
{
    PermanentTalentQuestRewards.clear();
    QueryResult result = WorldDatabase.Query(
        "SELECT `ID`, `RewardTalentsPermanent` FROM `quest_template` "
        "WHERE `RewardTalentsPermanent` > 0");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        PermanentTalentQuestRewards.emplace(fields[0].Get<uint32>(), fields[1].Get<uint32>());
    } while (result->NextRow());
}

void ApplyTsWowBattlegroundDoors(Battleground* battleground, bool opening)
{
    BattlegroundMap* map = battleground->FindBgMap();
    if (!map)
        return;

    QueryResult result = WorldDatabase.Query(
        "SELECT `entry`, `type` FROM `battleground_door_object` WHERE `map` = {}",
        battleground->GetMapId());
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 const entry = fields[0].Get<uint32>();
        uint8 const type = fields[1].Get<uint8>();
        bool const active = opening ? type == 0 : type != 0;
        for (auto const& object : map->GetGameObjectBySpawnIdStore())
            if (object.second->GetEntry() == entry)
                object.second->SetGoState(active ? GO_STATE_ACTIVE : GO_STATE_READY);
    } while (result->NextRow());
}

std::uint64_t GetPlayerGuid(void* player)
{
    return static_cast<Player*>(player)->GetGUID().GetRawValue();
}

void SendPlayerBroadcastMessage(void* player, char const* message)
{
    ChatHandler(static_cast<Player*>(player)->GetSession()).SendSysMessage(message);
}

std::uint32_t GetPlayerClass(void* player)
{
    return static_cast<Player*>(player)->getClass();
}

std::uint32_t GetPlayerMapId(void* player)
{
    return static_cast<Player*>(player)->GetMapId();
}

std::uint32_t GetPlayerQuestRewardTalentPoints(void* player)
{
    return static_cast<Player*>(player)->GetQuestRewardTalentPoints();
}

std::uint32_t GetPlayerTeam(void* player)
{
    return static_cast<Player*>(player)->GetTeamId();
}

void AddPlayerGossipItem(void* player, std::uint32_t icon, char const* message, std::uint32_t sender,
    std::uint32_t action, bool code, char const* prompt, std::uint32_t money)
{
    AddGossipItemFor(static_cast<Player*>(player), icon, message, sender, action, prompt, money, code);
}

void CompletePlayerGossip(void* player)
{
    CloseGossipMenuFor(static_cast<Player*>(player));
}

void SendPlayerGossipText(void* playerHandle, void* senderHandle, char const* text, std::uint32_t language,
    std::uint32_t emote0, std::uint32_t emote0Delay, std::uint32_t emote1, std::uint32_t emote1Delay,
    std::uint32_t emote2, std::uint32_t emote2Delay, std::uint32_t menuId)
{
    constexpr std::uint32_t CustomGossipText = 0x7FFFFFFF;
    Player* player = static_cast<Player*>(playerHandle);
    Unit* sender = static_cast<Unit*>(senderHandle);
    bool const isFemale = sender->getGender() == GENDER_FEMALE;
    std::string const male = isFemale ? "" : text;
    std::string const female = isFemale ? text : "";

    WorldPacket packet(SMSG_NPC_TEXT_UPDATE, 4 + (8 * 4 * MAX_GOSSIP_TEXT_OPTIONS) +
        ((MAX_GOSSIP_TEXT_OPTIONS - 1) * 2) + male.size() + female.size() + 2);
    packet << CustomGossipText << std::uint32_t(100) << male << female << language;
    packet << emote0 << emote0Delay << emote1 << emote1Delay << emote2 << emote2Delay;
    for (std::size_t i = 0; i < MAX_GOSSIP_TEXT_OPTIONS - 1; ++i)
    {
        packet << std::uint32_t(0) << "" << "";
        for (std::size_t j = 0; j < 7; ++j)
            packet << std::uint32_t(0);
    }
    player->GetSession()->SendPacket(&packet);

    if (sender->IsPlayer())
        player->PlayerTalkClass->GetGossipMenu().SetMenuId(menuId);
    SendGossipMenuFor(player, CustomGossipText, sender->GetGUID());
}

void ClearPlayerGossip(void* player)
{
    ClearGossipMenuFor(static_cast<Player*>(player));
}

void SetPlayerTaxiCheat(void* player, bool enabled)
{
    static_cast<Player*>(player)->SetTaxiCheater(enabled);
}

void SendPlayerWorldState(void* player, std::uint32_t variable, std::uint32_t value)
{
    static_cast<Player*>(player)->SendUpdateWorldState(variable, value);
}

void SendPlayerCustomPacket(void* playerHandle, std::uint16_t opcode, char const* data, std::uint32_t size)
{
    Player* player = static_cast<Player*>(playerHandle);
    CustomPacketWrite write(opcode, MAX_FRAGMENT_SIZE, size);
    if (size)
        write.WriteBytes(size, data);
    for (CustomPacketChunk& chunk : write.buildMessages())
    {
        WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
        packet.append(reinterpret_cast<std::uint8_t*>(chunk.Data()), chunk.FullSize());
        player->SendDirectMessage(&packet);
    }
    write.Destroy();
}

void SendPlayerAddonMessage(void* playerHandle, char const* prefix, char const* message,
    std::uint8_t channel, void* receiverHandle)
{
    Player* player = static_cast<Player*>(playerHandle);
    Player* receiver = static_cast<Player*>(receiverHandle);
    std::string const fullMessage = std::string(prefix) + '\t' + message;
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, static_cast<ChatMsg>(channel), LANG_ADDON, player->GetGUID(),
        receiver->GetGUID(), fullMessage, player->GetChatTag());
    receiver->GetSession()->SendPacket(&packet);
}

bool TeleportPlayer(void* playerHandle, std::uint32_t map, float x, float y, float z, float orientation)
{
    Player* player = static_cast<Player*>(playerHandle);
    if (player->IsInFlight())
    {
        player->GetMotionMaster()->MovementExpired();
        player->m_taxi.ClearTaxiDestinations();
    }
    else
        player->SaveRecallPosition();
    return player->TeleportTo(map, x, y, z, orientation);
}

bool PlayerHasItem(void* player, std::uint32_t item, std::uint32_t count, bool checkBank)
{
    return static_cast<Player*>(player)->HasItemCount(item, count, checkBank);
}

std::uint16_t GetPlayerSkillValue(void* player, std::uint32_t skill)
{
    return static_cast<Player*>(player)->GetSkillValue(skill);
}

void SetPlayerSkill(void* player, std::uint16_t id, std::uint16_t step, std::uint16_t value,
    std::uint16_t maximum)
{
    static_cast<Player*>(player)->SetSkill(id, step, value, maximum);
}

void PlayPlayerDirectSound(void* player, std::uint32_t sound, void* receiver)
{
    static_cast<Player*>(player)->PlayDirectSound(sound, static_cast<Player*>(receiver));
}

bool AddPlayerItem(void* player, std::uint32_t item, std::uint32_t count)
{
    return static_cast<Player*>(player)->AddItem(item, count);
}

void CompletePlayerQuestObjective(void* player, std::uint32_t quest)
{
    static_cast<Player*>(player)->AreaExploredOrEventHappens(quest);
}

void TeachPlayerSpell(void* player, std::uint32_t spell)
{
    static_cast<Player*>(player)->learnSpell(spell, false);
}

std::int32_t GetBattlegroundStartDelay(void* battleground)
{
    return static_cast<Battleground*>(battleground)->GetStartDelayTime();
}

void SetBattlegroundStartDelay(void* battleground, std::int32_t time)
{
    static_cast<Battleground*>(battleground)->SetStartDelayTime(time);
}

void ReadCustomPacketBytes(void* packet, std::uint32_t size, void* output)
{
    char* bytes = static_cast<CustomPacketRead*>(packet)->ReadBytes(size, false);
    if (bytes)
    {
        std::memcpy(output, bytes, size);
        delete[] bytes;
    }
}

std::string ReadCustomPacketString(void* packet, std::string const& value)
{
    return static_cast<CustomPacketRead*>(packet)->ReadString(value);
}

std::uint32_t GetCustomPacketSize(void* packet)
{
    return static_cast<CustomPacketRead*>(packet)->Size();
}

void ResetCustomPacket(void* packet)
{
    static_cast<CustomPacketRead*>(packet)->Reset();
}

bool IsPlayerUnit(void* unit)
{
    return static_cast<Unit*>(unit)->IsPlayer();
}

void* GetEffectiveUnitOwner(void* unitHandle)
{
    Unit* unit = static_cast<Unit*>(unitHandle);
    std::set<Unit*> visited;
    while (unit && visited.insert(unit).second)
    {
        Unit* owner = unit->GetCharmerOrOwner();
        if (!owner)
            return unit;
        unit = owner;
    }
    return nullptr;
}

std::uint32_t GetUnitGuidLow(void* unit)
{
    return static_cast<Unit*>(unit)->GetGUID().GetCounter();
}

std::uint32_t GetUnitMapId(void* unit)
{
    return static_cast<Unit*>(unit)->GetMapId();
}

void* GetUnitMap(void* unit)
{
    return static_cast<Unit*>(unit)->GetMap();
}

bool IsBattlegroundMap(void* map)
{
    return static_cast<Map*>(map)->IsBattleground();
}

void* GetMapBattleground(void* map)
{
    BattlegroundMap* battlegroundMap = static_cast<Map*>(map)->ToBattlegroundMap();
    return battlegroundMap ? battlegroundMap->GetBG() : nullptr;
}

std::size_t GetMapPlayerCount(void* map)
{
    return static_cast<Map*>(map)->GetPlayers().getSize();
}

void* GetMapPlayerAt(void* map, std::size_t index)
{
    auto const& players = static_cast<Map*>(map)->GetPlayers();
    if (index >= players.getSize())
        return nullptr;
    auto player = players.begin();
    std::advance(player, index);
    return player->GetSource();
}

void RespawnCreature(void* creature)
{
    static_cast<Creature*>(creature)->Respawn();
}

void RemoveCreatureCorpse(void* creature)
{
    static_cast<Creature*>(creature)->RemoveCorpse();
}

std::uint32_t ResolveOutfitItemDisplay(std::uint32_t entry)
{
    ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
    return item ? item->DisplayInfoID : 0;
}

std::uint32_t ResolveOutfitDisplay(std::uint8_t race, std::uint8_t gender)
{
    ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(race);
    if (!raceEntry)
        raceEntry = sChrRacesStore.LookupEntry(RACE_HUMAN);
    return gender == GENDER_FEMALE ? raceEntry->model_f : raceEntry->model_m;
}

bool CopyPlayerOutfit(void* playerHandle, std::uint32_t settings, std::int32_t race,
    std::int32_t gender, TSOutfitData* output)
{
    Player* player = static_cast<Player*>(playerHandle);
    if (!player || !output)
        return false;

    *output = TSOutfitData();
    output->Race = race > 0 ? static_cast<std::uint8_t>(race) : player->getRace();
    output->Gender = gender >= 0 ? static_cast<std::uint8_t>(gender) : player->getGender();
    output->DisplayId = ResolveOutfitDisplay(output->Race, output->Gender);
    if (race <= 0 && gender < 0)
    {
        output->Skin = player->GetByteValue(PLAYER_BYTES, 0);
        output->Face = player->GetByteValue(PLAYER_BYTES, 1);
        output->HairStyle = player->GetByteValue(PLAYER_BYTES, 2);
        output->HairColor = player->GetByteValue(PLAYER_BYTES, 3);
        output->FacialStyle = player->GetByteValue(PLAYER_BYTES_2, 0);
    }
    if (settings & Outfit::CLASS)
        output->Class = player->getClass();
    if (settings & Outfit::GUILD)
        output->Guild = player->GetGuildId();

    static constexpr std::array<std::pair<std::uint32_t, EquipmentSlots>, 10> armorSlots{{
        {Outfit::HEAD, EQUIPMENT_SLOT_HEAD}, {Outfit::SHOULDERS, EQUIPMENT_SLOT_SHOULDERS},
        {Outfit::BODY, EQUIPMENT_SLOT_BODY}, {Outfit::CHEST, EQUIPMENT_SLOT_CHEST},
        {Outfit::WAIST, EQUIPMENT_SLOT_WAIST}, {Outfit::LEGS, EQUIPMENT_SLOT_LEGS},
        {Outfit::FEET, EQUIPMENT_SLOT_FEET}, {Outfit::WRISTS, EQUIPMENT_SLOT_WRISTS},
        {Outfit::HANDS, EQUIPMENT_SLOT_HANDS}, {Outfit::BACK, EQUIPMENT_SLOT_BACK}
    }};
    for (auto const& [flag, slot] : armorSlots)
    {
        if (!(settings & flag))
            continue;
        if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            std::uint32_t display = item->GetTemplate()->DisplayInfoID;
            sScriptMgr->OnGlobalMirrorImageDisplayItem(item, display);
            output->ItemDisplays[slot] = display;
        }
    }

    if (settings & Outfit::MAINHAND)
        output->Mainhand = player->GetUInt32Value(PLAYER_VISIBLE_ITEM_16_ENTRYID);
    if (settings & Outfit::OFFHAND)
        output->Offhand = player->GetUInt32Value(PLAYER_VISIBLE_ITEM_17_ENTRYID);
    if (settings & Outfit::RANGED)
        output->Ranged = player->GetUInt32Value(PLAYER_VISIBLE_ITEM_18_ENTRYID);
    return true;
}

bool CopyCreatureOutfit(void* creatureHandle, std::uint32_t, std::int32_t, std::int32_t,
    TSOutfitData* output)
{
    Creature* creature = static_cast<Creature*>(creatureHandle);
    if (!creature || !output)
        return false;
    std::lock_guard<std::mutex> lock(NativeOutfitsMutex);
    auto found = NativeOutfits.find(creature->GetGUID().GetRawValue());
    if (found == NativeOutfits.end())
        return false;
    if (!found->second)
        return false;
    *output = *found->second;
    return true;
}

void ApplyNativeOutfit(void* creatureHandle, std::shared_ptr<TSOutfitData> const& input)
{
    Creature* creature = static_cast<Creature*>(creatureHandle);
    if (!creature || !input)
        return;

    if (!input->DisplayId)
        input->DisplayId = ResolveOutfitDisplay(input->Race, input->Gender);
    TSOutfitData outfit = *input;
    {
        std::lock_guard<std::mutex> lock(NativeOutfitsMutex);
        NativeOutfits[creature->GetGUID().GetRawValue()] = input;
    }

    if (outfit.Mainhand >= 0) creature->SetVirtualItem(0, outfit.Mainhand);
    if (outfit.Offhand >= 0) creature->SetVirtualItem(1, outfit.Offhand);
    if (outfit.Ranged >= 0) creature->SetVirtualItem(2, outfit.Ranged);
    creature->RemoveAurasByType(SPELL_AURA_CLONE_CASTER);
    creature->AddAura(45204, creature);
    creature->SetUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE);
    creature->SetDisplayId(outfit.DisplayId);
    creature->UpdateObjectVisibility(true);
}

void ApplyPlayerOutfit(void* creatureHandle, void* playerHandle)
{
    TSOutfitData outfit;
    if (CopyPlayerOutfit(playerHandle, Outfit::EVERYTHING, -1, -1, &outfit))
        ApplyNativeOutfit(creatureHandle, std::make_shared<TSOutfitData>(outfit));
}

TSOutfitApi const NativeOutfitApi{
    &ResolveOutfitDisplay,
    &ResolveOutfitItemDisplay,
    &CopyPlayerOutfit,
    &CopyCreatureOutfit,
    &ApplyNativeOutfit,
};

void UpdateBattlegroundWorldState(void* battleground, std::uint32_t variable, std::uint32_t value)
{
    static_cast<Battleground*>(battleground)->UpdateWorldState(variable, value);
}

void EndBattleground(void* battleground, std::uint32_t winnerTeam)
{
    static_cast<Battleground*>(battleground)->EndBattleground(static_cast<TeamId>(winnerTeam));
}

void RewardBattlegroundHonor(void* battleground, std::uint32_t honor, std::uint32_t team)
{
    Battleground* bg = static_cast<Battleground*>(battleground);
    if (team == TEAM_NEUTRAL)
    {
        bg->RewardHonorToTeam(honor, TEAM_ALLIANCE);
        bg->RewardHonorToTeam(honor, TEAM_HORDE);
    }
    else
        bg->RewardHonorToTeam(honor, static_cast<TeamId>(team));
}

std::size_t GetBattlegroundPlayerCount(void* battleground)
{
    return static_cast<Battleground*>(battleground)->GetPlayers().size();
}

void* GetBattlegroundPlayerAt(void* battleground, std::size_t index)
{
    auto const& players = static_cast<Battleground*>(battleground)->GetPlayers();
    if (index >= players.size())
        return nullptr;
    auto player = players.begin();
    std::advance(player, index);
    return ObjectAccessor::FindConnectedPlayer(player->first);
}

void* GetBattlegroundScore(void* battleground, std::uint32_t guid)
{
    Battleground::BattlegroundScoreMap const* scores =
        static_cast<Battleground*>(battleground)->GetPlayerScores();
    auto const score = scores->find(guid);
    return score == scores->end() ? nullptr : score->second;
}

void WritePacketBytes(void* packet, char const* bytes, std::uint32_t size)
{
    static_cast<WorldPacket*>(packet)->append(reinterpret_cast<std::uint8_t const*>(bytes), size);
}

void WritePacketString(void* packet, std::string const& value)
{
    *static_cast<WorldPacket*>(packet) << value;
}

void AppendBattlegroundScoreBase(void* score, void* packet)
{
    static_cast<BattlegroundScore*>(score)->AppendBaseToPacket(*static_cast<WorldPacket*>(packet));
}

std::uint32_t GetBattlegroundScoreCustomAttr(void* score, std::string const& key)
{
    auto const scoreAttributes = BattlegroundScoreAttributes.find(score);
    if (scoreAttributes == BattlegroundScoreAttributes.end())
        return 0;
    auto const attribute = scoreAttributes->second.find(key);
    return attribute == scoreAttributes->second.end() ? 0 : attribute->second;
}

void SetBattlegroundScoreCustomAttr(void* score, std::string const& key, std::uint32_t value)
{
    BattlegroundScoreAttributes[score][key] = value;
}

void ModBattlegroundScoreCustomAttr(void* score, std::string const& key, std::int32_t value)
{
    BattlegroundScoreAttributes[score][key] += value;
}

extern TSMapApi const MapApi;
extern TSBattlegroundApi const BattlegroundApi;
extern TSBattlegroundScoreApi const BattlegroundScoreApi;
extern TSPlayerApi const PlayerApi;
extern TSUnitApi const UnitApi;

TSWorldPacketApi const WorldPacketApi = { &WritePacketBytes, &WritePacketString };

TSBattlegroundScoreApi const BattlegroundScoreApi = {
    &AppendBattlegroundScoreBase,
    &GetBattlegroundScoreCustomAttr,
    &SetBattlegroundScoreCustomAttr,
    &ModBattlegroundScoreCustomAttr,
};

TSBattlegroundApi const BattlegroundApi = {
    &UpdateBattlegroundWorldState,
    &EndBattleground,
    &RewardBattlegroundHonor,
    &GetBattlegroundPlayerCount,
    &GetBattlegroundPlayerAt,
    &GetBattlegroundScore,
    &GetBattlegroundStartDelay,
    &SetBattlegroundStartDelay,
    &PlayerApi,
    &UnitApi,
};

TSMapApi const MapApi = {
    &IsBattlegroundMap,
    &GetMapBattleground,
    &GetMapPlayerCount,
    &GetMapPlayerAt,
    &BattlegroundApi,
    &BattlegroundScoreApi,
    &ObjectStateApi,
    &AddNativeTimer,
    &RemoveNativeTimer,
    &AddNativeDelayedCallback,
    &NativeTimerApi,
};

TSPlayerApi const PlayerApi = {
    &GetPlayerGuid,
    &SendPlayerBroadcastMessage,
    &GetPlayerClass,
    &GetPlayerMapId,
    &GetPlayerQuestRewardTalentPoints,
    &GetPlayerTeam,
    &AddPlayerGossipItem,
    &CompletePlayerGossip,
    &SendPlayerGossipText,
    &ClearPlayerGossip,
    &SetPlayerTaxiCheat,
    &SendPlayerWorldState,
    &SendPlayerCustomPacket,
    &SendPlayerAddonMessage,
    &TeleportPlayer,
    &PlayerHasItem,
    &GetPlayerSkillValue,
    &SetPlayerSkill,
    &PlayPlayerDirectSound,
    &AddPlayerItem,
    &CompletePlayerQuestObjective,
    &TeachPlayerSpell,
};

TSPacketReadApi const PacketReadApi = {
    &ReadCustomPacketBytes,
    &ReadCustomPacketString,
    &GetCustomPacketSize,
    &ResetCustomPacket,
};

TSUnitApi const UnitApi = {
    &IsPlayerUnit,
    &GetEffectiveUnitOwner,
    &GetUnitGuidLow,
    &GetUnitMapId,
    &GetUnitMap,
    &RespawnCreature,
    &RemoveCreatureCorpse,
    &MapApi,
    &ObjectStateApi,
    &ApplyPlayerOutfit,
    &NativeOutfitApi,
};

TSPlayer WrapPlayer(Player* player)
{
    return TSPlayer(player, &PlayerApi, &UnitApi);
}

TSUnit WrapUnit(Unit* unit)
{
    return TSUnit(unit, &UnitApi, &PlayerApi);
}

TSCreature WrapCreature(Creature* creature)
{
    return TSCreature(creature, &UnitApi, &PlayerApi);
}

TSBattleground WrapBattleground(Battleground* battleground);

template <typename CoreType, typename WrappedType>
class TsWowReloadObjectVisitor
{
public:
    TsWowReloadObjectVisitor(std::function<void(WrappedType)> const& callback, std::uint32_t id) :
        _callback(callback), _id(id) { }

    void Visit(std::unordered_map<ObjectGuid, CoreType*>& objects)
    {
        for (auto const& [guid, object] : objects)
        {
            (void)guid;
            if (_id != UINT32_MAX && object->GetEntry() != _id)
                continue;

            if constexpr (std::is_same_v<CoreType, Creature>)
                _callback(WrapCreature(object));
            else
                _callback(TSGameObject(object));
        }
    }

    template <typename T>
    void Visit(std::unordered_map<ObjectGuid, T*>&) { }

private:
    std::function<void(WrappedType)> const& _callback;
    std::uint32_t _id;
};

void ReloadPlayers(TSEvents::PlayerEvents::OnReloadCallback const& callback)
{
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        (void)guid;
        callback(WrapPlayer(player), false);
    }
}

void ReloadCreatures(TSEvents::CreatureEvents::OnReloadCallback const& callback, std::uint32_t id)
{
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        TsWowReloadObjectVisitor<Creature, TSCreature> worker(callback, id);
        TypeContainerVisitor<decltype(worker), MapStoredObjectTypesContainer> visitor(worker);
        visitor.Visit(map->GetObjectsStore());
    });
}

void ReloadGameObjects(TSEvents::GameObjectEvents::OnReloadCallback const& callback, std::uint32_t id)
{
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        TsWowReloadObjectVisitor<GameObject, TSGameObject> worker(callback, id);
        TypeContainerVisitor<decltype(worker), MapStoredObjectTypesContainer> visitor(worker);
        visitor.Visit(map->GetObjectsStore());
    });
}

void ReloadMaps(TSEvents::MapEvents::OnReloadCallback const& callback, std::uint32_t id)
{
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (id == UINT32_MAX || map->GetId() == id)
            callback(TSMap(map, &MapApi));
    });
}

void ReloadInstances(TSEvents::InstanceEvents::OnReloadCallback const& callback, std::uint32_t id)
{
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (id != UINT32_MAX && map->GetId() != id)
            return;
        if (InstanceMap* instanceMap = map->ToInstanceMap())
            if (InstanceScript* instance = instanceMap->GetInstanceScript())
                callback(TSInstance(instance));
    });
}

void ReloadBattlegrounds(TSEvents::BattlegroundEvents::OnReloadCallback const& callback,
    std::uint32_t id)
{
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (id != UINT32_MAX && map->GetId() != id)
            return;
        if (BattlegroundMap* battlegroundMap = map->ToBattlegroundMap())
            if (Battleground* battleground = battlegroundMap->GetBG())
                callback(WrapBattleground(battleground));
    });
}

TSBattleground WrapBattleground(Battleground* battleground)
{
    return TSBattleground(battleground, battleground ? battleground->FindBgMap() : nullptr, &MapApi,
        &BattlegroundApi, &BattlegroundScoreApi);
}

TSBattlegroundScore WrapBattlegroundScore(BattlegroundScore* score)
{
    return TSBattlegroundScore(score, &BattlegroundScoreApi);
}

class TsWowServerBuffer final : public CustomPacketBuffer
{
public:
    explicit TsWowServerBuffer(Player* player) :
        CustomPacketBuffer(MIN_FRAGMENT_SIZE, BUFFER_QUOTA, MAX_FRAGMENT_SIZE), _player(player) { }

private:
    void OnPacket(CustomPacketRead* packet) override
    {
        ts_events.CustomPacket.Fire(packet->Opcode(), TSPacketRead(packet, &PacketReadApi), WrapPlayer(_player));
    }

    void OnError(CustomPacketResult error) override
    {
        _player->GetSession()->KickPlayer("Custom packet error: " +
            std::to_string(static_cast<std::uint32_t>(error)));
    }

    Player* _player;
};

std::unordered_map<std::uint64_t, std::unique_ptr<TsWowServerBuffer>> CustomPacketBuffers;

LibraryHandle OpenLibrary(std::filesystem::path const& path)
{
#if PLATFORM == PLATFORM_WINDOWS
    return LoadLibraryW(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW);
#endif
}

void* FindSymbol(LibraryHandle library, char const* name)
{
#if PLATFORM == PLATFORM_WINDOWS
    return reinterpret_cast<void*>(GetProcAddress(library, name));
#else
    return dlsym(library, name);
#endif
}

void CloseLibrary(LibraryHandle library)
{
#if PLATFORM == PLATFORM_WINDOWS
    FreeLibrary(library);
#else
    dlclose(library);
#endif
}

void UnloadLivescripts()
{
    ts_events.Clear();
    DetachNativeOutfitReferences();
    NativeObjectState.clear();
    NativeTimers.clear();
    NativeDelayedCallbacks.clear();
    // Lua callbacks own references into the Lua state, so callbacks must be
    // cleared before the state is destroyed.
    UnloadLuaLivescripts();
    CustomPacketBuffers.clear();
    BattlegroundScoreAttributes.clear();
    for (auto const& [path, library] : Libraries)
    {
        CloseLibrary(library);
        LOG_INFO("module.tswow.livescripts", "Unloaded {}", path.string());
    }
    Libraries.clear();
}

void LoadLivescripts()
{
    UnloadLivescripts();

    std::filesystem::path const directory = sConfigMgr->GetOption<std::string>("TSWoW.LivescriptDir", "./lib");
    if (!std::filesystem::exists(directory))
        return;

    for (auto const& entry : std::filesystem::directory_iterator(directory))
    {
        std::filesystem::path const& path = entry.path();
        if (!entry.is_regular_file() || path.extension() != TSWOW_LIBRARY_EXTENSION)
            continue;

        LibraryHandle library = OpenLibrary(path);
        if (!library)
        {
            LOG_ERROR("module.tswow.livescripts", "Could not load {}", path.string());
            continue;
        }

        auto addScripts = reinterpret_cast<AddTsScripts>(FindSymbol(library, "AddTSScripts"));
        if (!addScripts)
        {
            LOG_ERROR("module.tswow.livescripts", "{} does not export AddTSScripts", path.string());
            CloseLibrary(library);
            continue;
        }

        Libraries.emplace(path, library);
        addScripts(&ts_events);
        LOG_INFO("module.tswow.livescripts", "Loaded {}", path.string());
    }

    std::filesystem::path const luaDirectory =
        sConfigMgr->GetOption<std::string>("TSWoW.LuaDir", "./lib/lua");
    if (!LoadLuaLivescripts(luaDirectory))
        UnloadLivescripts();
}

class TsWowWorldScript final : public WorldScript
{
public:
    TsWowWorldScript() : WorldScript("TsWowWorldScript") { }

    void OnOpenStateChange(bool open) override { ts_events.World.OnOpenStateChangeCallbacks.Fire(open); }
    void OnAfterConfigLoad(bool reload) override
    {
        ts_events.World.OnConfigLoadCallbacks.Fire(reload);
        if (reload)
        {
            LoadPermanentTalentQuestRewards();
            LoadPlayerLevelStats();
            LoadLivescripts();
        }
    }
    void OnMotdChange(std::string& motd, LocaleConstant&) override
    {
        ts_events.World.OnMotdChangeCallbacks.Fire(motd);
    }
    void OnShutdownInitiate(ShutdownExitCode code, ShutdownMask mask) override
    {
        ts_events.World.OnShutdownInitiateCallbacks.Fire(code, mask);
    }
    void OnShutdownCancel() override { ts_events.World.OnShutdownCancelCallbacks.Fire(); }
    void OnUpdate(uint32 diff) override
    {
        if (LuaLivescriptsNeedReload())
            LoadLivescripts();
        UpdateLuaLivescripts(diff);
        ts_events.World.OnUpdateCallbacks.Fire(diff, TSMainThreadContext());
    }
    void OnStartup() override
    {
        LoadPermanentTalentQuestRewards();
        LoadPlayerLevelStats();
        LoadLivescripts();
        ts_events.World.OnStartupCallbacks.Fire();
    }
    void OnShutdown() override
    {
        ts_events.World.OnShutdownCallbacks.Fire();
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            (void)accountId;
            if (Player* player = session->GetPlayer())
                ts_events.Player.OnSaveCallbacks.Fire(WrapPlayer(player));
        }
        UnloadLivescripts();
    }
};

class TsWowFormulaScript final : public FormulaScript
{
public:
    TsWowFormulaScript() : FormulaScript("TsWowFormulaScript") { }

    void OnHonorCalculation(float& honor, uint8 level, float multiplier) override
    {
        ts_events.World.OnCalcHonorCallbacks.Fire(TSMutableNumber<float>(&honor), level, multiplier);
    }
    void OnGainCalculation(uint32& gain, Player* player, Unit* unit) override
    {
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            ts_events.Creature.OnCalcGainCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
                TSMutableNumber<uint32>(&gain), WrapPlayer(player));
    }
};

class TsWowAccountScript final : public AccountScript
{
public:
    TsWowAccountScript() : AccountScript("TsWowAccountScript") { }

    void OnAccountLogin(uint32 accountId) override { ts_events.Account.OnAccountLoginCallbacks.Fire(accountId); }
    void OnFailedAccountLogin(uint32 accountId) override
    {
        ts_events.Account.OnFailedAccountLoginCallbacks.Fire(accountId);
    }
    void OnEmailChange(uint32 accountId) override { ts_events.Account.OnEmailChangeCallbacks.Fire(accountId); }
    void OnFailedEmailChange(uint32 accountId) override
    {
        ts_events.Account.OnFailedEmailChangeCallbacks.Fire(accountId);
    }
    void OnPasswordChange(uint32 accountId) override { ts_events.Account.OnPasswordChangeCallbacks.Fire(accountId); }
    void OnFailedPasswordChange(uint32 accountId) override
    {
        ts_events.Account.OnFailedPasswordChangeCallbacks.Fire(accountId);
    }
};

class TsWowAuctionScript final : public AuctionHouseScript
{
public:
    TsWowAuctionScript() : AuctionHouseScript("TsWowAuctionScript") { }

    void OnAuctionAdd(AuctionHouseObject* house, AuctionEntry* entry) override
    {
        ts_events.Auction.OnAuctionAddCallbacks.Fire(TSAuctionHouseObject(house), TSAuctionEntry(entry));
    }
    void OnAuctionRemove(AuctionHouseObject* house, AuctionEntry* entry) override
    {
        ts_events.Auction.OnAuctionRemoveCallbacks.Fire(TSAuctionHouseObject(house), TSAuctionEntry(entry));
    }
    void OnAuctionSuccessful(AuctionHouseObject* house, AuctionEntry* entry) override
    {
        ts_events.Auction.OnAuctionSuccessfulCallbacks.Fire(TSAuctionHouseObject(house), TSAuctionEntry(entry));
    }
    void OnAuctionExpire(AuctionHouseObject* house, AuctionEntry* entry) override
    {
        ts_events.Auction.OnAuctionExpireCallbacks.Fire(TSAuctionHouseObject(house), TSAuctionEntry(entry));
    }
};

class TsWowVehicleScript final : public VehicleScript
{
public:
    TsWowVehicleScript() : VehicleScript("TsWowVehicleScript") { }

    void OnInstall(Vehicle* vehicle) override
    {
        ts_events.Vehicle.OnInstallCallbacks.Fire(vehicle->GetBase()->GetEntry(), TSVehicle(vehicle));
    }
    void OnUninstall(Vehicle* vehicle) override
    {
        ts_events.Vehicle.OnUninstallCallbacks.Fire(vehicle->GetBase()->GetEntry(), TSVehicle(vehicle));
    }
    void OnReset(Vehicle* vehicle) override
    {
        ts_events.Vehicle.OnResetCallbacks.Fire(vehicle->GetBase()->GetEntry(), TSVehicle(vehicle));
    }
    void OnAddPassenger(Vehicle* vehicle, Unit* passenger, int8 seatId) override
    {
        seatId = passenger->GetTransSeat();
        _passengerSeats[passenger->GetGUID().GetRawValue()] = static_cast<std::uint8_t>(seatId);
        ts_events.Vehicle.OnAddPassengerCallbacks.Fire(vehicle->GetBase()->GetEntry(), TSVehicle(vehicle),
            WrapUnit(passenger), static_cast<std::uint8_t>(seatId));
    }
    void OnRemovePassenger(Vehicle* vehicle, Unit* passenger) override
    {
        std::uint8_t seatId = static_cast<std::uint8_t>(-1);
        auto const itr = _passengerSeats.find(passenger->GetGUID().GetRawValue());
        if (itr != _passengerSeats.end())
        {
            seatId = itr->second;
            _passengerSeats.erase(itr);
        }
        ts_events.Vehicle.OnRemovePassengerCallbacks.Fire(vehicle->GetBase()->GetEntry(), TSVehicle(vehicle),
            WrapUnit(passenger), seatId);
    }

private:
    std::unordered_map<std::uint64_t, std::uint8_t> _passengerSeats;
};

class TsWowGuildScript final : public GuildScript
{
public:
    TsWowGuildScript() : GuildScript("TsWowGuildScript") { }

    void OnAddMember(Guild* guild, Player* player, uint8& rank) override
    {
        ts_events.Guild.OnAddMemberCallbacks.Fire(TSGuild(guild), WrapPlayer(player),
            TSMutableNumber<uint8>(&rank));
    }
    void OnRemoveMember(Guild* guild, Player* player, bool disbanding, bool kicked) override
    {
        ts_events.Guild.OnRemoveMemberCallbacks.Fire(TSGuild(guild), WrapPlayer(player), disbanding, kicked);
    }
    void OnMOTDChanged(Guild* guild, std::string const& motd) override
    {
        ts_events.Guild.OnMOTDChangedCallbacks.Fire(TSGuild(guild), motd);
    }
    void OnInfoChanged(Guild* guild, std::string const& info) override
    {
        ts_events.Guild.OnInfoChangedCallbacks.Fire(TSGuild(guild), info);
    }
    void OnCreate(Guild* guild, Player* leader, std::string const& name) override
    {
        ts_events.Guild.OnCreateCallbacks.Fire(TSGuild(guild), WrapPlayer(leader), name);
    }
    void OnDisband(Guild* guild) override { ts_events.Guild.OnDisbandCallbacks.Fire(TSGuild(guild)); }
    void OnMemberWitdrawMoney(Guild* guild, Player* player, uint32& amount, bool repair) override
    {
        ts_events.Guild.OnMemberWitdrawMoneyCallbacks.Fire(TSGuild(guild), WrapPlayer(player),
            TSMutableNumber<uint32>(&amount), repair);
    }
    void OnMemberDepositMoney(Guild* guild, Player* player, uint32& amount) override
    {
        ts_events.Guild.OnMemberDepositMoneyCallbacks.Fire(TSGuild(guild), WrapPlayer(player),
            TSMutableNumber<uint32>(&amount));
    }
    void OnEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType player1,
        ObjectGuid::LowType player2, uint8 rank) override
    {
        ts_events.Guild.OnEventCallbacks.Fire(TSGuild(guild), eventType, player1, player2, rank);
    }
    void OnBankEvent(Guild* guild, uint8 eventType, uint8 tabId, ObjectGuid::LowType playerGuid,
        uint32 itemOrMoney, uint16 itemStackCount, uint8 destTabId) override
    {
        ts_events.Guild.OnBankEventCallbacks.Fire(TSGuild(guild), eventType, tabId, playerGuid,
            itemOrMoney, itemStackCount, destTabId);
    }
};

class TsWowGroupScript final : public GroupScript
{
public:
    TsWowGroupScript() : GroupScript("TsWowGroupScript") { }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        ts_events.Group.OnAddMemberCallbacks.Fire(TSGroup(group), guid.GetRawValue());
    }
    void OnInviteMember(Group* group, ObjectGuid guid) override
    {
        ts_events.Group.OnInviteMemberCallbacks.Fire(TSGroup(group), guid.GetRawValue());
    }
    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker,
        char const* reason) override
    {
        std::string const removalReason = reason ? reason : "";
        ts_events.Group.OnRemoveMemberCallbacks.Fire(TSGroup(group), guid.GetRawValue(), method,
            kicker.GetRawValue(), removalReason);
    }
    void OnChangeLeader(Group* group, ObjectGuid newLeader, ObjectGuid oldLeader) override
    {
        ts_events.Group.OnChangeLeaderCallbacks.Fire(TSGroup(group), newLeader.GetRawValue(),
            oldLeader.GetRawValue());
    }
    void OnDisband(Group* group) override { ts_events.Group.OnDisbandCallbacks.Fire(TSGroup(group)); }
};

class TsWowGameEventScript final : public GameEventScript
{
public:
    TsWowGameEventScript() : GameEventScript("TsWowGameEventScript") { }

    void OnStart(uint16 eventId) override { ts_events.GameEvent.OnStartCallbacks.Fire(eventId, eventId); }
    void OnStop(uint16 eventId) override { ts_events.GameEvent.OnEndCallbacks.Fire(eventId, eventId); }
    void OnEventCheck(uint16 eventId) override { ts_events.GameEvent.OnUpdateStateCallbacks.Fire(eventId, eventId); }
};

class TsWowAreaTriggerScript final : public ALEScript
{
public:
    TsWowAreaTriggerScript() : ALEScript("TsWowAreaTriggerScript") { }

    bool CanAreaTrigger(Player* player, AreaTrigger const* trigger) override
    {
        bool cancel = false;
        ts_events.AreaTrigger.OnTriggerCallbacks.Fire(trigger->entry,
            TSAreaTriggerEntry(const_cast<AreaTrigger*>(trigger)), WrapPlayer(player),
            TSMutable<bool, bool>(&cancel));
        return cancel;
    }

    void OnWeatherChange(Weather* weather, WeatherState, float) override
    {
        Map* map = weather->GetMap();
        ts_events.Map.OnWeatherChangeCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), TSWeather(weather));
    }

    void OnWeatherUpdate(Weather* weather, uint32) override
    {
        Map* map = weather->GetMap();
        ts_events.Map.OnWeatherUpdateCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), TSWeather(weather));
    }
};

class TsWowPlayerScript final : public PlayerScript
{
public:
    TsWowPlayerScript() : PlayerScript("TsWowPlayerScript") { }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        ApplyPlayerLevelStats(player);
        ts_events.Player.OnLevelChangedCallbacks.Fire(WrapPlayer(player), oldLevel);
    }
    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        ts_events.Player.OnPVPKillCallbacks.Fire(WrapPlayer(killer), WrapPlayer(killed));
        if (Battleground* battleground = killer->GetBattleground())
            ts_events.Battleground.OnKillPlayerCallbacks.Fire(battleground->GetMapId(),
                WrapBattleground(battleground), WrapPlayer(killed), WrapPlayer(killer));
    }
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        ts_events.Player.OnCreatureKillCallbacks.Fire(WrapPlayer(killer), WrapCreature(killed));
        if (Battleground* battleground = killer->GetBattleground())
            ts_events.Battleground.OnKillCreatureCallbacks.Fire(battleground->GetMapId(),
                WrapBattleground(battleground), WrapCreature(killed), WrapPlayer(killer));
    }
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        ts_events.Player.OnPlayerKilledByCreatureCallbacks.Fire(WrapCreature(killer), WrapPlayer(killed));
    }
    void OnPlayerFreeTalentPointsChanged(Player* player, uint32 points) override
    {
        ts_events.Player.OnFreeTalentPointsChangedCallbacks.Fire(WrapPlayer(player), points);
    }
    void OnPlayerTalentsReset(Player* player, bool noCost) override
    {
        ts_events.Player.OnTalentsResetCallbacks.Fire(WrapPlayer(player), noCost);
    }
    void OnPlayerBeforeTalentsReset(Player* player, bool& noCost) override
    {
        ts_events.Player.OnTalentsResetEarlyCallbacks.Fire(WrapPlayer(player),
            TSMutable<bool, bool>(&noCost));
    }
    void OnPlayerAfterTalentsReset(Player* player, bool noCost) override
    {
        ts_events.Player.OnTalentsResetLateCallbacks.Fire(WrapPlayer(player), noCost);
    }
    bool CanPlayerLearnTalentSpell(Player* player, TalentEntry const* talent, uint32 rank,
        SpellInfo const* spellInfo) override
    {
        bool cancel = false;
        std::uint32_t const spellId = spellInfo->Id;
        ts_events.Player.OnLearnTalentCallbacks.Fire(WrapPlayer(player), talent->TalentTab, talent->TalentID,
            rank, spellId, TSMutable<bool, bool>(&cancel));
        ts_events.Spell.OnLearnTalentCallbacks.Fire(spellId,
            TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), WrapPlayer(player), talent->TalentTab,
            talent->TalentID, rank, spellId, TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        ts_events.Player.OnMoneyChangedCallbacks.Fire(WrapPlayer(player), TSMutableNumber<int32>(&amount));
    }
    void OnPlayerMoneyLimit(Player* player, int32 amount) override
    {
        ts_events.Player.OnMoneyLimitCallbacks.Fire(WrapPlayer(player), amount);
    }
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8) override
    {
        ts_events.Player.OnGiveXPCallbacks.Fire(WrapPlayer(player), TSMutableNumber<uint32>(&amount), WrapUnit(victim));
    }
    bool OnPlayerReputationChange(Player* player, uint32 faction, int32& standing,
        bool incremental) override
    {
        ts_events.Player.OnReputationChangeCallbacks.Fire(WrapPlayer(player), faction,
            TSMutableNumber<int32>(&standing), incremental);
        return true;
    }
    void OnPlayerDuelRequest(Player* target, Player* challenger) override
    {
        ts_events.Player.OnDuelRequestCallbacks.Fire(WrapPlayer(target), WrapPlayer(challenger));
    }
    void OnPlayerDuelStart(Player* player1, Player* player2) override
    {
        ts_events.Player.OnDuelStartCallbacks.Fire(WrapPlayer(player1), WrapPlayer(player2));
    }
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        ts_events.Player.OnDuelEndCallbacks.Fire(WrapPlayer(winner), WrapPlayer(loser), type);
    }
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& language,
        std::string& message) override
    {
        if (type == CHAT_MSG_SAY)
            ts_events.Player.OnSayCallbacks.Fire(WrapPlayer(player), TSMutableString(&message), type, language);
    }
    void OnPlayerEmote(Player* player, uint32 emote) override
    {
        ts_events.Player.OnEmoteCallbacks.Fire(WrapPlayer(player), emote);
    }
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid) override
    {
        ts_events.Player.OnTextEmoteCallbacks.Fire(WrapPlayer(player), textEmote, emoteNum,
            guid.GetRawValue());
    }
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        ts_events.Player.OnSpellCastCallbacks.Fire(WrapPlayer(player), TSSpell(spell, WrapUnit(player)), skipCheck);
    }
    void OnPlayerReleasedGhost(Player* player) override
    {
        ts_events.Player.OnPlayerRepopCallbacks.Fire(WrapPlayer(player));
    }
    void OnPlayerAfterUpdateMaxHealth(Player* player, float& value) override
    {
        ts_events.Player.OnUpdateMaxHealthCallbacks.Fire(WrapPlayer(player), TSMutableNumber<float>(&value));
    }
    void OnPlayerAfterUpdateMaxPower(Player* player, Powers& power, float& value) override
    {
        float const bonus = power == POWER_MANA && player->GetCreatePowers(power) > 0
            ? player->GetManaBonusFromIntellect() : 0.0f;
        ts_events.Player.OnUpdateMaxPowerCallbacks.Fire(WrapPlayer(player), TSMutableNumber<float>(&value),
            static_cast<std::int8_t>(power), bonus);
    }
    void OnPlayerFloatStatCalculation(Player* player, PlayerStatCalculation type, float& value,
        float argument, float secondaryArgument) override
    {
        TSPlayer const wrapped = WrapPlayer(player);
        TSMutableNumber<float> const mutableValue(&value);
        switch (type)
        {
            case PlayerStatCalculation::Resistance:
                ts_events.Player.OnUpdateResistanceCallbacks.Fire(wrapped, mutableValue,
                    static_cast<std::uint32_t>(argument));
                break;
            case PlayerStatCalculation::Armor:
                ts_events.Player.OnUpdateArmorCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::AttackPower:
                ts_events.Player.OnUpdateAttackPowerCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::RangedAttackPower:
                ts_events.Player.OnUpdateRangedAttackPowerCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::BlockPercentage:
                ts_events.Player.OnUpdateBlockPercentageCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::CritPercentage:
                ts_events.Player.OnUpdateCritCallbacks.Fire(wrapped, mutableValue,
                    static_cast<std::uint32_t>(argument));
                break;
            case PlayerStatCalculation::ParryPercentage:
                ts_events.Player.OnUpdateParryPercentageCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::DodgePercentage:
                ts_events.Player.OnUpdateDodgePercentageCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::SpellCrit:
                ts_events.Player.OnUpdateSpellCritCallbacks.Fire(wrapped, mutableValue,
                    static_cast<std::uint32_t>(argument));
                break;
            case PlayerStatCalculation::MeleeHit:
                ts_events.Player.OnUpdateMeleeHitChancesCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::RangedHit:
                ts_events.Player.OnUpdateRangedHitChancesCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::SpellHit:
                ts_events.Player.OnUpdateSpellHitChancesCallbacks.Fire(wrapped, mutableValue);
                break;
            case PlayerStatCalculation::RuneRegen:
                ts_events.Player.OnUpdateRuneRegenCallbacks.Fire(wrapped, mutableValue,
                    static_cast<std::uint32_t>(argument));
                break;
            case PlayerStatCalculation::StaminaHealthBonus:
                ts_events.Player.OnCalcStaminaHealthBonusCallbacks.Fire(wrapped, mutableValue,
                    argument, secondaryArgument);
                break;
            case PlayerStatCalculation::IntellectManaBonus:
                ts_events.Player.OnCalcIntellectManaBonusCallbacks.Fire(wrapped, mutableValue,
                    argument, secondaryArgument);
                break;
            case PlayerStatCalculation::ArmorPenetration:
            case PlayerStatCalculation::Expertise:
            case PlayerStatCalculation::ShieldBlock:
                break;
        }
    }
    void OnPlayerIntStatCalculation(Player* player, PlayerStatCalculation type, int32& value,
        uint32 argument, Item* item) override
    {
        if (type == PlayerStatCalculation::ArmorPenetration)
            ts_events.Player.OnUpdateArmorPenetrationCallbacks.Fire(WrapPlayer(player),
                TSMutableNumber<int32>(&value));
        else if (type == PlayerStatCalculation::Expertise)
            ts_events.Player.OnUpdateExpertiseCallbacks.Fire(WrapPlayer(player), TSMutableNumber<int32>(&value),
                argument, TSItem(item));
    }
    void OnPlayerUIntStatCalculation(Player* player, PlayerStatCalculation type, uint32& value) override
    {
        if (type == PlayerStatCalculation::ShieldBlock)
            ts_events.Player.OnUpdateShieldBlockCallbacks.Fire(WrapPlayer(player),
                TSMutableNumber<uint32>(&value));
        else if (type == PlayerStatCalculation::GlyphSlots)
            ts_events.Player.OnGlyphInitForLevelCallbacks.Fire(WrapPlayer(player),
                TSMutableNumber<uint32>(&value));
    }
    void OnPlayerManaRegenCalculation(Player* player, float& spiritRegen, float& flatRegen,
        int32& interruptPercent) override
    {
        ts_events.Player.OnUpdateManaRegenCallbacks.Fire(WrapPlayer(player),
            TSMutableNumber<float>(&spiritRegen), TSMutableNumber<float>(&flatRegen),
            TSMutableNumber<int32>(&interruptPercent));
    }
    void OnPlayerGetReputationPriceDiscount(Player const* player, Creature const* creature,
        float& discount) override
    {
        FactionTemplateEntry const* faction = creature ? creature->GetFactionTemplateEntry() : nullptr;
        ts_events.Player.OnReputationPriceDiscountCallbacks.Fire(
            WrapPlayer(const_cast<Player*>(player)), TSFactionTemplate(const_cast<FactionTemplateEntry*>(faction)),
            WrapCreature(const_cast<Creature*>(creature)), TSMutableNumber<float>(&discount));
    }
    void OnPlayerGetReputationPriceDiscount(Player const* player, FactionTemplateEntry const* faction,
        float& discount) override
    {
        ts_events.Player.OnReputationPriceDiscountCallbacks.Fire(
            WrapPlayer(const_cast<Player*>(player)), TSFactionTemplate(const_cast<FactionTemplateEntry*>(faction)),
            TSCreature(), TSMutableNumber<float>(&discount));
    }
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Player* receiver) override
    {
        ts_events.Player.OnWhisperCallbacks.Fire(WrapPlayer(player), WrapPlayer(receiver),
            TSMutableString(&message), type, language);
        return true;
    }
    bool CanPlayerGossipSelect(Player* player, uint32, uint32 sender, uint32 action) override
    {
        bool cancel = false;
        TSPlayer const wrapped = WrapPlayer(player);
        ts_events.Player.OnGossipSelectCallbacks.Fire(wrapped, wrapped, sender, action,
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    bool CanPlayerGossipSelectCode(Player* player, uint32, uint32 sender, uint32 action,
        char const* code) override
    {
        bool cancel = false;
        TSPlayer const wrapped = WrapPlayer(player);
        std::string const text = code ? code : "";
        ts_events.Player.OnGossipSelectCodeCallbacks.Fire(wrapped, wrapped, sender, action, text,
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Group* group) override
    {
        ts_events.Player.OnChatGroupCallbacks.Fire(WrapPlayer(player), TSGroup(group),
            TSMutableString(&message), type, language);
        return true;
    }
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Guild* guild) override
    {
        ts_events.Player.OnChatGuildCallbacks.Fire(WrapPlayer(player), TSGuild(guild),
            TSMutableString(&message), type, language);
        return true;
    }
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Channel* channel) override
    {
        ts_events.Player.OnChatCallbacks.Fire(WrapPlayer(player), TSChannel(channel),
            TSMutableString(&message), type, language);
        return true;
    }
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        ts_events.Achievement.OnCompleteCallbacks.Fire(achievement->ID, WrapPlayer(player),
            TSAchievementEntry(const_cast<AchievementEntry*>(achievement)));
    }
    void OnPlayerLogin(Player* player) override
    {
        ApplyPlayerLevelStats(player);
        ts_events.Player.OnLoginCallbacks.Fire(WrapPlayer(player), player->HasAtLoginFlag(AT_LOGIN_FIRST));
        if (Battleground* battleground = player->GetBattleground())
            ts_events.Battleground.OnPlayerLoginCallbacks.Fire(battleground->GetMapId(),
                WrapBattleground(battleground), WrapPlayer(player));
    }
    void OnPlayerLogout(Player* player) override
    {
        if (Battleground* battleground = player->GetBattleground())
            ts_events.Battleground.OnPlayerLogoutCallbacks.Fire(battleground->GetMapId(),
                WrapBattleground(battleground), WrapPlayer(player));
        ts_events.Player.OnLogoutCallbacks.Fire(WrapPlayer(player));
        ClearLuaEntityState(player);
        ClearNativeObjectState(player);
        CustomPacketBuffers.erase(player->GetGUID().GetRawValue());
    }
    void OnPlayerCreate(Player* player) override
    {
        ApplyPlayerLevelStats(player);
        ts_events.Player.OnCreateCallbacks.Fire(WrapPlayer(player));
    }
    void OnPlayerDelete(ObjectGuid guid, uint32 accountId) override
    {
        ts_events.Player.OnDeleteCallbacks.Fire(guid.GetRawValue(), accountId);
    }
    void OnPlayerFailedDelete(ObjectGuid guid, uint32 accountId) override
    {
        ts_events.Player.OnFailedDeleteCallbacks.Fire(guid.GetRawValue(), accountId);
    }
    void OnPlayerSave(Player* player) override { ts_events.Player.OnSaveCallbacks.Fire(WrapPlayer(player)); }
    void OnPlayerBindToInstance(Player* player, Difficulty difficulty, uint32 mapId, bool permanent) override
    {
        InstancePlayerBind const* bind = sInstanceSaveMgr->PlayerGetBoundInstance(player->GetGUID(), mapId,
            difficulty);
        std::uint32_t const instanceId = bind && bind->save ? bind->save->GetInstanceId() : 0;
        ts_events.Player.OnBindToInstanceCallbacks.Fire(WrapPlayer(player), mapId, instanceId, permanent,
            static_cast<std::uint8_t>(difficulty));
    }
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        ts_events.Player.OnUpdateZoneCallbacks.Fire(WrapPlayer(player), newZone, newArea);
    }
    void OnPlayerMapChanged(Player* player) override
    {
        ts_events.Player.OnMapChangedCallbacks.Fire(WrapPlayer(player));
    }
    void OnPlayerMovieComplete(Player* player, uint32 movieId) override
    {
        ts_events.Player.OnMovieCompleteCallbacks.Fire(WrapPlayer(player), movieId);
    }
    void OnPlayerCalculateTalentsPoints(Player const* player, uint32& points) override
    {
        for (auto const& [questId, reward] : PermanentTalentQuestRewards)
            if (player->GetQuestRewardStatus(questId))
                points += reward;
        ts_events.Player.OnCalcTalentPointsCallbacks.Fire(WrapPlayer(const_cast<Player*>(player)),
            TSMutableNumber<uint32>(&points));
    }
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (PermanentTalentQuestRewards.find(quest->GetQuestId()) != PermanentTalentQuestRewards.end())
            player->InitTalentForLevel();
    }
    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& xpValue) override
    {
        ts_events.Quest.OnRewardXPCallbacks.Fire(quest->GetQuestId(),
            TSQuest(const_cast<Quest*>(quest)), WrapPlayer(player), TSMutableNumber<uint32>(&xpValue));
    }
    void OnPlayerQuestStatusChanged(Player* player, Quest const* quest) override
    {
        TSPlayer wrappedPlayer = WrapPlayer(player);
        TSQuest wrappedQuest(const_cast<Quest*>(quest));
        ts_events.Player.OnQuestStatusChangeCallbacks.Fire(wrappedPlayer, quest->GetQuestId());
        ts_events.Quest.OnStatusChangedCallbacks.Fire(quest->GetQuestId(), wrappedQuest, wrappedPlayer);
    }
    void OnPlayerQuestObjectiveProgress(Player* player, Quest const* quest, uint32 objectiveIndex,
        uint16 progress) override
    {
        TSPlayer wrappedPlayer = WrapPlayer(player);
        TSQuest wrappedQuest(const_cast<Quest*>(quest));
        ts_events.Player.OnQuestObjectiveProgressCallbacks.Fire(wrappedPlayer, wrappedQuest,
            objectiveIndex, progress);
        ts_events.Quest.OnObjectiveProgressCallbacks.Fire(quest->GetQuestId(), wrappedQuest,
            wrappedPlayer, objectiveIndex, progress);
    }
    bool OnPlayerCanUseItem(Player* player, ItemTemplate const* itemTemplate,
        InventoryResult& result) override
    {
        std::uint32_t mutableResult = result;
        ts_events.Item.OnCanUseTypeCallbacks.Fire(itemTemplate->ItemId,
            TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), WrapPlayer(player),
            TSMutableNumber<std::uint32_t>(&mutableResult));
        result = static_cast<InventoryResult>(mutableResult);
        return result == EQUIP_ERR_OK;
    }
    bool OnPlayerCanEquipItem(Player* player, uint8 slot, uint16&, Item* item, bool swap,
        bool notLoading) override
    {
        std::uint32_t result = EQUIP_ERR_OK;
        ts_events.Item.OnCanEquipCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player), slot,
            swap, notLoading, TSMutableNumber<std::uint32_t>(&result));
        return result == EQUIP_ERR_OK;
    }
    bool OnPlayerCanUnequipItem(Player* player, uint16 position, bool swap) override
    {
        Item* item = player->GetItemByPos(position);
        if (!item)
            return true;

        std::uint32_t result = EQUIP_ERR_OK;
        ts_events.Item.OnUnequipCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player), swap,
            TSMutableNumber<std::uint32_t>(&result));
        return result == EQUIP_ERR_OK;
    }
    void OnPlayerItemLifecycle(Player* player, PlayerItemLifecycleEvent type, Item* item,
        ItemTemplate const* itemTemplate, WorldObject* lootedObject, uint8 bag, uint8 slot, bool flag,
        uint32* result, int32* signedResult, bool* boolResult) override
    {
        switch (type)
        {
            case PlayerItemLifecycleEvent::CanChangeEquipState:
                ts_events.Item.OnCanChangeEquipStateCallbacks.Fire(itemTemplate->ItemId,
                    TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), TSMutable<bool, bool>(boolResult));
                break;
            case PlayerItemLifecycleEvent::Bank:
                ts_events.Item.OnBankCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
                    bag, slot, flag, TSMutableNumber<std::uint32_t>(result));
                break;
            case PlayerItemLifecycleEvent::CanUse:
                ts_events.Item.OnCanUseCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
                    TSMutableNumber<std::uint32_t>(result));
                break;
            case PlayerItemLifecycleEvent::LFGRollEarly:
                ts_events.Item.OnLFGRollEarlyCallbacks.Fire(itemTemplate->ItemId,
                    TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), TSWorldObject(lootedObject),
                    WrapPlayer(player), TSMutableNumber<std::int32_t>(signedResult));
                break;
            case PlayerItemLifecycleEvent::DestroyEarly:
                ts_events.Item.OnDestroyEarlyCallbacks.Fire(item->GetEntry(), TSItem(item),
                    WrapPlayer(player), TSMutable<bool, bool>(boolResult));
                break;
        }
    }
    void OnPlayerFormulaCalculation(Player* player, PlayerFormulaEvent type, uint8* byteValue,
        float* floatValue, int32* intValue, uint32 argument1, uint32 argument2, uint32 argument3,
        uint32 argument4, uint32 argument5, bool flag) override
    {
        TSPlayer const wrapped = WrapPlayer(player);
        switch (type)
        {
            case PlayerFormulaEvent::GrayLevel:
                ts_events.Player.OnCalcGreyLevelCallbacks.Fire(wrapped,
                    TSMutableNumber<std::uint8_t>(byteValue));
                break;
            case PlayerFormulaEvent::ZeroDifference:
                ts_events.Player.OnCalcZeroDiffCallbacks.Fire(wrapped,
                    TSMutableNumber<std::uint8_t>(byteValue));
                break;
            case PlayerFormulaEvent::GroupGain:
                ts_events.Player.OnCalcGroupGainCallbacks.Fire(wrapped,
                    TSMutableNumber<float>(floatValue), argument1, flag);
                break;
            case PlayerFormulaEvent::SkillGainChance:
                ts_events.Player.OnCalcSkillGainChanceCallbacks.Fire(wrapped,
                    TSMutableNumber<std::int32_t>(intValue), static_cast<std::int32_t>(argument1),
                    static_cast<std::int32_t>(argument2), static_cast<std::int32_t>(argument3),
                    static_cast<std::int32_t>(argument4), static_cast<std::int32_t>(argument5));
                break;
        }
    }

    void OnPlayerTradeCompleted(Player* player, Player* trader, Item* const* playerItems,
        Item* const* traderItems, uint8 itemCount, uint32 playerMoney,
        uint32 traderMoney) override
    {
        TSArray<TSItem> mine;
        TSArray<TSItem> theirs;
        for (uint8 index = 0; index < itemCount; ++index)
        {
            if (playerItems[index])
                mine.push(TSItem(playerItems[index]));
            if (traderItems[index])
                theirs.push(TSItem(traderItems[index]));
        }
        ts_events.Player.OnTradeCompletedCallbacks.Fire(WrapPlayer(player), WrapPlayer(trader),
            mine, theirs, playerMoney, traderMoney);
    }
    void OnPlayerLootLifecycle(Player* player, PlayerLootLifecycleEvent type, Item* item,
        Loot* loot, WorldObject* source, LootItem* lootItem, uint32 lootType) override
    {
        TSPlayer const wrappedPlayer = WrapPlayer(player);
        switch (type)
        {
            case PlayerLootLifecycleEvent::GenerateItemLoot:
                ts_events.Player.OnGenerateItemLootCallbacks.Fire(wrappedPlayer, TSItem(item),
                    TSLoot(loot), lootType);
                break;
            case PlayerLootLifecycleEvent::LootCorpse:
                ts_events.Player.OnLootCorpseCallbacks.Fire(wrappedPlayer, TSCorpse(source));
                break;
            case PlayerLootLifecycleEvent::TakenAsLoot:
                ts_events.Item.OnTakenAsLootCallbacks.Fire(item->GetEntry(), TSItem(item),
                    TSLootItem(lootItem), TSLoot(loot), wrappedPlayer);
                break;
        }
    }
    void OnPlayerItemEquipped(Player* player, Item* item, uint8 slot, bool isMerge) override
    {
        ts_events.Item.OnEquipCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            slot, isMerge);
    }
    void OnPlayerGetTrainerSpellState(Player const* player, uint32 trainerId, uint32 spellId,
        Trainer::SpellState& state) override
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return;

        bool allowTrain = state != Trainer::SpellState::Unavailable;
        ts_events.Spell.OnTrainerSendCallbacks.Fire(spellId,
            TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), trainerId,
            WrapPlayer(const_cast<Player*>(player)), TSMutable<bool, bool>(&allowTrain));
        if (!allowTrain)
            state = Trainer::SpellState::Unavailable;
    }
    void OnPlayerGetFeralApBonus(Player*, int32& feralBonus, int32 dpsMod,
        ItemTemplate const* itemTemplate, ScalingStatValuesEntry const*) override
    {
        ts_events.Item.OnCalculateFeralAttackPowerCallbacks.Fire(itemTemplate->ItemId,
            TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), dpsMod,
            TSMutableNumber<std::int32_t>(&feralBonus));
    }
};

class TsWowMiscScript final : public MiscScript
{
public:
    TsWowMiscScript() : MiscScript("TsWowMiscScript") { }

    void OnConstructPlayer(Player* player) override
    {
        ts_events.Player.OnCreateEarlyCallbacks.Fire(WrapPlayer(player));
    }
};

class TsWowMailScript final : public MailScript
{
public:
    TsWowMailScript() : MailScript("TsWowMailScript") { }

    void OnBeforeMailDraftSendMailTo(MailDraft* draft, MailReceiver const&, MailSender const& sender,
        MailCheckMask&, uint32& deliveryDelay, uint32&, bool&, bool&) override
    {
        Player* player = ObjectAccessor::FindPlayerByLowGUID(sender.GetSenderId());
        ts_events.Player.OnSendMailCallbacks.Fire(WrapPlayer(player), TSMailDraft(draft),
            TSMutableNumber<uint32>(&deliveryDelay));
    }
};

void RewriteNativeOutfitPacket(WorldPacket const& source)
{
    if (source.GetOpcode() != SMSG_MIRRORIMAGE_DATA || source.size() < sizeof(std::uint64_t))
        return;
    std::uint64_t guid = 0;
    std::memcpy(&guid, source.contents(), sizeof(guid));
    TSOutfitData outfit;
    {
        std::lock_guard<std::mutex> lock(NativeOutfitsMutex);
        auto found = NativeOutfits.find(guid);
        if (found == NativeOutfits.end())
            return;
        if (!found->second)
            return;
        outfit = *found->second;
    }

    WorldPacket replacement(SMSG_MIRRORIMAGE_DATA, 68);
    replacement << ObjectGuid(guid);
    replacement << outfit.DisplayId << outfit.Race << outfit.Gender << outfit.Class;
    replacement << outfit.Skin << outfit.Face << outfit.HairStyle << outfit.HairColor << outfit.FacialStyle;
    replacement << static_cast<std::uint32_t>(outfit.Guild);
    static constexpr std::array<std::uint8_t, 11> slots{{ 0, 2, 3, 4, 5, 6, 7, 8, 9, 14, 18 }};
    for (std::uint8_t slot : slots)
        replacement << outfit.ItemDisplays[slot];
    const_cast<WorldPacket&>(source) = std::move(replacement);
}

class TsWowServerScript final : public ServerScript
{
public:
    TsWowServerScript() : ServerScript("TsWowServerScript") { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        ts_events.WorldPacket.OnReceiveCallbacks.Fire(packet.GetOpcode(), packet.GetOpcode(),
            TSWorldPacket(const_cast<WorldPacket*>(&packet), &WorldPacketApi), WrapPlayer(player));

        if (packet.GetOpcode() != CLIENT_TO_SERVER_OPCODE || packet.size() <= sizeof(CustomPacketHeader) ||
            packet.size() > MAX_FRAGMENT_SIZE)
            return true;

        if (!player)
            return true;

        CustomPacketHeader header;
        std::memcpy(&header, packet.contents(), sizeof(header));
        if (!header.totalFrags || header.fragmentId >= header.totalFrags)
            return true;

        std::vector<char> bytes(packet.size());
        std::memcpy(bytes.data(), packet.contents(), packet.size());
        std::uint64_t const guid = player->GetGUID().GetRawValue();
        auto& buffer = CustomPacketBuffers[guid];
        if (!buffer)
            buffer = std::make_unique<TsWowServerBuffer>(player);
        buffer->ReceivePacket(static_cast<chunkSize_t>(bytes.size()), bytes.data());
        return false;
    }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        RewriteNativeOutfitPacket(packet);
        ts_events.WorldPacket.OnSendCallbacks.Fire(packet.GetOpcode(),
            TSWorldPacket(const_cast<WorldPacket*>(&packet), &WorldPacketApi),
            WrapPlayer(session ? session->GetPlayer() : nullptr));
        return true;
    }
};

class TsWowCommandScript final : public AllCommandScript
{
public:
    TsWowCommandScript() : AllCommandScript("TsWowCommandScript") { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view commandText) override
    {
        if (commandText == "reload livescripts")
        {
            LoadLivescripts();
            handler.SendSysMessage("TSWoW livescripts reloaded.");
            return false;
        }

        Player* player = handler.GetPlayer();
        if (!player)
            return true;

        std::string command(commandText);
        bool found = false;
        ts_events.Player.OnCommandCallbacks.Fire(WrapPlayer(player), TSMutableString(&command),
            TSMutable<bool, bool>(&found));

        if (!found && command != commandText)
        {
            LOG_ERROR("module.tswow.livescripts", "A livescript changed command '{}' to '{}', but AzerothCore's "
                "command hook cannot replace parser input", commandText, command);
            return false;
        }

        return !found;
    }
};

class TsWowCreatureScript final : public AllCreatureScript
{
public:
    TsWowCreatureScript() : AllCreatureScript("TsWowCreatureScript") { }

    void OnCreatureFloatStatCalculation(Creature* creature, CreatureStatCalculation type, float& value,
        bool isGuardian, float argument) override
    {
        TSCreature const wrapped = WrapCreature(creature);
        TSMutableNumber<float> const mutableValue(&value);
        switch (type)
        {
            case CreatureStatCalculation::Resistance:
                ts_events.Creature.OnUpdateResistanceCallbacks.Fire(creature->GetEntry(), wrapped, mutableValue,
                    isGuardian, static_cast<std::uint32_t>(argument));
                break;
            case CreatureStatCalculation::Armor:
                ts_events.Creature.OnUpdateArmorCallbacks.Fire(creature->GetEntry(), wrapped, mutableValue,
                    isGuardian);
                break;
            case CreatureStatCalculation::MaxHealth:
                ts_events.Creature.OnUpdateMaxHealthCallbacks.Fire(creature->GetEntry(), wrapped, mutableValue,
                    isGuardian);
                break;
            case CreatureStatCalculation::MaxPower:
                ts_events.Creature.OnUpdateMaxPowerCallbacks.Fire(creature->GetEntry(), wrapped, mutableValue,
                    isGuardian, static_cast<std::int8_t>(argument));
                break;
            case CreatureStatCalculation::LevelArmor:
                ts_events.Creature.OnUpdateLvlDepArmorCallbacks.Fire(creature->GetEntry(), wrapped, mutableValue,
                    argument);
                break;
            case CreatureStatCalculation::LevelMaxHealth:
            case CreatureStatCalculation::LevelMaxMana:
                break;
        }
    }

    void OnCreatureUIntStatCalculation(Creature* creature, CreatureStatCalculation type, uint32& value,
        float modifier, uint32 base) override
    {
        if (type == CreatureStatCalculation::LevelMaxHealth)
            ts_events.Creature.OnUpdateLvlDepMaxHealthCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
                TSMutableNumber<uint32>(&value), modifier, base);
        else if (type == CreatureStatCalculation::LevelMaxMana)
            ts_events.Creature.OnUpdateLvlDepMaxManaCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
                TSMutableNumber<uint32>(&value), static_cast<float>(base));
    }

    void OnCreatureBaseDamageCalculation(Creature* creature, float& minimum, float& maximum,
        float baseDamage) override
    {
        ts_events.Creature.OnUpdateLvlDepBaseDamageCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
            TSMutableNumber<float>(&minimum), TSMutableNumber<float>(&maximum), baseDamage);
    }

    void OnCreatureBaseAttackPowerCalculation(Creature* creature, uint32& attackPower,
        uint32& rangedAttackPower) override
    {
        ts_events.Creature.OnUpdateLvlDepAttackPowerCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
            TSMutableNumber<uint32>(&attackPower), TSMutableNumber<uint32>(&rangedAttackPower));
    }

    void OnCreatureAttackPowerCalculation(Creature* creature, float& base, float& modifier,
        float& multiplier, bool isGuardian, bool ranged) override
    {
        ts_events.Creature.OnUpdateAttackPowerDamageCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
            TSMutableNumber<float>(&base), TSMutableNumber<float>(&modifier),
            TSMutableNumber<float>(&multiplier), isGuardian, ranged);
    }

    void OnCreatureDamageCalculation(Creature* creature, float& minimum, float& maximum,
        bool isGuardian, uint8 attackType) override
    {
        ts_events.Creature.OnUpdateDamagePhysicalCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
            TSMutableNumber<float>(&minimum), TSMutableNumber<float>(&maximum), isGuardian, attackType);
    }

    void OnCreatureLifecycle(Creature* creature, CreatureLifecycleEvent type, WorldObject* primary,
        WorldObject* secondary, uint32 value, uint32 secondaryValue, bool apply,
        SpellInfo const* spellInfo, Loot* loot, ItemTemplate const* itemTemplate,
        bool* mutableResult, uint32* mutableValue) override
    {
        std::uint32_t const entry = creature->GetEntry();
        TSCreature const wrapped = WrapCreature(creature);
        switch (type)
        {
            case CreatureLifecycleEvent::JustEnteredCombat:
                ts_events.Creature.OnJustEnteredCombatCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::JustEngagedWith:
                ts_events.Creature.OnJustEngagedWithCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::KilledUnit:
                ts_events.Creature.OnKilledUnitCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::Summoned:
                ts_events.Creature.OnSummonedCallbacks.Fire(entry, wrapped, WrapCreature(primary->ToCreature()));
                break;
            case CreatureLifecycleEvent::IsSummoned:
                ts_events.Creature.OnIsSummonedCallbacks.Fire(entry, wrapped, TSWorldObject(primary));
                break;
            case CreatureLifecycleEvent::SummonDespawn:
                ts_events.Creature.OnSummonDespawnCallbacks.Fire(entry, wrapped, WrapCreature(primary->ToCreature()));
                break;
            case CreatureLifecycleEvent::Despawn:
                ts_events.Creature.OnDespawnCallbacks.Fire(entry, wrapped, TSWorldObject(primary));
                break;
            case CreatureLifecycleEvent::SummonDies:
                ts_events.Creature.OnSummonDiesCallbacks.Fire(entry, wrapped, WrapCreature(primary->ToCreature()),
                    WrapUnit(secondary ? secondary->ToUnit() : nullptr));
                break;
            case CreatureLifecycleEvent::ReceiveEmote:
                ts_events.Creature.OnReceiveEmoteCallbacks.Fire(entry, wrapped, WrapPlayer(primary->ToPlayer()), value);
                break;
            case CreatureLifecycleEvent::CorpseRemoved:
                ts_events.Creature.OnCorpseRemovedCallbacks.Fire(entry, wrapped, value);
                break;
            case CreatureLifecycleEvent::PassengerBoarded:
                ts_events.Creature.OnPassengerBoardedCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()),
                    static_cast<std::int8_t>(value), apply);
                break;
            case CreatureLifecycleEvent::Charmed:
                ts_events.Creature.OnCharmedCallbacks.Fire(entry, wrapped, apply);
                break;
            case CreatureLifecycleEvent::ReachedHome:
                ts_events.Creature.OnReachedHomeCallbacks.Fire(entry, wrapped);
                break;
            case CreatureLifecycleEvent::OwnerAttacked:
                ts_events.Creature.OnOwnerAttackedCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::OwnerAttacks:
                ts_events.Creature.OnOwnerAttacksCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::WaypointStarted:
                ts_events.Creature.OnWaypointStartedCallbacks.Fire(entry, wrapped, value, secondaryValue);
                break;
            case CreatureLifecycleEvent::WaypointReached:
                ts_events.Creature.OnWaypointReachedCallbacks.Fire(entry, wrapped, value, secondaryValue);
                break;
            case CreatureLifecycleEvent::WaypointPathEnded:
                ts_events.Creature.OnWaypointPathEndedCallbacks.Fire(entry, wrapped, value, secondaryValue);
                break;
            case CreatureLifecycleEvent::UpdateAI:
                ts_events.Creature.OnUpdateAICallbacks.Fire(entry, wrapped, value);
                break;
            case CreatureLifecycleEvent::MoveInLOS:
                ts_events.Creature.OnMoveInLOSCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()));
                break;
            case CreatureLifecycleEvent::HitBySpell:
                ts_events.Creature.OnHitBySpellCallbacks.Fire(entry, wrapped, TSWorldObject(primary),
                    TSSpellInfo(const_cast<SpellInfo*>(spellInfo)));
                break;
            case CreatureLifecycleEvent::SpellHitTarget:
                ts_events.Creature.OnSpellHitTargetCallbacks.Fire(entry, wrapped, TSWorldObject(primary),
                    TSSpellInfo(const_cast<SpellInfo*>(spellInfo)));
                break;
            case CreatureLifecycleEvent::SpellClick:
                ts_events.Creature.OnSpellClickCallbacks.Fire(entry, wrapped, WrapUnit(primary->ToUnit()), apply);
                break;
            case CreatureLifecycleEvent::GenerateLoot:
                ts_events.Creature.OnGenerateLootCallbacks.Fire(entry, wrapped, WrapPlayer(primary->ToPlayer()));
                break;
            case CreatureLifecycleEvent::SpellCastFinished:
                ts_events.Creature.OnSpellCastFinishedCallbacks.Fire(entry, wrapped,
                    TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), value);
                break;
            case CreatureLifecycleEvent::JustAppeared:
                ts_events.Creature.OnJustAppearedCallbacks.Fire(entry, wrapped);
                break;
            case CreatureLifecycleEvent::CanGeneratePickPocketLoot:
                ts_events.Creature.OnCanGeneratePickPocketLootCallbacks.Fire(entry, wrapped,
                    WrapPlayer(primary->ToPlayer()), TSMutable<bool, bool>(mutableResult));
                break;
            case CreatureLifecycleEvent::GeneratePickPocketLoot:
                ts_events.Creature.OnGeneratePickPocketLootCallbacks.Fire(entry, wrapped,
                    WrapPlayer(primary->ToPlayer()), TSLoot(loot));
                break;
            case CreatureLifecycleEvent::GenerateSkinningLoot:
                ts_events.Creature.OnGenerateSkinningLootCallbacks.Fire(entry, wrapped,
                    WrapPlayer(primary->ToPlayer()), TSLoot(loot));
                break;
            case CreatureLifecycleEvent::SendVendorItem:
                ts_events.Creature.OnSendVendorItemCallbacks.Fire(entry, wrapped,
                    TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), WrapPlayer(primary->ToPlayer()),
                    TSMutable<bool, bool>(mutableResult));
                break;
            case CreatureLifecycleEvent::CalcBaseGain:
                ts_events.Creature.OnCalcBaseGainCallbacks.Fire(entry, wrapped,
                    TSMutableNumber<std::uint32_t>(mutableValue), WrapPlayer(primary->ToPlayer()));
                break;
            case CreatureLifecycleEvent::CalcColorCode:
            {
                std::uint8_t color = static_cast<std::uint8_t>(*mutableValue);
                ts_events.Creature.OnCalcColorCodeCallbacks.Fire(entry, wrapped,
                    TSMutableNumber<std::uint8_t>(&color), WrapPlayer(primary->ToPlayer()), value,
                    secondaryValue);
                *mutableValue = color;
                break;
            }
            case CreatureLifecycleEvent::MovementInform:
                ts_events.Creature.OnMovementInformCallbacks.Fire(entry, wrapped, value, secondaryValue);
                break;
        }
    }

    bool CanCreatureAddWorld(Creature* creature) override
    {
        bool cancel = false;
        TSCreature const wrapped = WrapCreature(creature);
        ts_events.Creature.OnCreateCallbacks.Fire(creature->GetEntry(), wrapped,
            TSMutable<bool, bool>(&cancel));
        if (Map* map = creature->GetMap())
            ts_events.Map.OnCreatureCreateCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), wrapped,
                TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        bool cancel = false;
        std::uint32_t const entry = creature->GetEntry();
        ts_events.Creature.OnGossipHelloCallbacks.Fire(entry, WrapCreature(creature), WrapPlayer(player),
            TSMutable<bool, bool>(&cancel));
        return cancel;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        bool cancel = false;
        std::uint32_t const entry = creature->GetEntry();
        ts_events.Creature.OnGossipSelectCallbacks.Fire(entry, WrapCreature(creature), WrapPlayer(player),
            sender, action, TSMutable<bool, bool>(&cancel));
        return cancel;
    }

    bool CanCreatureGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action,
        char const* code) override
    {
        bool cancel = false;
        std::uint32_t const entry = creature->GetEntry();
        std::string const text(code);
        ts_events.Creature.OnGossipSelectCodeCallbacks.Fire(entry, WrapCreature(creature), WrapPlayer(player),
            sender, action, text, TSMutable<bool, bool>(&cancel));
        return cancel;
    }

    bool CanCreatureQuestAccept(Player* player, Creature* creature, Quest const* quest) override
    {
        std::uint32_t const entry = creature->GetEntry();
        TSQuest const wrappedQuest(const_cast<Quest*>(quest));
        ts_events.Creature.OnQuestAcceptCallbacks.Fire(entry, WrapCreature(creature), WrapPlayer(player),
            wrappedQuest);
        ts_events.Quest.OnAcceptCallbacks.Fire(quest->GetQuestId(), wrappedQuest, WrapPlayer(player),
            TSObject(creature));
        return false;
    }

    bool CanCreatureQuestReward(Player* player, Creature* creature, Quest const* quest,
        uint32 reward) override
    {
        std::uint32_t const entry = creature->GetEntry();
        TSQuest const wrappedQuest(const_cast<Quest*>(quest));
        ts_events.Creature.OnQuestRewardCallbacks.Fire(entry, WrapCreature(creature), WrapPlayer(player),
            wrappedQuest, reward);
        ts_events.Quest.OnRewardCallbacks.Fire(quest->GetQuestId(), wrappedQuest, WrapPlayer(player),
            TSObject(creature), reward);
        return false;
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        std::uint32_t const entry = creature->GetEntry();
        TSCreature const wrapped = WrapCreature(creature);
        ts_events.Creature.OnRemoveCallbacks.Fire(entry, wrapped);
        if (Map* map = creature->GetMap())
            ts_events.Map.OnCreatureRemoveCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), wrapped);
        ClearLuaEntityState(creature);
        ClearNativeObjectState(creature);
        std::lock_guard<std::mutex> lock(NativeOutfitsMutex);
        NativeOutfits.erase(creature->GetGUID().GetRawValue());
    }
};

class TsWowGameObjectScript final : public AllGameObjectScript
{
public:
    TsWowGameObjectScript() : AllGameObjectScript("TsWowGameObjectScript") { }

    bool CanGameObjectAddWorld(GameObject* gameObject) override
    {
        bool cancel = false;
        TSGameObject const wrapped(gameObject);
        ts_events.GameObject.OnCreateCallbacks.Fire(gameObject->GetEntry(), wrapped,
            TSMutable<bool, bool>(&cancel));
        if (Map* map = gameObject->GetMap())
            ts_events.Map.OnGameObjectCreateCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), wrapped,
                TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanGameObjectUse(GameObject* gameObject, Unit* user) override
    {
        bool cancel = false;
        ts_events.GameObject.OnUseCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapUnit(user), TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    void OnGameObjectDialogStatus(GameObject* gameObject, Player* player) override
    {
        ts_events.GameObject.OnDialogStatusCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player));
    }

    void OnGameObjectGenerateLoot(GameObject* gameObject, Player* player) override
    {
        ts_events.GameObject.OnGenerateLootCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player));
    }

    void OnGameObjectGenerateFishLoot(GameObject* gameObject, Player* player, Loot* loot, bool junk) override
    {
        ts_events.GameObject.OnGenerateFishLootCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), TSLoot(loot), junk);
    }

    void OnGameObjectUpdate(GameObject* gameObject, uint32 diff) override
    {
        ts_events.GameObject.OnUpdateCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject), diff);
    }
    void OnGameObjectDestroyed(GameObject* gameObject, Player* player) override
    {
        ts_events.GameObject.OnDestroyedCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            TSWorldObject(player));
    }
    void OnGameObjectDamaged(GameObject* gameObject, Player* player) override
    {
        ts_events.GameObject.OnDamagedCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            TSWorldObject(player));
    }
    void OnGameObjectLootStateChanged(GameObject* gameObject, uint32 state, Unit* unit) override
    {
        ts_events.GameObject.OnLootStateChangedCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            state, WrapUnit(unit));
    }
    void OnGameObjectStateChanged(GameObject* gameObject, uint32 state) override
    {
        ts_events.GameObject.OnGOStateChangedCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject), state);
    }
    bool CanGameObjectGossipHello(Player* player, GameObject* gameObject) override
    {
        bool cancel = false;
        ts_events.GameObject.OnGossipHelloCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), TSMutable<bool, bool>(&cancel));
        return cancel;
    }
    bool CanGameObjectGossipSelect(Player* player, GameObject* gameObject, uint32 sender,
        uint32 action) override
    {
        bool cancel = false;
        ts_events.GameObject.OnGossipSelectCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), sender, action, TSMutable<bool, bool>(&cancel));
        return cancel;
    }
    bool CanGameObjectGossipSelectCode(Player* player, GameObject* gameObject, uint32 sender,
        uint32 action, char const* code) override
    {
        bool cancel = false;
        std::string const text = code ? code : "";
        ts_events.GameObject.OnGossipSelectCodeCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), sender, action, text, TSMutable<bool, bool>(&cancel));
        return cancel;
    }
    bool CanGameObjectQuestAccept(Player* player, GameObject* gameObject, Quest const* quest) override
    {
        TSQuest const wrappedQuest(const_cast<Quest*>(quest));
        ts_events.GameObject.OnQuestAcceptCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), wrappedQuest);
        ts_events.Quest.OnAcceptCallbacks.Fire(quest->GetQuestId(), wrappedQuest, WrapPlayer(player),
            TSObject(gameObject));
        return false;
    }
    bool CanGameObjectQuestReward(Player* player, GameObject* gameObject, Quest const* quest,
        uint32 reward) override
    {
        TSQuest const wrappedQuest(const_cast<Quest*>(quest));
        ts_events.GameObject.OnQuestRewardCallbacks.Fire(gameObject->GetEntry(), TSGameObject(gameObject),
            WrapPlayer(player), wrappedQuest, reward);
        ts_events.Quest.OnRewardCallbacks.Fire(quest->GetQuestId(), wrappedQuest, WrapPlayer(player),
            TSObject(gameObject), reward);
        return false;
    }
    void OnGameObjectRemoveWorld(GameObject* gameObject) override
    {
        std::uint32_t const entry = gameObject->GetEntry();
        TSGameObject const wrapped(gameObject);
        ts_events.GameObject.OnRemoveCallbacks.Fire(entry, wrapped);
        if (Map* map = gameObject->GetMap())
            ts_events.Map.OnGameObjectRemoveCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), wrapped);
        ClearLuaEntityState(gameObject);
        ClearNativeObjectState(gameObject);
    }
};

class TsWowItemScript final : public AllItemScript
{
public:
    TsWowItemScript() : AllItemScript("TsWowItemScript") { }

    bool CanItemGossipSelect(Player* player, Item* item, uint32 sender, uint32 action) override
    {
        bool cancel = false;
        ts_events.Item.OnGossipSelectCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            sender, action, TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanItemGossipSelectCode(Player* player, Item* item, uint32 sender, uint32 action,
        char const* code) override
    {
        bool cancel = false;
        std::string const text = code ? code : "";
        ts_events.Item.OnGossipSelectCodeCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            sender, action, text, TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanItemUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        bool cancel = false;
        ts_events.Item.OnUseCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            const_cast<SpellCastTargets*>(&targets), TSMutable<bool, bool>(&cancel));
        return cancel;
    }
    bool CanItemExpire(Player* player, ItemTemplate const* itemTemplate) override
    {
        bool cancel = false;
        ts_events.Item.OnExpireCallbacks.Fire(itemTemplate->ItemId,
            TSItemTemplate(const_cast<ItemTemplate*>(itemTemplate)), WrapPlayer(player),
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    bool CanItemRemove(Player* player, Item* item) override
    {
        bool cancel = false;
        ts_events.Item.OnRemoveCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    bool CanCastItemCombatSpell(Player* player, Unit* victim, SpellInfo const* spellInfo, Item* item) override
    {
        bool cancel = false;
        ts_events.Item.OnCastSpellCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            WrapUnit(victim), TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
    bool CanItemQuestAccept(Player* player, Item* item, Quest const* quest) override
    {
        TSQuest const wrappedQuest(const_cast<Quest*>(quest));
        ts_events.Item.OnQuestAcceptCallbacks.Fire(item->GetEntry(), TSItem(item), WrapPlayer(player),
            wrappedQuest);
        ts_events.Quest.OnAcceptCallbacks.Fire(quest->GetQuestId(), wrappedQuest, WrapPlayer(player),
            TSObject(item));
        return true;
    }
};

class TsWowUnitScript final : public UnitScript
{
public:
    TsWowUnitScript() : UnitScript("TsWowUnitScript") { }

    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        ts_events.Unit.OnEnterCombatCallbacks.Fire(WrapUnit(unit));
        ts_events.Unit.OnEnterCombatWithCallbacks.Fire(WrapUnit(unit), WrapUnit(victim));
    }
    void OnUnitExitCombat(Unit* unit) override
    {
        ts_events.Unit.OnExitCombatCallbacks.Fire(WrapUnit(unit));
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        ts_events.Unit.OnDeathCallbacks.Fire(WrapUnit(unit), WrapUnit(killer));
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return;
        std::uint32_t const entry = creature->GetEntry();
        ts_events.Creature.OnDeathCallbacks.Fire(entry, WrapCreature(creature), WrapUnit(killer));
    }

    void OnUnitDeathEarly(Unit* unit, Unit* killer) override
    {
        ts_events.Unit.OnDeathEarlyCallbacks.Fire(WrapUnit(unit), WrapUnit(killer));
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            ts_events.Creature.OnDeathEarlyCallbacks.Fire(creature->GetEntry(), WrapCreature(creature),
                WrapUnit(killer));
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        ts_events.Unit.OnCalcHealCallbacks.Fire(WrapUnit(healer), WrapUnit(receiver),
            TSMutableNumber<uint32>(&gain));
    }

    void OnBeforeRollMeleeOutcomeAgainst(Unit const* attacker, Unit const* victim,
        WeaponAttackType attackType, int32&, int32&, int32&, int32&, int32& critChance,
        int32& missChance, int32& dodgeChance, int32& parryChance, int32& blockChance) override
    {
        float miss = missChance / 100.0f;
        float crit = critChance / 100.0f;
        float dodge = dodgeChance / 100.0f;
        float block = blockChance / 100.0f;
        float parry = parryChance / 100.0f;
        ts_events.Unit.OnCalcMeleeOutcomeCallbacks.Fire(WrapUnit(const_cast<Unit*>(attacker)),
            WrapUnit(const_cast<Unit*>(victim)), TSMutableNumber<float>(&miss),
            TSMutableNumber<float>(&crit), TSMutableNumber<float>(&dodge),
            TSMutableNumber<float>(&block), TSMutableNumber<float>(&parry), attackType);
        missChance = static_cast<int32>(miss * 100.0f);
        critChance = static_cast<int32>(crit * 100.0f);
        dodgeChance = static_cast<int32>(dodge * 100.0f);
        blockChance = static_cast<int32>(block * 100.0f);
        parryChance = static_cast<int32>(parry * 100.0f);
    }

    void OnUnitLifecycle(Unit* unit, UnitLifecycleEvent type, Unit* other, SpellInfo const* spellInfo,
        CalcDamageInfo* damageInfo, float* floatValue, uint32* uintValue, bool* boolValue,
        uint64 firstValue, uint64 secondValue, uint32 argument, uint32 secondaryArgument,
        bool flag) override
    {
        TSUnit const wrapped = WrapUnit(unit);
        TSUnit const wrappedOther = WrapUnit(other);
        switch (type)
        {
            case UnitLifecycleEvent::CalcMissChance:
                ts_events.Unit.OnCalcMissChanceCallbacks.Fire(wrapped,
                    TSMutableNumber<float>(floatValue));
                break;
            case UnitLifecycleEvent::MeleeDamageEarly:
                ts_events.Unit.OnMeleeDamageEarlyCallbacks.Fire(TSMeleeDamageInfo(damageInfo),
                    TSMutableNumber<std::uint32_t>(uintValue), argument, secondaryArgument);
                break;
            case UnitLifecycleEvent::MeleeDamageLate:
                ts_events.Unit.OnMeleeDamageLateCallbacks.Fire(TSMeleeDamageInfo(damageInfo),
                    TSMutableNumber<std::uint32_t>(uintValue), argument, secondaryArgument);
                break;
            case UnitLifecycleEvent::CalcMeleeCrit:
                ts_events.Unit.OnCalcMeleeCritCallbacks.Fire(wrapped, wrappedOther,
                    TSMutableNumber<float>(floatValue), argument);
                break;
            case UnitLifecycleEvent::CalcThreatEarly:
                ts_events.Unit.OnCalcThreatEarlyCallbacks.Fire(wrapped, wrappedOther,
                    TSMutableNumber<float>(floatValue), TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), flag);
                break;
            case UnitLifecycleEvent::CalcThreatLate:
                ts_events.Unit.OnCalcThreatLateCallbacks.Fire(wrapped, wrappedOther,
                    TSMutableNumber<float>(floatValue), TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), flag);
                break;
            case UnitLifecycleEvent::CalcScaleThreat:
                ts_events.Unit.OnCalcScaleThreatCallbacks.Fire(wrapped, wrappedOther,
                    TSMutableNumber<float>(floatValue), flag);
                break;
            case UnitLifecycleEvent::ExitCombatWith:
                ts_events.Unit.OnExitCombatWithCallbacks.Fire(wrapped, wrappedOther);
                break;
            case UnitLifecycleEvent::SetTarget:
                ts_events.Unit.OnSetTargetCallbacks.Fire(wrapped, firstValue, secondValue);
                break;
            case UnitLifecycleEvent::LiquidStatusChanged:
                ts_events.Unit.OnLiquidStatusChangedCallbacks.Fire(wrapped,
                    TSMutableNumber<std::uint32_t>(uintValue));
                break;
            case UnitLifecycleEvent::OutdoorsChanged:
                ts_events.Unit.OnOutdoorsChangedCallbacks.Fire(wrapped,
                    TSMutable<bool, bool>(boolValue));
                break;
        }
    }
};

class TsWowMapScript final : public AllMapScript
{
public:
    TsWowMapScript() : AllMapScript("TsWowMapScript") { }

    void OnCreateMap(Map* map) override
    {
        std::uint32_t const mapId = map->GetId();
        ts_events.Map.OnCreateCallbacks.Fire(mapId, TSMap(map, &MapApi));
    }

    void OnDestroyMap(Map* map) override
    {
        ClearLuaEntityState(map);
        ClearNativeObjectState(map);
    }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        UpdateNativeTimers(map, diff);
        std::uint32_t const mapId = map->GetId();
        ts_events.Map.OnUpdateCallbacks.Fire(mapId, TSMap(map, &MapApi), diff);
    }

    void OnMapDelayedUpdate(Map* map, uint32 diff) override
    {
        RunLuaDelayedCallbacks(map);
        RunNativeDelayedCallbacks(map);
        std::uint32_t const mapId = map->GetId();
        ts_events.Map.OnUpdateDelayedCallbacks.Fire(mapId, TSMap(map, &MapApi), diff, TSMainThreadContext());
    }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        std::uint32_t const mapId = map->GetId();
        ts_events.Map.OnPlayerEnterCallbacks.Fire(mapId, TSMap(map, &MapApi), WrapPlayer(player));
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        std::uint32_t const mapId = map->GetId();
        ts_events.Map.OnPlayerLeaveCallbacks.Fire(mapId, TSMap(map, &MapApi), WrapPlayer(player));
    }

    void OnMapCheckEncounter(Map* map, Player* player) override
    {
        ts_events.Map.OnCheckEncounterCallbacks.Fire(map->GetId(), TSMap(map, &MapApi), WrapPlayer(player));
    }

    void OnInstanceLifecycle(InstanceMap* map, InstanceScript* script, InstanceLifecycleEvent type,
        Player* player, uint32 value, uint32 secondaryValue, bool flag, uint32* mutableValue) override
    {
        std::uint32_t const mapId = map->GetId();
        TSInstance const instance(script);
        switch (type)
        {
            case InstanceLifecycleEvent::Create:
                ts_events.Instance.OnCreateCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::Reload:
                ts_events.Instance.OnReloadCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::Load:
                ts_events.Instance.OnLoadCallbacks.Fire(mapId, instance, flag);
                break;
            case InstanceLifecycleEvent::Save:
                ts_events.Instance.OnSaveCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::Update:
                ts_events.Instance.OnUpdateCallbacks.Fire(mapId, instance, value);
                break;
            case InstanceLifecycleEvent::PlayerEnter:
                ts_events.Instance.OnPlayerEnterCallbacks.Fire(mapId, instance, WrapPlayer(player));
                break;
            case InstanceLifecycleEvent::PlayerLeave:
                ts_events.Instance.OnPlayerLeaveCallbacks.Fire(mapId, instance, WrapPlayer(player));
                break;
            case InstanceLifecycleEvent::BossStateChange:
                ts_events.Instance.OnBossStateChangeCallbacks.Fire(mapId, instance, value, secondaryValue);
                break;
            case InstanceLifecycleEvent::LoadBossBoundaries:
                ts_events.Instance.OnLoadBossBoundariesCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::LoadMinionData:
                ts_events.Instance.OnLoadMinionDataCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::LoadDoorData:
                ts_events.Instance.OnLoadDoorDataCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::LoadObjectData:
                ts_events.Instance.OnLoadObjectDataCallbacks.Fire(mapId, instance);
                break;
            case InstanceLifecycleEvent::SetBossNumber:
                ts_events.Instance.OnSetBossNumberCallbacks.Fire(mapId, instance,
                    TSMutableNumber<std::uint32_t>(mutableValue));
                break;
        }
    }

    void OnInstanceFillInitialWorldStates(InstanceMap* map, InstanceScript* script,
        WorldPackets::WorldState::InitWorldStates& packet) override
    {
        ts_events.Instance.OnFillInitialWorldStatesCallbacks.Fire(map->GetId(), TSInstance(script),
            TSWorldStatePacket(&packet));
    }

    void OnInstanceCanKillBoss(InstanceMap* map, InstanceScript* script, uint32 bossId,
        Player* player, bool& canKill) override
    {
        ts_events.Instance.OnCanKillBossCallbacks.Fire(map->GetId(), TSInstance(script), bossId,
            WrapPlayer(player), TSMutable<bool, bool>(&canKill));
    }
};

class TsWowSpellScript final : public AllSpellScript
{
public:
    TsWowSpellScript() : AllSpellScript("TsWowSpellScript") { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool) override
    {
        ts_events.Spell.OnCastCallbacks.Fire(spellInfo->Id, TSSpell(spell, WrapUnit(caster)));
    }

    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        SpellInfo const* spellInfo = spell->GetSpellInfo();
        std::uint8_t mutableResult = result;
        ts_events.Spell.OnCheckCastCallbacks.Fire(spellInfo->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())),
            TSMutableNumber<std::uint8_t>(&mutableResult));
        result = static_cast<SpellCastResult>(mutableResult);
    }

    bool OnSpellLifecycle(Spell* spell, SpellLifecycleEvent type, uint32 value,
        Player* player, Quest const* quest, bool* boolValue, float* floatValue) override
    {
        SpellInfo const* spellInfo = spell->GetSpellInfo();
        TSSpell const wrapped(spell, WrapUnit(spell->GetCaster()));
        bool cancelDefault = false;
        TSMutable<bool, bool> const mutableCancel(&cancelDefault);
        switch (type)
        {
            case SpellLifecycleEvent::BeforeCast:
                ts_events.Spell.OnBeforeCastCallbacks.Fire(spellInfo->Id, wrapped, mutableCancel);
                break;
            case SpellLifecycleEvent::AfterCast:
                ts_events.Spell.OnAfterCastCallbacks.Fire(spellInfo->Id, wrapped, mutableCancel);
                break;
            case SpellLifecycleEvent::BeforeHit:
                ts_events.Spell.OnBeforeHitCallbacks.Fire(spellInfo->Id, wrapped, value, mutableCancel);
                break;
            case SpellLifecycleEvent::Hit:
                ts_events.Spell.OnHitCallbacks.Fire(spellInfo->Id, wrapped);
                break;
            case SpellLifecycleEvent::AfterHit:
                ts_events.Spell.OnAfterHitCallbacks.Fire(spellInfo->Id, wrapped, mutableCancel);
                break;
            case SpellLifecycleEvent::Cancel:
                ts_events.Spell.OnCancelCallbacks.Fire(spellInfo->Id, wrapped, value);
                break;
            case SpellLifecycleEvent::QuestFinish:
                ts_events.Quest.OnSpellFinishCallbacks.Fire(quest->GetQuestId(),
                    TSQuest(const_cast<Quest*>(quest)), WrapPlayer(player), wrapped);
                break;
            case SpellLifecycleEvent::EffectApplyGlyph:
                ts_events.Spell.OnEffectApplyGlyphCallbacks.Fire(spellInfo->Id, wrapped,
                    TSMutable<bool, bool>(boolValue));
                break;
            case SpellLifecycleEvent::CalcCrit:
                ts_events.Spell.OnCalcCritCallbacks.Fire(spellInfo->Id, wrapped,
                    TSMutableNumber<float>(floatValue));
                break;
            case SpellLifecycleEvent::SuccessfulDispel:
                ts_events.Spell.OnSuccessfulDispelCallbacks.Fire(spellInfo->Id, wrapped, value);
                break;
        }
        return !cancelDefault;
    }

    bool OnAuraLifecycle(Aura* aura, AuraLifecycleEvent type, AuraEffect const* effect,
        AuraApplication const* application, Unit* target, DispelInfo* dispelInfo, DamageInfo* damageInfo,
        ProcEventInfo* procInfo, int32* intValue, uint32* uintValue, bool* boolValue,
        SpellModifier* spellModifier, uint32 mode, float* floatValue) override
    {
        std::uint32_t const spellId = aura->GetId();
        TSAura const wrappedAura(aura);
        TSAuraEffect const wrappedEffect(const_cast<AuraEffect*>(effect));
        TSAuraApplication const wrappedApplication(const_cast<AuraApplication*>(application));
        TSDispelInfo const wrappedDispel(dispelInfo);
        TSDamageInfo const wrappedDamage(damageInfo);
        TSProcEventInfo const wrappedProc(procInfo);
        bool cancelDefault = false;
        TSMutable<bool, bool> const cancel(&cancelDefault);
        switch (type)
        {
            case AuraLifecycleEvent::CheckAreaTarget:
                ts_events.Spell.OnCheckAreaTargetCallbacks.Fire(spellId, wrappedAura, WrapUnit(target),
                    TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::Dispel:
                ts_events.Spell.OnDispelCallbacks.Fire(spellId, wrappedAura, wrappedDispel, cancel);
                break;
            case AuraLifecycleEvent::AfterDispel:
                ts_events.Spell.OnAfterDispelCallbacks.Fire(spellId, wrappedAura, wrappedDispel, cancel);
                break;
            case AuraLifecycleEvent::EffectApply:
                ts_events.Spell.OnApplyCallbacks.Fire(spellId, wrappedEffect, wrappedApplication, mode, cancel);
                break;
            case AuraLifecycleEvent::EffectRemove:
                ts_events.Spell.OnRemoveCallbacks.Fire(spellId, wrappedEffect, wrappedApplication, mode, cancel);
                break;
            case AuraLifecycleEvent::AfterEffectApply:
                ts_events.Spell.OnAfterEffectApplyCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    mode, cancel);
                break;
            case AuraLifecycleEvent::AfterEffectRemove:
                ts_events.Spell.OnAfterEffectRemoveCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    mode, cancel);
                break;
            case AuraLifecycleEvent::EffectPeriodic:
                ts_events.Spell.OnEffectPeriodicCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    cancel);
                break;
            case AuraLifecycleEvent::Tick:
                ts_events.Spell.OnTickCallbacks.Fire(spellId, wrappedEffect);
                break;
            case AuraLifecycleEvent::EffectCalcAmount:
                ts_events.Spell.OnEffectCalcAmountCallbacks.Fire(spellId, wrappedEffect,
                    TSMutableNumber<std::int32_t>(intValue), TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::EffectCalcPeriodic:
                ts_events.Spell.OnEffectCalcPeriodicCallbacks.Fire(spellId, wrappedEffect,
                    TSMutable<bool, bool>(boolValue), TSMutableNumber<std::int32_t>(intValue), cancel);
                break;
            case AuraLifecycleEvent::EffectCalcSpellMod:
                ts_events.Spell.OnEffectCalcSpellModCallbacks.Fire(spellId, wrappedEffect,
                    TSSpellModifier(spellModifier), cancel);
                break;
            case AuraLifecycleEvent::EffectAbsorb:
                ts_events.Spell.OnEffectAbsorbCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedDamage, TSMutableNumber<std::uint32_t>(uintValue), cancel);
                break;
            case AuraLifecycleEvent::EffectAfterAbsorb:
                ts_events.Spell.OnEffectAfterAbsorbCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedDamage, TSMutableNumber<std::uint32_t>(uintValue), cancel);
                break;
            case AuraLifecycleEvent::EffectManaShield:
                ts_events.Spell.OnEffectManaShieldCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedDamage, TSMutableNumber<std::uint32_t>(uintValue), cancel);
                break;
            case AuraLifecycleEvent::EffectAfterManaShield:
                ts_events.Spell.OnEffectAfterManaShieldCallbacks.Fire(spellId, wrappedEffect,
                    wrappedApplication, wrappedDamage, TSMutableNumber<std::uint32_t>(uintValue), cancel);
                break;
            case AuraLifecycleEvent::EffectSplit:
                ts_events.Spell.OnEffectSplitCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedDamage, TSMutableNumber<std::uint32_t>(uintValue), cancel);
                break;
            case AuraLifecycleEvent::CheckProc:
                ts_events.Spell.OnCheckProcCallbacks.Fire(spellId, wrappedApplication, wrappedProc,
                    TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::CheckEffectProc:
                ts_events.Spell.OnCheckEffectProcCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedProc, TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::PrepareProc:
                ts_events.Spell.OnPrepareProcCallbacks.Fire(spellId, wrappedApplication, wrappedProc,
                    TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::Proc:
                ts_events.Spell.OnProcCallbacks.Fire(spellId, wrappedApplication, wrappedProc,
                    TSMutable<bool, bool>(boolValue), cancel);
                break;
            case AuraLifecycleEvent::AfterProc:
                ts_events.Spell.OnAfterProcCallbacks.Fire(spellId, wrappedApplication, wrappedProc, cancel);
                break;
            case AuraLifecycleEvent::EffectProc:
                ts_events.Spell.OnEffectProcCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedProc, cancel);
                break;
            case AuraLifecycleEvent::AfterEffectProc:
                ts_events.Spell.OnAfterEffectProcCallbacks.Fire(spellId, wrappedEffect, wrappedApplication,
                    wrappedProc, cancel);
                break;
            case AuraLifecycleEvent::SetDuration:
                ts_events.Spell.OnSetDurationCallbacks.Fire(spellId, wrappedAura,
                    TSMutableNumber<std::int32_t>(intValue), TSMutable<bool, bool>(boolValue));
                break;
            case AuraLifecycleEvent::PeriodicDamage:
                ts_events.Spell.OnPeriodicDamageCallbacks.Fire(spellId, wrappedEffect,
                    TSMutableNumber<std::uint32_t>(uintValue));
                break;
            case AuraLifecycleEvent::CalcAuraCrit:
                ts_events.Spell.OnCalcAuraCritCallbacks.Fire(spellId, wrappedEffect,
                    TSMutableNumber<float>(floatValue));
                break;
        }
        return !cancelDefault;
    }
    void OnSpellCalculation(SpellInfo const* spellInfo, SpellCalculationEvent type,
        WorldObject* caster, Unit* target, Spell* spell, float* floatValue, int32* intValue,
        uint32* uintValue, uint32* secondaryUIntValue, uint8 attackType, int32 argument) override
    {
        std::uint32_t const spellId = spellInfo->Id;
        TSSpellInfo const wrappedInfo(const_cast<SpellInfo*>(spellInfo));
        switch (type)
        {
            case SpellCalculationEvent::Miss:
                ts_events.Spell.OnCalcMissCallbacks.Fire(spellId,
                    TSSpell(spell, WrapUnit(spell->GetCaster())), WrapUnit(target),
                    TSMutableNumber<std::uint32_t>(uintValue),
                    TSMutableNumber<std::uint32_t>(secondaryUIntValue));
                break;
            case SpellCalculationEvent::SpellPowerLevelPenalty:
                ts_events.Spell.OnCalcSpellPowerLevelPenaltyCallbacks.Fire(spellId, wrappedInfo,
                    TSMutableNumber<float>(floatValue), WrapUnit(caster->ToUnit()));
                break;
            case SpellCalculationEvent::Reflect:
                ts_events.Spell.OnCalcReflectCallbacks.Fire(spellId, wrappedInfo,
                    TSMutableNumber<std::int32_t>(intValue), TSWorldObject(caster), WrapUnit(target));
                break;
            case SpellCalculationEvent::Hit:
                ts_events.Spell.OnCalcHitCallbacks.Fire(spellId, wrappedInfo,
                    TSMutableNumber<std::int32_t>(intValue), TSWorldObject(caster), WrapUnit(target));
                break;
            case SpellCalculationEvent::Resist:
                ts_events.Spell.OnCalcResistCallbacks.Fire(spellId, wrappedInfo,
                    TSMutableNumber<std::int32_t>(intValue), TSWorldObject(caster), WrapUnit(target));
                break;
            case SpellCalculationEvent::MeleeMiss:
                ts_events.Spell.OnCalcMeleeMissCallbacks.Fire(spellId, wrappedInfo,
                    TSMutableNumber<float>(floatValue), WrapUnit(caster->ToUnit()), WrapUnit(target),
                    attackType, argument);
                break;
        }
    }

    bool CanHandleSpellEffect(Spell* spell, SpellEffectInfo const* effect, uint32 mode,
        Unit* unitTarget, Item* itemTarget, GameObject* gameObjectTarget, Corpse* corpseTarget) override
    {
        bool preventDefault = false;
        ts_events.Spell.OnEffectCallbacks.Fire(spell->GetSpellInfo()->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())), TSMutable<bool, bool>(&preventDefault),
            TSSpellEffectInfo(const_cast<SpellEffectInfo*>(effect)), mode, WrapUnit(unitTarget),
            TSItem(itemTarget), TSGameObject(gameObjectTarget), TSCorpse(corpseTarget));
        return !preventDefault;
    }

    void OnSpellDamage(Spell* spell, SpellDamagePhase phase, SpellNonMeleeDamage* damageInfo,
        int32* earlyDamage, uint32* lateDamage, uint8 attackType, bool critical,
        uint32 effectMask) override
    {
        std::uint32_t const spellId = spell->GetSpellInfo()->Id;
        TSSpell const wrapped(spell, WrapUnit(spell->GetCaster()));
        TSSpellDamageInfo const wrappedDamage(damageInfo);
        if (phase == SpellDamagePhase::Early)
            ts_events.Spell.OnDamageEarlyCallbacks.Fire(spellId, wrapped,
                TSMutableNumber<std::int32_t>(earlyDamage), wrappedDamage, attackType, critical,
                effectMask);
        else
            ts_events.Spell.OnDamageLateCallbacks.Fire(spellId, wrapped,
                TSMutableNumber<std::uint32_t>(lateDamage), wrappedDamage, attackType, critical,
                effectMask);
    }

    bool CanSelectSpellObjectAreaTarget(Spell* spell, std::list<WorldObject*>& targets,
        uint32 effectIndex, SpellImplicitTargetInfo const& targetType) override
    {
        bool cancel = false;
        ts_events.Spell.OnObjectAreaTargetSelectCallbacks.Fire(spell->GetSpellInfo()->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())), TSWorldObjectCollection(&targets),
            effectIndex, TSSpellImplicitTargetInfo(const_cast<SpellImplicitTargetInfo*>(&targetType)),
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanSelectSpellObjectTarget(Spell* spell, WorldObject*& target, uint32 effectIndex,
        SpellImplicitTargetInfo const& targetType) override
    {
        bool cancel = false;
        ts_events.Spell.OnObjectTargetSelectCallbacks.Fire(spell->GetSpellInfo()->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())), TSMutableWorldObject(&target), effectIndex,
            TSSpellImplicitTargetInfo(const_cast<SpellImplicitTargetInfo*>(&targetType)),
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    bool CanSelectSpellDestinationTarget(Spell* spell, SpellDestination& target,
        uint32 effectIndex, SpellImplicitTargetInfo const& targetType) override
    {
        bool cancel = false;
        ts_events.Spell.OnDestinationTargetSelectCallbacks.Fire(spell->GetSpellInfo()->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())), TSSpellDestination(&target), effectIndex,
            TSSpellImplicitTargetInfo(const_cast<SpellImplicitTargetInfo*>(&targetType)),
            TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    void OnSpellLearn(SpellInfo const* spellInfo, Player* player, bool active, bool disabled,
        bool superseded, uint32 fromSkill) override
    {
        ts_events.Spell.OnLearnCallbacks.Fire(spellInfo->Id,
            TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), WrapPlayer(player), active, disabled,
            superseded, fromSkill);
    }

    void OnSpellUnlearn(SpellInfo const* spellInfo, Player* player, bool disabled,
        bool learnLowRank) override
    {
        ts_events.Spell.OnUnlearnCallbacks.Fire(spellInfo->Id,
            TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), WrapPlayer(player), disabled, learnLowRank);
    }

    void OnSpellUnlearnTalent(SpellInfo const* spellInfo, Player* player, uint32 tabIndex,
        uint32 tier, uint32 column, uint32 rank, bool direct) override
    {
        ts_events.Spell.OnUnlearnTalentCallbacks.Fire(spellInfo->Id,
            TSSpellInfo(const_cast<SpellInfo*>(spellInfo)), WrapPlayer(player), tabIndex, tier,
            column, rank, direct);
    }

    bool CanCalculateSpellResistAbsorb(Spell* spell, DamageInfo const& damageInfo,
        uint32& resistAmount, int32& absorbAmount) override
    {
        bool cancel = false;
        ts_events.Spell.OnOnResistAbsorbCalculateCallbacks.Fire(spell->GetSpellInfo()->Id,
            TSSpell(spell, WrapUnit(spell->GetCaster())),
            TSDamageInfo(const_cast<DamageInfo*>(&damageInfo)),
            TSMutableNumber<std::uint32_t>(&resistAmount),
            TSMutableNumber<std::int32_t>(&absorbAmount), TSMutable<bool, bool>(&cancel));
        return !cancel;
    }
};

class TsWowSmartScript final : public AllSmartScript
{
public:
    TsWowSmartScript() : AllSmartScript("TsWowSmartScript") { }

    void OnSmartAction(uint32 actionType, SmartActionPhase phase, SmartActionContext& context,
        bool& cancelAction, bool& cancelLink) override
    {
        TSSmartScriptValues const wrapped(&context);
        if (phase == SmartActionPhase::Early)
            ts_events.SmartAction.OnActivateEarlyCallbacks.Fire(actionType, wrapped,
                TSMutable<bool, bool>(&cancelAction), TSMutable<bool, bool>(&cancelLink));
        else
            ts_events.SmartAction.OnActivateLateCallbacks.Fire(actionType, wrapped,
                TSMutable<bool, bool>(&cancelLink));
    }
};

class TsWowConditionScript final : public AllConditionScript
{
public:
    TsWowConditionScript() : AllConditionScript("TsWowConditionScript") { }

    void OnConditionEvaluation(Condition* condition, ConditionSourceInfo& sourceInfo,
        bool& result) override
    {
        ts_events.Condition.OnCheckCallbacks.Fire(static_cast<std::uint32_t>(condition->ConditionType),
            TSCondition(condition), TSConditionSourceInfo(&sourceInfo), TSMutable<bool, bool>(&result));
    }
};

class TsWowAchievementScript final : public AchievementScript
{
public:
    TsWowAchievementScript() : AchievementScript("TsWowAchievementScript") { }

    void OnCriteriaProgress(Player* player, AchievementEntry const* achievement,
        AchievementCriteriaEntry const* criteria, uint32 progressType, uint32 timeElapsed,
        bool timedCompleted) override
    {
        ts_events.Achievement.OnUpdateCallbacks.Fire(criteria->referredAchievement, WrapPlayer(player),
            TSAchievementEntry(const_cast<AchievementEntry*>(achievement)),
            TSAchievementCriteriaEntry(const_cast<AchievementCriteriaEntry*>(criteria)),
            progressType, timeElapsed, timedCompleted);
    }
};

class TsWowBattlegroundScript final : public AllBattlegroundScript
{
public:
    TsWowBattlegroundScript() : AllBattlegroundScript("TsWowBattlegroundScript") { }

    void OnBattlegroundLifecycle(Battleground* battleground, BattlegroundLifecycleEvent type,
        Player* player, uint32* value, uint32 secondaryValue, bool flag, bool* result) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        TSBattleground const wrapped = WrapBattleground(battleground);
        switch (type)
        {
            case BattlegroundLifecycleEvent::CanCreate:
                ts_events.Battleground.OnCanCreateCallbacks.Fire(mapId, wrapped,
                    TSMutable<bool, bool>(result));
                break;
            case BattlegroundLifecycleEvent::Reload:
                ts_events.Battleground.OnReloadCallbacks.Fire(mapId, wrapped);
                break;
            case BattlegroundLifecycleEvent::UpdateEarly:
                ts_events.Battleground.OnUpdateEarlyCallbacks.Fire(mapId, wrapped, *value);
                break;
            case BattlegroundLifecycleEvent::EndEarly:
                ts_events.Battleground.OnEndEarlyCallbacks.Fire(mapId, wrapped,
                    TSMutableNumber<std::uint32_t>(value));
                break;
            case BattlegroundLifecycleEvent::Reset:
                ts_events.Battleground.OnResetCallbacks.Fire(mapId, wrapped);
                break;
            case BattlegroundLifecycleEvent::UpdateScore:
                ts_events.Battleground.OnUpdateScoreCallbacks.Fire(mapId, wrapped, WrapPlayer(player),
                    secondaryValue, flag, TSMutableNumber<std::uint32_t>(value));
                break;
            case BattlegroundLifecycleEvent::CloseDoors:
                ApplyTsWowBattlegroundDoors(battleground, false);
                ts_events.Battleground.OnCloseDoorsCallbacks.Fire(mapId, wrapped);
                break;
            case BattlegroundLifecycleEvent::PlayerUnderMap:
                ts_events.Battleground.OnPlayerUnderMapCallbacks.Fire(mapId, wrapped, WrapPlayer(player),
                    TSMutable<bool, bool>(result));
                break;
            case BattlegroundLifecycleEvent::AreaTrigger:
                ts_events.Battleground.OnAreaTriggerCallbacks.Fire(mapId, wrapped, WrapPlayer(player),
                    secondaryValue, TSMutable<bool, bool>(result));
                break;
        }
    }

    void OnBattlegroundAction(Battleground* battleground, BattlegroundActionEvent type,
        Player* player, GameObject* target) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        TSBattleground const wrapped = WrapBattleground(battleground);
        switch (type)
        {
            case BattlegroundActionEvent::ClickFlag:
                ts_events.Battleground.OnClickFlagCallbacks.Fire(mapId, wrapped, WrapPlayer(player),
                    TSGameObject(target));
                break;
            case BattlegroundActionEvent::DropFlag:
                ts_events.Battleground.OnDropFlagCallbacks.Fire(mapId, wrapped, WrapPlayer(player));
                break;
            case BattlegroundActionEvent::DestroyGate:
                ts_events.Battleground.OnDestroyGateCallbacks.Fire(mapId, wrapped, WrapPlayer(player),
                    TSGameObject(target));
                break;
        }
    }

    void OnBattlegroundAchievementCriteria(Battleground* battleground, uint32 criteriaId,
        Player* source, Unit* target, uint32 miscValue, bool& handled) override
    {
        ts_events.Battleground.OnAchievementCriteriaCallbacks.Fire(battleground->GetMapId(),
            WrapBattleground(battleground), criteriaId, WrapPlayer(source), WrapUnit(target), miscValue,
            TSMutable<bool, bool>(&handled));
    }

    void OnBattlegroundGenericEvent(Battleground* battleground, WorldObject* object,
        uint32 eventId, WorldObject* invoker) override
    {
        ts_events.Battleground.OnGenericEventCallbacks.Fire(battleground->GetMapId(),
            WrapBattleground(battleground), TSWorldObject(object), eventId, TSWorldObject(invoker));
    }

    void OnBattlegroundTypeSelection(uint32 candidateType, float* weight,
        uint32 originalType, uint32* selectedType) override
    {
        if (weight)
            ts_events.Battleground.OnWeightCallbacks.Fire(candidateType, candidateType,
                TSMutableNumber<float>(weight), originalType);
        if (selectedType)
            ts_events.Battleground.OnSelectCallbacks.Fire(*selectedType,
                TSMutableNumber<std::uint32_t>(selectedType));
    }
    void OnBattlegroundSpawn(Battleground* battleground, BattlegroundSpawnEvent type, uint32 slot,
        uint32& entry, uint8* stateOrTeam, float& x, float& y, float& z, float& o,
        float* rotation0, float* rotation1, float* rotation2, float* rotation3,
        uint32* respawnTime) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        TSBattleground const wrapped = WrapBattleground(battleground);
        switch (type)
        {
            case BattlegroundSpawnEvent::GameObject:
                ts_events.Battleground.OnAddGameObjectCallbacks.Fire(mapId, wrapped, slot,
                    TSMutableNumber<std::uint32_t>(&entry), TSMutableNumber<std::uint8_t>(stateOrTeam),
                    TSMutableNumber<float>(&x), TSMutableNumber<float>(&y), TSMutableNumber<float>(&z),
                    TSMutableNumber<float>(&o), TSMutableNumber<float>(rotation0),
                    TSMutableNumber<float>(rotation1), TSMutableNumber<float>(rotation2),
                    TSMutableNumber<float>(rotation3));
                break;
            case BattlegroundSpawnEvent::Creature:
                ts_events.Battleground.OnAddCreatureCallbacks.Fire(mapId, wrapped, slot,
                    TSMutableNumber<std::uint32_t>(&entry), TSMutableNumber<float>(&x),
                    TSMutableNumber<float>(&y), TSMutableNumber<float>(&z), TSMutableNumber<float>(&o),
                    TSMutableNumber<std::uint32_t>(respawnTime));
                break;
            case BattlegroundSpawnEvent::SpiritGuide:
                ts_events.Battleground.OnAddSpiritGuideCallbacks.Fire(mapId, wrapped, slot,
                    TSMutableNumber<std::uint32_t>(&entry), TSMutableNumber<std::uint8_t>(stateOrTeam),
                    TSMutableNumber<float>(&x), TSMutableNumber<float>(&y), TSMutableNumber<float>(&z),
                    TSMutableNumber<float>(&o));
                break;
        }
    }

    void OnBattlegroundAddPlayer(Battleground* battleground, Player* player) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnAddPlayerCallbacks.Fire(mapId, WrapBattleground(battleground), WrapPlayer(player));
    }

    void OnBattlegroundRemovePlayerAtLeave(Battleground* battleground, Player* player) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnRemovePlayerCallbacks.Fire(mapId, WrapBattleground(battleground),
            player->GetGUID().GetRawValue(), WrapPlayer(player), player->GetTeamId());
    }

    void OnBattlegroundCreate(Battleground* battleground) override
    {
        if (!battleground)
            return;
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnCreateCallbacks.Fire(mapId, WrapBattleground(battleground));
    }

    void OnBattlegroundUpdate(Battleground* battleground, uint32 diff) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnUpdateLateCallbacks.Fire(mapId, WrapBattleground(battleground), diff);
    }

    void OnBattlegroundEnd(Battleground* battleground, TeamId winner) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnEndLateCallbacks.Fire(mapId, WrapBattleground(battleground), winner);
    }

    void OnBattlegroundStart(Battleground* battleground) override
    {
        std::uint32_t const mapId = battleground->GetMapId();
        ApplyTsWowBattlegroundDoors(battleground, true);
        ts_events.Battleground.OnOpenDoorsCallbacks.Fire(mapId, WrapBattleground(battleground));
    }

    bool CanAppendBattlegroundScore(Battleground* battleground, BattlegroundScore* score,
        WorldPacket& packet) override
    {
        bool cancel = false;
        std::uint32_t const mapId = battleground->GetMapId();
        ts_events.Battleground.OnSendScoreCallbacks.Fire(mapId, WrapBattleground(battleground), WrapBattlegroundScore(score),
            TSWorldPacket(&packet, &WorldPacketApi), TSMutable<bool, bool>(&cancel));
        return !cancel;
    }

    void OnBattlegroundDestroy(Battleground* battleground) override
    {
        for (auto const& score : *battleground->GetPlayerScores())
            BattlegroundScoreAttributes.erase(score.second);
    }
};
}

TSEvents ts_events;

void Addmod_tswowScripts()
{
    ts_events.DatabaseApi = &NativeDatabaseApi;
    ts_events.OutfitApi = &NativeOutfitApi;
    ts_events.Player.OnReloadHandler = ReloadPlayers;
    ts_events.Creature.OnReloadHandler = ReloadCreatures;
    ts_events.GameObject.OnReloadHandler = ReloadGameObjects;
    ts_events.Map.OnReloadHandler = ReloadMaps;
    ts_events.Instance.OnReloadHandler = ReloadInstances;
    ts_events.Battleground.OnReloadHandler = ReloadBattlegrounds;

    new TsWowWorldScript();
    new TsWowFormulaScript();
    new TsWowAccountScript();
    new TsWowAuctionScript();
    new TsWowVehicleScript();
    new TsWowGuildScript();
    new TsWowGroupScript();
    new TsWowGameEventScript();
    new TsWowAreaTriggerScript();
    new TsWowPlayerScript();
    new TsWowMiscScript();
    new TsWowMailScript();
    new TsWowServerScript();
    new TsWowCommandScript();
    new TsWowCreatureScript();
    new TsWowGameObjectScript();
    new TsWowItemScript();
    new TsWowUnitScript();
    new TsWowMapScript();
    new TsWowSpellScript();
    new TsWowSmartScript();
    new TsWowConditionScript();
    new TsWowAchievementScript();
    new TsWowBattlegroundScript();
}
