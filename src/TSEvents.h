#ifndef MOD_TSWOW_EVENTS_H
#define MOD_TSWOW_EVENTS_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class WorldObject;

template <typename T>
using TSNumber = double;

template <typename T>
class TSArray
{
public:
    TSArray() : _values(std::make_shared<std::vector<T>>()) { }
    TSArray(std::initializer_list<T> values) : _values(std::make_shared<std::vector<T>>(values)) { }
    explicit TSArray(std::vector<T> values) : _values(std::make_shared<std::vector<T>>(std::move(values))) { }
    TSArray* operator->() { return this; }
    TSArray const* operator->() const { return this; }
    std::size_t get_length() const { return _values->size(); }
    T& operator[](int index) { return (*_values)[index]; }
    T const& operator[](int index) const { return (*_values)[index]; }
    std::string stringify(int indentation = 0) const;

    template <typename... Args>
    void push(Args&&... values)
    {
        (_values->push_back(std::forward<Args>(values)), ...);
    }

    template <typename Callback>
    void forEach(Callback callback)
    {
        for (std::size_t index = 0; index < _values->size(); ++index)
        {
            if constexpr (std::is_invocable_v<Callback, T, std::size_t, TSArray<T>&>)
                callback((*_values)[index], index, *this);
            else if constexpr (std::is_invocable_v<Callback, T, std::size_t>)
                callback((*_values)[index], index);
            else
                callback((*_values)[index]);
        }
    }

private:
    std::shared_ptr<std::vector<T>> _values;
};

#define CreateArray TSArray

template <typename T, typename Result>
class TSMutable
{
public:
    explicit TSMutable(T* value) : _value(value) { }

    TSMutable* operator->() { return this; }
    void set(Result value) { *_value = static_cast<T>(value); }
    Result get() const { return *_value; }
    std::string stringify() const
    {
        if constexpr (std::is_same_v<T, std::string>)
            return *_value;
        else
            return std::to_string(*_value);
    }

private:
    T* _value;
};

template <typename T>
using TSMutableNumber = TSMutable<T, TSNumber<T>>;

using TSMutableString = TSMutable<std::string, std::string>;

class TSMainThreadContext
{
public:
    TSMainThreadContext* operator->() { return this; }
};

class TSGUID
{
public:
    explicit TSGUID(std::uint64_t guid = 0) : _guid(guid) { }

    TSGUID* operator->() { return this; }
    TSNumber<std::uint32_t> GetCounter() const
    {
        return HasEntry() ? _guid & 0xFFFFFF : _guid & 0xFFFFFFFF;
    }
    TSNumber<std::uint32_t> GetLow() const { return GetCounter(); }
    TSNumber<std::uint32_t> GetType() const { return (_guid >> 48) & 0xFFFF; }
    TSNumber<std::uint32_t> GetEntry() const { return HasEntry() ? (_guid >> 24) & 0xFFFFFF : 0; }
    bool operator==(TSGUID const& other) const { return _guid == other._guid; }
    bool operator!=(TSGUID const& other) const { return !(*this == other); }
    bool IsEmpty() const { return _guid == 0; }
    bool IsCreature() const { return High() == 0xF130; }
    bool IsPet() const { return High() == 0xF140; }
    bool IsVehicle() const { return High() == 0xF150; }
    bool IsCreatureOrPet() const { return IsCreature() || IsPet(); }
    bool IsCreatureOrVehicle() const { return IsCreature() || IsVehicle(); }
    bool IsAnyTypeCreature() const { return IsCreature() || IsPet() || IsVehicle(); }
    bool IsPlayer() const { return !IsEmpty() && High() == 0x0000; }
    bool IsUnit() const { return IsPlayer() || IsAnyTypeCreature(); }
    bool IsItem() const { return High() == 0x4000; }
    bool IsGameObject() const { return High() == 0xF110; }
    bool IsDynamicObject() const { return High() == 0xF100; }
    bool IsCorpse() const { return High() == 0xF101; }
    bool IsTransport() const { return High() == 0xF120; }
    bool IsMOTransport() const { return High() == 0x1FC0; }
    bool IsAnyTypeGameObject() const { return IsGameObject() || IsTransport() || IsMOTransport(); }
    bool IsInstance() const { return High() == 0x1F40; }
    bool IsGroup() const { return High() == 0x1F50; }
    std::string stringify(int = 0) const { return std::to_string(_guid); }

private:
    std::uint32_t High() const { return static_cast<std::uint32_t>((_guid >> 48) & 0xFFFF); }
    bool HasEntry() const
    {
        return IsGameObject() || IsTransport() || IsCreature() || IsPet() || IsVehicle() ||
               IsDynamicObject() || IsCorpse();
    }

    std::uint64_t _guid;
};

inline TSGUID CreateGUID(TSNumber<std::uint32_t> high, TSNumber<std::uint32_t> counter)
{
    std::uint64_t const value = static_cast<std::uint32_t>(counter);
    return TSGUID(value ? value | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 48) : 0);
}

inline TSGUID CreateGUID(TSNumber<std::uint32_t> high, TSNumber<std::uint32_t> entry,
    TSNumber<std::uint32_t> counter)
{
    std::uint64_t const value = static_cast<std::uint32_t>(counter) & 0xFFFFFF;
    return TSGUID(value ? value | ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(entry)) & 0xFFFFFF) << 24) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 48) : 0);
}

inline TSGUID EmptyGUID()
{
    return TSGUID();
}

class TSPlayer;
class TSBattleground;
class TSBattlegroundScore;
struct TSBattlegroundApi;
struct TSBattlegroundScoreApi;
struct TSPlayerApi;
struct TSUnitApi;

struct TSPacketReadApi
{
    void (*ReadBytes)(void*, std::uint32_t, void*);
    std::string (*ReadString)(void*, std::string const&);
    std::uint32_t (*Size)(void*);
    void (*Reset)(void*);
};

class TSPacketRead
{
public:
    explicit TSPacketRead(void* packet = nullptr, TSPacketReadApi const* api = nullptr) :
        _packet(packet), _api(api) { }
    TSPacketRead* operator->() { return this; }
    explicit operator bool() const { return _packet != nullptr; }

    template <typename T>
    T Read(T defaultValue)
    {
        if (!_api || !_packet)
            return defaultValue;
        T value = defaultValue;
        _api->ReadBytes(_packet, sizeof(T), &value);
        return value;
    }

    TSNumber<std::uint8_t> ReadUInt8(std::uint8_t value = 0) { return Read(value); }
    TSNumber<std::int8_t> ReadInt8(std::int8_t value = 0) { return Read(value); }
    TSNumber<std::uint16_t> ReadUInt16(std::uint16_t value = 0) { return Read(value); }
    TSNumber<std::int16_t> ReadInt16(std::int16_t value = 0) { return Read(value); }
    TSNumber<std::uint32_t> ReadUInt32(std::uint32_t value = 0) { return Read(value); }
    TSNumber<std::int32_t> ReadInt32(std::int32_t value = 0) { return Read(value); }
    TSNumber<std::uint64_t> ReadUInt64(std::uint64_t value = 0) { return static_cast<double>(Read(value)); }
    TSNumber<std::int64_t> ReadInt64(std::int64_t value = 0) { return static_cast<double>(Read(value)); }
    TSNumber<float> ReadFloat(float value = 0) { return Read(value); }
    TSNumber<double> ReadDouble(double value = 0) { return Read(value); }
    std::string ReadString(std::string const& value = "")
    {
        return _api && _packet ? _api->ReadString(_packet, value) : value;
    }
    std::uint32_t Size() const { return _api && _packet ? _api->Size(_packet) : 0; }
    void Reset() const
    {
        if (_api && _packet)
            _api->Reset(_packet);
    }

private:
    void* _packet;
    TSPacketReadApi const* _api;
};

struct TSMapApi
{
    bool (*IsBG)(void*);
    void* (*GetBattleground)(void*);
    TSBattlegroundApi const* BattlegroundApi;
    TSBattlegroundScoreApi const* BattlegroundScoreApi;
};

class TSMap
{
public:
    explicit TSMap(void* map = nullptr, TSMapApi const* api = nullptr) : __this(map), _map(map), _api(api) { }
    TSMap* operator->() { return this; }
    explicit operator bool() const { return _map != nullptr; }
    bool IsNull() const { return _map == nullptr; }
    void* GetNativeHandle() const { return _map; }
    bool IsBG() const { return _api && _map && _api->IsBG(_map); }
    TSBattleground ToBG() const;
    void* __this;

protected:
    void* _map;
    TSMapApi const* _api;
};

struct TSBattlegroundApi
{
    void (*UpdateWorldState)(void*, std::uint32_t, std::uint32_t);
    void (*EndBG)(void*, std::uint32_t);
    void (*RewardHonor)(void*, std::uint32_t, std::uint32_t);
    std::size_t (*GetPlayerCount)(void*);
    void* (*GetPlayerAt)(void*, std::size_t);
    void* (*GetScore)(void*, std::uint32_t);
    std::int32_t (*GetStartDelayTime)(void*);
    void (*SetStartDelayTime)(void*, std::int32_t);
    TSPlayerApi const* PlayerApi;
    TSUnitApi const* UnitApi;
};

class TSBattleground : public TSMap
{
public:
    explicit TSBattleground(void* battleground = nullptr, void* map = nullptr,
        TSMapApi const* mapApi = nullptr, TSBattlegroundApi const* battlegroundApi = nullptr,
        TSBattlegroundScoreApi const* scoreApi = nullptr) : TSMap(map, mapApi),
        __this(battleground), _battleground(battleground), _battlegroundApi(battlegroundApi), _scoreApi(scoreApi) { }
    TSBattleground* operator->() { return this; }
    void* GetNativeBattlegroundHandle() const { return _battleground; }
    void UpdateWorldState(std::uint32_t variable, std::uint32_t value) const
    {
        if (_battlegroundApi && _battleground)
            _battlegroundApi->UpdateWorldState(_battleground, variable, value);
    }
    void EndBG(std::uint32_t winnerTeam = 2) const
    {
        if (_battlegroundApi && _battleground)
            _battlegroundApi->EndBG(_battleground, winnerTeam);
    }
    void RewardHonor(std::uint32_t honor, std::uint32_t team = 2) const
    {
        if (_battlegroundApi && _battleground)
            _battlegroundApi->RewardHonor(_battleground, honor, team);
    }
    TSArray<TSPlayer> GetPlayers() const;
    TSBattlegroundScore GetScore(std::uint32_t guid) const;
    TSNumber<std::int32_t> GetStartDelayTime() const
    {
        return _battlegroundApi && _battleground ? _battlegroundApi->GetStartDelayTime(_battleground) : 0;
    }
    void SetStartDelayTime(std::int32_t time) const
    {
        if (_battlegroundApi && _battleground)
            _battlegroundApi->SetStartDelayTime(_battleground, time);
    }
    void* __this;

private:
    void* _battleground;
    TSBattlegroundApi const* _battlegroundApi;
    TSBattlegroundScoreApi const* _scoreApi;
};

inline TSBattleground TSMap::ToBG() const
{
    void* battleground = _api && _api->GetBattleground && _map ? _api->GetBattleground(_map) : nullptr;
    return TSBattleground(battleground, _map, _api, _api ? _api->BattlegroundApi : nullptr,
        _api ? _api->BattlegroundScoreApi : nullptr);
}

struct TSWorldPacketApi
{
    void (*WriteBytes)(void*, char const*, std::uint32_t);
    void (*WriteString)(void*, std::string const&);
};

class TSWorldPacket
{
public:
    explicit TSWorldPacket(void* packet = nullptr, TSWorldPacketApi const* api = nullptr) :
        _packet(packet), _api(api) { }
    TSWorldPacket* operator->() { return this; }
    explicit operator bool() const { return _packet != nullptr; }

    template <typename T>
    void Write(T value)
    {
        if (_api && _packet)
            _api->WriteBytes(_packet, reinterpret_cast<char const*>(&value), sizeof(T));
    }

    void WriteUInt8(std::uint8_t value) { Write(value); }
    void WriteInt8(std::int8_t value) { Write(value); }
    void WriteUInt16(std::uint16_t value) { Write(value); }
    void WriteInt16(std::int16_t value) { Write(value); }
    void WriteUInt32(std::uint32_t value) { Write(value); }
    void WriteInt32(std::int32_t value) { Write(value); }
    void WriteUInt64(std::uint64_t value) { Write(value); }
    void WriteInt64(std::int64_t value) { Write(value); }
    void WriteFloat(float value) { Write(value); }
    void WriteDouble(double value) { Write(value); }
    void WriteString(std::string const& value)
    {
        if (_api && _packet)
            _api->WriteString(_packet, value);
    }

    void* GetNativeHandle() const { return _packet; }

private:
    void* _packet;
    TSWorldPacketApi const* _api;
};

struct TSBattlegroundScoreApi
{
    void (*AppendBaseToPacket)(void*, void*);
    std::uint32_t (*GetCustomAttr)(void*, std::string const&);
    void (*SetCustomAttr)(void*, std::string const&, std::uint32_t);
    void (*ModCustomAttr)(void*, std::string const&, std::int32_t);
};

class TSBattlegroundScore
{
public:
    explicit TSBattlegroundScore(void* score = nullptr, TSBattlegroundScoreApi const* api = nullptr) :
        _score(score), _api(api) { }
    TSBattlegroundScore* operator->() { return this; }
    explicit operator bool() const { return _score != nullptr; }
    void ApplyBaseToPacket(TSBattleground, TSWorldPacket packet) const
    {
        if (_api && _score && packet)
            _api->AppendBaseToPacket(_score, packet.GetNativeHandle());
    }
    TSNumber<std::uint32_t> GetCustomAttr(std::string const& key) const
    {
        return _api && _score ? _api->GetCustomAttr(_score, key) : 0;
    }
    void SetCustomAttr(std::string const& key, std::uint32_t value) const
    {
        if (_api && _score)
            _api->SetCustomAttr(_score, key, value);
    }
    void ModCustomAttr(std::string const& key, std::int32_t value) const
    {
        if (_api && _score)
            _api->ModCustomAttr(_score, key, value);
    }

private:
    void* _score;
    TSBattlegroundScoreApi const* _api;
};

inline TSBattlegroundScore TSBattleground::GetScore(std::uint32_t guid) const
{
    void* score = _battlegroundApi && _battleground ? _battlegroundApi->GetScore(_battleground, guid) : nullptr;
    return TSBattlegroundScore(score, _scoreApi);
}

struct TSUnitApi
{
    bool (*IsPlayer)(void*);
    void* (*GetEffectiveOwner)(void*);
    std::uint32_t (*GetGUIDLow)(void*);
    std::uint32_t (*GetMapID)(void*);
    void* (*GetMap)(void*);
    void (*Respawn)(void*);
    void (*RemoveCorpse)(void*);
    TSMapApi const* MapApi;
};

struct TSPlayerApi
{
    std::uint64_t (*GetGUID)(void*);
    void (*SendBroadcastMessage)(void*, char const*);
    std::uint32_t (*GetClass)(void*);
    std::uint32_t (*GetMapID)(void*);
    std::uint32_t (*GetQuestRewardTalentPoints)(void*);
    std::uint32_t (*GetTeam)(void*);
    void (*GossipMenuAddItem)(void*, std::uint32_t, char const*, std::uint32_t, std::uint32_t, bool,
        char const*, std::uint32_t);
    void (*GossipComplete)(void*);
    void (*GossipSendTextMenu)(void*, void*, char const*, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    void (*GossipClearMenu)(void*);
    void (*SetTaxiCheat)(void*, bool);
    void (*SendUpdateWorldState)(void*, std::uint32_t, std::uint32_t);
    void (*SendCustomPacket)(void*, std::uint16_t, char const*, std::uint32_t);
    void (*SendAddonMessage)(void*, char const*, char const*, std::uint8_t, void*);
    bool (*Teleport)(void*, std::uint32_t, float, float, float, float);
    bool (*HasItem)(void*, std::uint32_t, std::uint32_t, bool);
    std::uint16_t (*GetSkillValue)(void*, std::uint32_t);
    void (*SetSkill)(void*, std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t);
    void (*PlayDirectSound)(void*, std::uint32_t, void*);
    bool (*AddItem)(void*, std::uint32_t, std::uint32_t);
    void (*AreaExploredOrEventHappens)(void*, std::uint32_t);
    void (*LearnSpell)(void*, std::uint32_t);
};

class TSUnit
{
public:
    explicit TSUnit(void* unit = nullptr, TSUnitApi const* api = nullptr,
        TSPlayerApi const* playerApi = nullptr) : __this(unit), _unit(unit), _api(api), _playerApi(playerApi) { }
    TSUnit* operator->() { return this; }
    explicit operator bool() const { return _unit != nullptr; }
    bool IsNull() const { return _unit == nullptr; }
    bool operator==(TSUnit const& other) const { return _unit == other._unit; }
    void* GetNativeHandle() const { return _unit; }
    bool IsPlayer() const { return _api && _unit && _api->IsPlayer(_unit); }
    TSPlayer ToPlayer() const;
    TSUnit GetEffectiveOwner() const
    {
        return TSUnit(_api && _unit ? _api->GetEffectiveOwner(_unit) : nullptr, _api, _playerApi);
    }
    TSNumber<std::uint32_t> GetGUIDLow() const { return _api && _unit ? _api->GetGUIDLow(_unit) : 0; }
    TSNumber<std::uint32_t> GetMapID() const { return _api && _unit ? _api->GetMapID(_unit) : 0; }
    TSMap GetMap() const
    {
        return TSMap(_api && _unit ? _api->GetMap(_unit) : nullptr, _api ? _api->MapApi : nullptr);
    }
    void* __this;

protected:
    void* _unit;
    TSUnitApi const* _api;
    TSPlayerApi const* _playerApi;
};

class TSCreature : public TSUnit
{
public:
    explicit TSCreature(void* creature = nullptr, TSUnitApi const* api = nullptr,
        TSPlayerApi const* playerApi = nullptr) : TSUnit(creature, api, playerApi) { }
    TSCreature* operator->() { return this; }
    void Respawn() const
    {
        if (_api && _unit)
            _api->Respawn(_unit);
    }
    void RemoveCorpse() const
    {
        if (_api && _unit)
            _api->RemoveCorpse(_unit);
    }
};

class TSPlayer : public TSUnit
{
public:
    explicit TSPlayer(void* player = nullptr, TSPlayerApi const* api = nullptr,
        TSUnitApi const* unitApi = nullptr) : TSUnit(player, unitApi, api), _playerApiImpl(api) { }
    TSPlayer* operator->() { return this; }
    TSGUID GetGUID() const { return TSGUID(_playerApiImpl && _unit ? _playerApiImpl->GetGUID(_unit) : 0); }
    void SendBroadcastMessage(std::string const& message) const
    {
        if (_playerApiImpl && _unit && !message.empty())
            _playerApiImpl->SendBroadcastMessage(_unit, message.c_str());
    }
    TSNumber<std::uint32_t> GetClass() const { return _playerApiImpl && _unit ? _playerApiImpl->GetClass(_unit) : 0; }
    TSNumber<std::uint32_t> GetMapID() const { return _playerApiImpl && _unit ? _playerApiImpl->GetMapID(_unit) : 0; }
    TSNumber<std::uint32_t> GetQuestRewardTempTalentPoints() const
    {
        return _playerApiImpl && _unit ? _playerApiImpl->GetQuestRewardTalentPoints(_unit) : 0;
    }
    TSNumber<std::uint32_t> GetTeam() const
    {
        return _playerApiImpl && _unit ? _playerApiImpl->GetTeam(_unit) : 2;
    }
    void GossipMenuAddItem(std::uint32_t icon, std::string const& message, std::uint32_t sender = 0,
        std::uint32_t action = 0, bool code = false, std::string const& prompt = "",
        std::uint32_t money = 0) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->GossipMenuAddItem(_unit, icon, message.c_str(), sender, action, code, prompt.c_str(), money);
    }
    void GossipComplete() const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->GossipComplete(_unit);
    }
    void GossipSendTextMenu(TSUnit sender, std::string const& text, std::uint32_t language = 0,
        std::uint32_t emote0 = 0, std::uint32_t emote0Delay = 0, std::uint32_t emote1 = 0,
        std::uint32_t emote1Delay = 0, std::uint32_t emote2 = 0, std::uint32_t emote2Delay = 0,
        std::uint32_t menuId = 0) const
    {
        if (_playerApiImpl && _unit && sender)
            _playerApiImpl->GossipSendTextMenu(_unit, sender.GetNativeHandle(), text.c_str(), language, emote0,
                emote0Delay, emote1, emote1Delay, emote2, emote2Delay, menuId);
    }
    void GossipClearMenu() const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->GossipClearMenu(_unit);
    }
    TSPlayer ToPlayer() const { return *this; }
    void SetTaxiCheat(bool enabled) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->SetTaxiCheat(_unit, enabled);
    }
    void SendUpdateWorldState(std::uint32_t variable, std::uint32_t value) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->SendUpdateWorldState(_unit, variable, value);
    }
    void SendCustomPacket(std::uint16_t opcode, char const* data, std::uint32_t size) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->SendCustomPacket(_unit, opcode, data, size);
    }
    void SendAddonMessage(std::string const& prefix, std::string const& message, std::uint8_t channel,
        TSPlayer receiver) const
    {
        if (_playerApiImpl && _unit && receiver)
            _playerApiImpl->SendAddonMessage(_unit, prefix.c_str(), message.c_str(), channel,
                receiver.GetNativeHandle());
    }
    bool Teleport(std::uint32_t map, float x, float y, float z, float orientation) const
    {
        return _playerApiImpl && _unit && _playerApiImpl->Teleport(_unit, map, x, y, z, orientation);
    }
    bool HasItem(std::uint32_t item, std::uint32_t count = 1, bool checkBank = false) const
    {
        return _playerApiImpl && _unit && _playerApiImpl->HasItem(_unit, item, count, checkBank);
    }
    TSNumber<std::uint16_t> GetSkillValue(std::uint32_t skill) const
    {
        return _playerApiImpl && _unit ? _playerApiImpl->GetSkillValue(_unit, skill) : 0;
    }
    void SetSkill(std::uint16_t id, std::uint16_t step, std::uint16_t value, std::uint16_t maximum) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->SetSkill(_unit, id, step, value, maximum);
    }
    void PlayDirectSound(std::uint32_t sound, TSPlayer receiver) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->PlayDirectSound(_unit, sound, receiver.GetNativeHandle());
    }
    bool AddItem(std::uint32_t item, std::uint32_t count) const
    {
        return _playerApiImpl && _unit && _playerApiImpl->AddItem(_unit, item, count);
    }
    void AreaExploredOrEventHappens(std::uint32_t quest) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->AreaExploredOrEventHappens(_unit, quest);
    }
    void LearnSpell(std::uint32_t spell) const
    {
        if (_playerApiImpl && _unit)
            _playerApiImpl->LearnSpell(_unit, spell);
    }

private:
    TSPlayerApi const* _playerApiImpl;
};

inline TSArray<TSPlayer> TSBattleground::GetPlayers() const
{
    TSArray<TSPlayer> players;
    if (!_battlegroundApi || !_battleground)
        return players;
    std::size_t const count = _battlegroundApi->GetPlayerCount(_battleground);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (void* player = _battlegroundApi->GetPlayerAt(_battleground, index))
            players.push(TSPlayer(player, _battlegroundApi->PlayerApi, _battlegroundApi->UnitApi));
    }
    return players;
}

class TSPacketWrite
{
public:
    explicit TSPacketWrite(std::uint16_t opcode = 0, std::uint32_t size = 0) : _opcode(opcode)
    {
        _data.reserve(size);
    }
    TSPacketWrite* operator->() { return this; }
    explicit operator bool() const { return true; }

    template <typename T>
    TSPacketWrite* Write(T value)
    {
        char const* bytes = reinterpret_cast<char const*>(&value);
        _data.insert(_data.end(), bytes, bytes + sizeof(T));
        return this;
    }

    TSPacketWrite* WriteUInt8(std::uint8_t value) { return Write(value); }
    TSPacketWrite* WriteInt8(std::int8_t value) { return Write(value); }
    TSPacketWrite* WriteUInt16(std::uint16_t value) { return Write(value); }
    TSPacketWrite* WriteInt16(std::int16_t value) { return Write(value); }
    TSPacketWrite* WriteUInt32(std::uint32_t value) { return Write(value); }
    TSPacketWrite* WriteInt32(std::int32_t value) { return Write(value); }
    TSPacketWrite* WriteUInt64(std::uint64_t value) { return Write(value); }
    TSPacketWrite* WriteInt64(std::int64_t value) { return Write(value); }
    TSPacketWrite* WriteFloat(float value) { return Write(value); }
    TSPacketWrite* WriteDouble(double value) { return Write(value); }
    TSPacketWrite* WriteString(std::string const& value)
    {
        Write<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
        _data.insert(_data.end(), value.begin(), value.end());
        return this;
    }
    std::uint32_t Size() const { return static_cast<std::uint32_t>(_data.size()); }
    void SendToPlayer(TSPlayer player) const
    {
        player.SendCustomPacket(_opcode, _data.data(), Size());
    }

private:
    std::uint16_t _opcode;
    std::vector<char> _data;
};

inline TSPacketWrite CreateCustomPacket(TSNumber<std::uint32_t> opcode, TSNumber<std::uint32_t> size)
{
    return TSPacketWrite(static_cast<std::uint16_t>(opcode), static_cast<std::uint32_t>(size));
}

inline TSPlayer TSUnit::ToPlayer() const
{
    return TSPlayer(IsPlayer() ? _unit : nullptr, _playerApi, _api);
}

class TSSpell
{
public:
    explicit TSSpell(void* spell = nullptr, TSUnit caster = TSUnit()) : _spell(spell), _caster(caster) { }
    TSSpell* operator->() { return this; }
    bool IsNull() const { return _spell == nullptr; }
    TSUnit GetCaster() const { return _caster; }

private:
    void* _spell;
    TSUnit _caster;
};

template <typename Tag>
class TSOpaque
{
public:
    explicit TSOpaque(void* value = nullptr) : _value(value) { }
    TSOpaque* operator->() { return this; }
    explicit operator bool() const { return _value != nullptr; }
    bool IsNull() const { return _value == nullptr; }
    bool operator==(TSOpaque const& other) const { return _value == other._value; }
    void* GetNativeHandle() const { return _value; }

private:
    void* _value;
};

struct TSAuctionHouseObjectTag;
struct TSAuctionEntryTag;
struct TSGuildTag;
struct TSGroupTag;
struct TSVehicleTag;
struct TSGameObjectTag;
struct TSWorldObjectTag;
struct TSItemTag;
struct TSItemTemplateTag;
struct TSQuestTag;
struct TSChannelTag;
struct TSAchievementEntryTag;
struct TSAchievementCriteriaEntryTag;
struct TSObjectTag;
struct TSMailDraftTag;
struct TSLootTag;
struct TSLootItemTag;
struct TSCorpseTag;
struct TSSpellInfoTag;
struct TSSpellEffectInfoTag;
struct TSFactionTemplateTag;
struct TSAreaTriggerEntryTag;
struct TSInstanceTag;
struct TSWorldStatePacketTag;
struct TSAuraTag;
struct TSAuraEffectTag;
struct TSAuraApplicationTag;
struct TSDispelInfoTag;
struct TSDamageInfoTag;
struct TSProcEventInfoTag;
struct TSSpellModifierTag;
struct TSMeleeDamageInfoTag;
struct TSWeatherTag;
struct TSConditionTag;
struct TSConditionSourceInfoTag;
struct TSSmartScriptValuesTag;
struct TSSpellDamageInfoTag;
struct TSWorldObjectCollectionTag;
struct TSSpellDestinationTag;
struct TSSpellImplicitTargetInfoTag;
using TSAuctionHouseObject = TSOpaque<TSAuctionHouseObjectTag>;
using TSAuctionEntry = TSOpaque<TSAuctionEntryTag>;
using TSGuild = TSOpaque<TSGuildTag>;
using TSGroup = TSOpaque<TSGroupTag>;
using TSVehicle = TSOpaque<TSVehicleTag>;
using TSGameObject = TSOpaque<TSGameObjectTag>;
using TSWorldObject = TSOpaque<TSWorldObjectTag>;
using TSItem = TSOpaque<TSItemTag>;
using TSItemTemplate = TSOpaque<TSItemTemplateTag>;
using TSQuest = TSOpaque<TSQuestTag>;
using TSChannel = TSOpaque<TSChannelTag>;
using TSAchievementEntry = TSOpaque<TSAchievementEntryTag>;
using TSAchievementCriteriaEntry = TSOpaque<TSAchievementCriteriaEntryTag>;
using TSObject = TSOpaque<TSObjectTag>;
using TSMailDraft = TSOpaque<TSMailDraftTag>;
using TSLoot = TSOpaque<TSLootTag>;
using TSLootItem = TSOpaque<TSLootItemTag>;
using TSCorpse = TSOpaque<TSCorpseTag>;
using TSSpellInfo = TSOpaque<TSSpellInfoTag>;
using TSSpellEffectInfo = TSOpaque<TSSpellEffectInfoTag>;
using TSFactionTemplate = TSOpaque<TSFactionTemplateTag>;
using TSAreaTriggerEntry = TSOpaque<TSAreaTriggerEntryTag>;
using TSInstance = TSOpaque<TSInstanceTag>;
using TSWorldStatePacket = TSOpaque<TSWorldStatePacketTag>;
using TSAura = TSOpaque<TSAuraTag>;
using TSAuraEffect = TSOpaque<TSAuraEffectTag>;
using TSAuraApplication = TSOpaque<TSAuraApplicationTag>;
using TSDispelInfo = TSOpaque<TSDispelInfoTag>;
using TSDamageInfo = TSOpaque<TSDamageInfoTag>;
using TSProcEventInfo = TSOpaque<TSProcEventInfoTag>;
using TSSpellModifier = TSOpaque<TSSpellModifierTag>;
using TSMeleeDamageInfo = TSOpaque<TSMeleeDamageInfoTag>;
using TSWeather = TSOpaque<TSWeatherTag>;
using TSCondition = TSOpaque<TSConditionTag>;
using TSConditionSourceInfo = TSOpaque<TSConditionSourceInfoTag>;
using TSSmartScriptValues = TSOpaque<TSSmartScriptValuesTag>;
using TSSpellDamageInfo = TSOpaque<TSSpellDamageInfoTag>;
using TSWorldObjectCollection = TSOpaque<TSWorldObjectCollectionTag>;
using TSSpellDestination = TSOpaque<TSSpellDestinationTag>;
using TSSpellImplicitTargetInfo = TSOpaque<TSSpellImplicitTargetInfoTag>;

class TSMutableWorldObject
{
public:
    explicit TSMutableWorldObject(WorldObject** value) : _value(value) { }
    TSMutableWorldObject* operator->() { return this; }
    TSWorldObject get() const { return TSWorldObject(*_value); }
    void set(TSWorldObject value) { *_value = static_cast<WorldObject*>(value.GetNativeHandle()); }

private:
    WorldObject** _value;
};

template <typename Callback>
class TSEvent
{
public:
    void Add(Callback callback) { _callbacks.push_back(std::move(callback)); }
    void Clear() { _callbacks.clear(); }

    template <typename... Args>
    void Fire(Args&&... args)
    {
        for (Callback const& callback : _callbacks)
            callback(std::forward<Args>(args)...);
    }

private:
    std::vector<Callback> _callbacks;
};

template <typename Callback>
class TSMappedEvent
{
public:
    void Add(Callback callback) { _all.Add(std::move(callback)); }
    void Add(std::uint32_t id, Callback callback) { _byId[id].Add(std::move(callback)); }
    void Clear()
    {
        _all.Clear();
        _byId.clear();
    }

    template <typename... Args>
    void Fire(std::uint32_t id, Args&&... args)
    {
        _all.Fire(std::forward<Args>(args)...);
        auto found = _byId.find(id);
        if (found != _byId.end())
            found->second.Fire(std::forward<Args>(args)...);
    }

private:
    TSEvent<Callback> _all;
    std::unordered_map<std::uint32_t, TSEvent<Callback>> _byId;
};

#define TS_EVENT(name, ...) \
    using name##Callback = std::function<void(__VA_ARGS__)>; \
    TSEvent<name##Callback> name##Callbacks; \
    void name(name##Callback callback) { name##Callbacks.Add(std::move(callback)); }

#define TS_ID_EVENT(name, ...) \
    using name##Callback = std::function<void(__VA_ARGS__)>; \
    TSMappedEvent<name##Callback> name##Callbacks; \
    void name(name##Callback callback) { name##Callbacks.Add(std::move(callback)); } \
    void name(std::uint32_t id, name##Callback callback) { name##Callbacks.Add(id, std::move(callback)); } \
    void name(TSArray<std::uint32_t> const& ids, name##Callback callback) { \
        for (std::size_t index = 0; index < ids.get_length(); ++index) \
            name##Callbacks.Add(ids[static_cast<int>(index)], callback); \
    }

struct TSEvents
{
    struct AchievementEvents
    {
        AchievementEvents* operator->() { return this; }

        TS_ID_EVENT(OnComplete, TSPlayer, TSAchievementEntry)
        TS_ID_EVENT(OnUpdate, TSPlayer, TSAchievementEntry, TSAchievementCriteriaEntry,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, bool)

        void Clear()
        {
            OnCompleteCallbacks.Clear();
            OnUpdateCallbacks.Clear();
        }
    } Achievement;

    struct AuctionEvents
    {
        AuctionEvents* operator->() { return this; }

        TS_EVENT(OnAuctionAdd, TSAuctionHouseObject, TSAuctionEntry)
        TS_EVENT(OnAuctionRemove, TSAuctionHouseObject, TSAuctionEntry)
        TS_EVENT(OnAuctionSuccessful, TSAuctionHouseObject, TSAuctionEntry)
        TS_EVENT(OnAuctionExpire, TSAuctionHouseObject, TSAuctionEntry)

        void Clear()
        {
            OnAuctionAddCallbacks.Clear();
            OnAuctionRemoveCallbacks.Clear();
            OnAuctionSuccessfulCallbacks.Clear();
            OnAuctionExpireCallbacks.Clear();
        }
    } Auction;

    struct VehicleEvents
    {
        VehicleEvents* operator->() { return this; }

        TS_ID_EVENT(OnInstall, TSVehicle)
        TS_ID_EVENT(OnUninstall, TSVehicle)
        TS_ID_EVENT(OnReset, TSVehicle)
        TS_ID_EVENT(OnAddPassenger, TSVehicle, TSUnit, TSNumber<std::uint8_t>)
        TS_ID_EVENT(OnRemovePassenger, TSVehicle, TSUnit, TSNumber<std::uint8_t>)

        void Clear()
        {
            OnInstallCallbacks.Clear();
            OnUninstallCallbacks.Clear();
            OnResetCallbacks.Clear();
            OnAddPassengerCallbacks.Clear();
            OnRemovePassengerCallbacks.Clear();
        }
    } Vehicle;

    struct WorldEvents
    {
        WorldEvents* operator->() { return this; }

        TS_EVENT(OnOpenStateChange, bool)
        TS_EVENT(OnConfigLoad, bool)
        TS_EVENT(OnMotdChange, std::string const&)
        TS_EVENT(OnShutdownInitiate, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnUpdate, TSNumber<std::uint32_t>, TSMainThreadContext)
        TS_EVENT(OnStartup)
        TS_EVENT(OnShutdownCancel)
        TS_EVENT(OnShutdown)
        TS_EVENT(OnCalcHonor, TSMutableNumber<float>, TSNumber<std::uint8_t>, TSNumber<float>)

        void Clear()
        {
            OnOpenStateChangeCallbacks.Clear();
            OnConfigLoadCallbacks.Clear();
            OnMotdChangeCallbacks.Clear();
            OnShutdownInitiateCallbacks.Clear();
            OnUpdateCallbacks.Clear();
            OnStartupCallbacks.Clear();
            OnShutdownCancelCallbacks.Clear();
            OnShutdownCallbacks.Clear();
            OnCalcHonorCallbacks.Clear();
        }
    } World;

    struct AccountEvents
    {
        AccountEvents* operator->() { return this; }

        TS_EVENT(OnAccountLogin, TSNumber<std::uint32_t>)
        TS_EVENT(OnFailedAccountLogin, TSNumber<std::uint32_t>)
        TS_EVENT(OnEmailChange, TSNumber<std::uint32_t>)
        TS_EVENT(OnFailedEmailChange, TSNumber<std::uint32_t>)
        TS_EVENT(OnPasswordChange, TSNumber<std::uint32_t>)
        TS_EVENT(OnFailedPasswordChange, TSNumber<std::uint32_t>)

        void Clear()
        {
            OnAccountLoginCallbacks.Clear();
            OnFailedAccountLoginCallbacks.Clear();
            OnEmailChangeCallbacks.Clear();
            OnFailedEmailChangeCallbacks.Clear();
            OnPasswordChangeCallbacks.Clear();
            OnFailedPasswordChangeCallbacks.Clear();
        }
    } Account;

    struct PlayerEvents
    {
        PlayerEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSPlayer, bool)>;
        TSEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback);
        }

        TS_EVENT(OnLevelChanged, TSPlayer, TSNumber<std::uint8_t>)
        TS_EVENT(OnPVPKill, TSPlayer, TSPlayer)
        TS_EVENT(OnCreatureKill, TSPlayer, TSCreature)
        TS_EVENT(OnPlayerKilledByCreature, TSCreature, TSPlayer)
        TS_EVENT(OnFreeTalentPointsChanged, TSPlayer, TSNumber<std::uint32_t>)
        TS_EVENT(OnTalentsReset, TSPlayer, bool)
        TS_EVENT(OnTalentsResetEarly, TSPlayer, TSMutable<bool, bool>)
        TS_EVENT(OnTalentsResetLate, TSPlayer, bool)
        TS_EVENT(OnMoneyChanged, TSPlayer, TSMutableNumber<std::int32_t>)
        TS_EVENT(OnMoneyLimit, TSPlayer, TSNumber<std::int32_t>)
        TS_EVENT(OnGiveXP, TSPlayer, TSMutableNumber<std::uint32_t>, TSUnit)
        TS_EVENT(OnReputationChange, TSPlayer, TSNumber<std::uint32_t>, TSMutableNumber<std::int32_t>, bool)
        TS_EVENT(OnDuelRequest, TSPlayer, TSPlayer)
        TS_EVENT(OnDuelStart, TSPlayer, TSPlayer)
        TS_EVENT(OnDuelEnd, TSPlayer, TSPlayer, TSNumber<std::uint32_t>)
        TS_EVENT(OnSay, TSPlayer, TSMutableString, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnWhisper, TSPlayer, TSPlayer, TSMutableString, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnChatGroup, TSPlayer, TSGroup, TSMutableString, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>)
        TS_EVENT(OnChatGuild, TSPlayer, TSGuild, TSMutableString, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>)
        TS_EVENT(OnChat, TSPlayer, TSChannel, TSMutableString, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>)
        TS_EVENT(OnCommand, TSPlayer, TSMutableString, TSMutable<bool, bool>)
        TS_EVENT(OnEmote, TSPlayer, TSNumber<std::uint32_t>)
        TS_EVENT(OnTextEmote, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, TSNumber<std::uint64_t>)
        TS_EVENT(OnSpellCast, TSPlayer, TSSpell, bool)
        TS_EVENT(OnLogin, TSPlayer, bool)
        TS_EVENT(OnLogout, TSPlayer)
        TS_EVENT(OnCreate, TSPlayer)
        TS_EVENT(OnCreateEarly, TSPlayer)
        TS_EVENT(OnDelete, TSNumber<std::uint64_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnFailedDelete, TSNumber<std::uint64_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnSave, TSPlayer)
        TS_EVENT(OnBindToInstance, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, bool,
            TSNumber<std::uint8_t>)
        TS_EVENT(OnUpdateZone, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnMapChanged, TSPlayer)
        TS_EVENT(OnMovieComplete, TSPlayer, TSNumber<std::uint32_t>)
        TS_EVENT(OnPlayerRepop, TSPlayer)
        TS_EVENT(OnUpdateMaxHealth, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateMaxPower, TSPlayer, TSMutableNumber<float>, TSNumber<std::int8_t>, TSNumber<float>)
        TS_EVENT(OnUpdateResistance, TSPlayer, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnUpdateArmor, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateAttackPower, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateRangedAttackPower, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateShieldBlock, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnUpdateBlockPercentage, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateCrit, TSPlayer, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnUpdateParryPercentage, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateDodgePercentage, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateSpellCrit, TSPlayer, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnUpdateArmorPenetration, TSPlayer, TSMutableNumber<std::int32_t>)
        TS_EVENT(OnUpdateMeleeHitChances, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateRangedHitChances, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateSpellHitChances, TSPlayer, TSMutableNumber<float>)
        TS_EVENT(OnUpdateExpertise, TSPlayer, TSMutableNumber<std::int32_t>, TSNumber<std::uint32_t>, TSItem)
        TS_EVENT(OnUpdateManaRegen, TSPlayer, TSMutableNumber<float>, TSMutableNumber<float>,
            TSMutableNumber<std::int32_t>)
        TS_EVENT(OnUpdateRuneRegen, TSPlayer, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnReputationPriceDiscount, TSPlayer, TSFactionTemplate, TSCreature,
            TSMutableNumber<float>)
        TS_EVENT(OnCalcTalentPoints, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnCalcStaminaHealthBonus, TSPlayer, TSMutableNumber<float>, TSNumber<float>, TSNumber<float>)
        TS_EVENT(OnCalcIntellectManaBonus, TSPlayer, TSMutableNumber<float>, TSNumber<float>, TSNumber<float>)
        TS_EVENT(OnCalcGreyLevel, TSPlayer, TSMutableNumber<std::uint8_t>)
        TS_EVENT(OnCalcZeroDiff, TSPlayer, TSMutableNumber<std::uint8_t>)
        TS_EVENT(OnCalcGroupGain, TSPlayer, TSMutableNumber<float>, TSNumber<std::uint32_t>, bool)
        TS_EVENT(OnCalcSkillGainChance, TSPlayer, TSMutableNumber<std::int32_t>, TSNumber<std::int32_t>,
            TSNumber<std::int32_t>, TSNumber<std::int32_t>, TSNumber<std::int32_t>, TSNumber<std::int32_t>)
        TS_EVENT(OnGlyphInitForLevel, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnSendMail, TSPlayer, TSMailDraft, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnQuestObjectiveProgress, TSPlayer, TSQuest, TSNumber<std::uint32_t>, TSNumber<std::uint16_t>)
        TS_EVENT(OnQuestStatusChange, TSPlayer, TSNumber<std::uint32_t>)
        TS_EVENT(OnLearnTalent, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_EVENT(OnTradeCompleted, TSPlayer, TSPlayer, TSArray<TSItem>, TSArray<TSItem>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnGossipSelect, TSPlayer, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            TSMutable<bool, bool>)
        TS_EVENT(OnGossipSelectCode, TSPlayer, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            std::string const&, TSMutable<bool, bool>)
        TS_EVENT(OnGenerateItemLoot, TSPlayer, TSItem, TSLoot, TSNumber<std::uint32_t>)
        TS_EVENT(OnLootCorpse, TSPlayer, TSCorpse)

        void Clear()
        {
            OnReloadCallbacks.Clear();
            OnLevelChangedCallbacks.Clear();
            OnPVPKillCallbacks.Clear();
            OnCreatureKillCallbacks.Clear();
            OnPlayerKilledByCreatureCallbacks.Clear();
            OnFreeTalentPointsChangedCallbacks.Clear();
            OnTalentsResetCallbacks.Clear();
            OnTalentsResetEarlyCallbacks.Clear();
            OnTalentsResetLateCallbacks.Clear();
            OnMoneyChangedCallbacks.Clear();
            OnMoneyLimitCallbacks.Clear();
            OnGiveXPCallbacks.Clear();
            OnReputationChangeCallbacks.Clear();
            OnDuelRequestCallbacks.Clear();
            OnDuelStartCallbacks.Clear();
            OnDuelEndCallbacks.Clear();
            OnSayCallbacks.Clear();
            OnWhisperCallbacks.Clear();
            OnChatGroupCallbacks.Clear();
            OnChatGuildCallbacks.Clear();
            OnChatCallbacks.Clear();
            OnCommandCallbacks.Clear();
            OnEmoteCallbacks.Clear();
            OnTextEmoteCallbacks.Clear();
            OnSpellCastCallbacks.Clear();
            OnLoginCallbacks.Clear();
            OnLogoutCallbacks.Clear();
            OnCreateCallbacks.Clear();
            OnCreateEarlyCallbacks.Clear();
            OnDeleteCallbacks.Clear();
            OnFailedDeleteCallbacks.Clear();
            OnSaveCallbacks.Clear();
            OnBindToInstanceCallbacks.Clear();
            OnUpdateZoneCallbacks.Clear();
            OnMapChangedCallbacks.Clear();
            OnMovieCompleteCallbacks.Clear();
            OnPlayerRepopCallbacks.Clear();
            OnUpdateMaxHealthCallbacks.Clear();
            OnUpdateMaxPowerCallbacks.Clear();
            OnUpdateResistanceCallbacks.Clear();
            OnUpdateArmorCallbacks.Clear();
            OnUpdateAttackPowerCallbacks.Clear();
            OnUpdateRangedAttackPowerCallbacks.Clear();
            OnUpdateShieldBlockCallbacks.Clear();
            OnUpdateBlockPercentageCallbacks.Clear();
            OnUpdateCritCallbacks.Clear();
            OnUpdateParryPercentageCallbacks.Clear();
            OnUpdateDodgePercentageCallbacks.Clear();
            OnUpdateSpellCritCallbacks.Clear();
            OnUpdateArmorPenetrationCallbacks.Clear();
            OnUpdateMeleeHitChancesCallbacks.Clear();
            OnUpdateRangedHitChancesCallbacks.Clear();
            OnUpdateSpellHitChancesCallbacks.Clear();
            OnUpdateExpertiseCallbacks.Clear();
            OnUpdateManaRegenCallbacks.Clear();
            OnUpdateRuneRegenCallbacks.Clear();
            OnReputationPriceDiscountCallbacks.Clear();
            OnCalcTalentPointsCallbacks.Clear();
            OnCalcStaminaHealthBonusCallbacks.Clear();
            OnCalcIntellectManaBonusCallbacks.Clear();
            OnCalcGreyLevelCallbacks.Clear();
            OnCalcZeroDiffCallbacks.Clear();
            OnCalcGroupGainCallbacks.Clear();
            OnCalcSkillGainChanceCallbacks.Clear();
            OnGlyphInitForLevelCallbacks.Clear();
            OnSendMailCallbacks.Clear();
            OnQuestObjectiveProgressCallbacks.Clear();
            OnQuestStatusChangeCallbacks.Clear();
            OnLearnTalentCallbacks.Clear();
            OnTradeCompletedCallbacks.Clear();
            OnGossipSelectCallbacks.Clear();
            OnGossipSelectCodeCallbacks.Clear();
            OnGenerateItemLootCallbacks.Clear();
            OnLootCorpseCallbacks.Clear();
        }
    } Player;

    struct GuildEvents
    {
        GuildEvents* operator->() { return this; }

        TS_EVENT(OnAddMember, TSGuild, TSPlayer, TSMutableNumber<std::uint8_t>)
        TS_EVENT(OnRemoveMember, TSGuild, TSPlayer, bool, bool)
        TS_EVENT(OnMOTDChanged, TSGuild, std::string const&)
        TS_EVENT(OnInfoChanged, TSGuild, std::string const&)
        TS_EVENT(OnCreate, TSGuild, TSPlayer, std::string const&)
        TS_EVENT(OnDisband, TSGuild)
        TS_EVENT(OnMemberWitdrawMoney, TSGuild, TSPlayer, TSMutableNumber<std::uint32_t>, bool)
        TS_EVENT(OnMemberDepositMoney, TSGuild, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnEvent, TSGuild, TSNumber<std::uint8_t>, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint8_t>)
        TS_EVENT(OnBankEvent, TSGuild, TSNumber<std::uint8_t>, TSNumber<std::uint8_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, TSNumber<std::uint16_t>,
            TSNumber<std::uint8_t>)

        void Clear()
        {
            OnAddMemberCallbacks.Clear();
            OnRemoveMemberCallbacks.Clear();
            OnMOTDChangedCallbacks.Clear();
            OnInfoChangedCallbacks.Clear();
            OnCreateCallbacks.Clear();
            OnDisbandCallbacks.Clear();
            OnMemberWitdrawMoneyCallbacks.Clear();
            OnMemberDepositMoneyCallbacks.Clear();
            OnEventCallbacks.Clear();
            OnBankEventCallbacks.Clear();
        }
    } Guild;

    struct GroupEvents
    {
        GroupEvents* operator->() { return this; }

        TS_EVENT(OnAddMember, TSGroup, TSNumber<std::uint64_t>)
        TS_EVENT(OnInviteMember, TSGroup, TSNumber<std::uint64_t>)
        TS_EVENT(OnRemoveMember, TSGroup, TSNumber<std::uint64_t>, TSNumber<std::uint32_t>,
            TSNumber<std::uint64_t>, std::string const&)
        TS_EVENT(OnChangeLeader, TSGroup, TSNumber<std::uint64_t>, TSNumber<std::uint64_t>)
        TS_EVENT(OnDisband, TSGroup)

        void Clear()
        {
            OnAddMemberCallbacks.Clear();
            OnInviteMemberCallbacks.Clear();
            OnRemoveMemberCallbacks.Clear();
            OnChangeLeaderCallbacks.Clear();
            OnDisbandCallbacks.Clear();
        }
    } Group;

    struct UnitEvents
    {
        UnitEvents* operator->() { return this; }

        TS_EVENT(OnEnterCombat, TSUnit)
        TS_EVENT(OnExitCombat, TSUnit)
        TS_EVENT(OnEnterCombatWith, TSUnit, TSUnit)
        TS_EVENT(OnDeath, TSUnit, TSUnit)
        TS_EVENT(OnDeathEarly, TSUnit, TSUnit)
        TS_EVENT(OnCalcHeal, TSUnit, TSUnit, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnCalcMissChance, TSUnit, TSMutableNumber<float>)
        TS_EVENT(OnMeleeDamageEarly, TSMeleeDamageInfo, TSMutableNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnMeleeDamageLate, TSMeleeDamageInfo, TSMutableNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_EVENT(OnCalcMeleeCrit, TSUnit, TSUnit, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnCalcMeleeOutcome, TSUnit, TSUnit, TSMutableNumber<float>, TSMutableNumber<float>,
            TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_EVENT(OnCalcThreatEarly, TSUnit, TSUnit, TSMutableNumber<float>, TSSpellInfo, bool)
        TS_EVENT(OnCalcThreatLate, TSUnit, TSUnit, TSMutableNumber<float>, TSSpellInfo, bool)
        TS_EVENT(OnCalcScaleThreat, TSUnit, TSUnit, TSMutableNumber<float>, bool)
        TS_EVENT(OnExitCombatWith, TSUnit, TSUnit)
        TS_EVENT(OnSetTarget, TSUnit, TSNumber<std::uint64_t>, TSNumber<std::uint64_t>)
        TS_EVENT(OnLiquidStatusChanged, TSUnit, TSMutableNumber<std::uint32_t>)
        TS_EVENT(OnOutdoorsChanged, TSUnit, TSMutable<bool, bool>)

        void Clear()
        {
            OnEnterCombatCallbacks.Clear();
            OnExitCombatCallbacks.Clear();
            OnEnterCombatWithCallbacks.Clear();
            OnDeathCallbacks.Clear();
            OnDeathEarlyCallbacks.Clear();
            OnCalcHealCallbacks.Clear();
            OnCalcMissChanceCallbacks.Clear();
            OnMeleeDamageEarlyCallbacks.Clear();
            OnMeleeDamageLateCallbacks.Clear();
            OnCalcMeleeCritCallbacks.Clear();
            OnCalcMeleeOutcomeCallbacks.Clear();
            OnCalcThreatEarlyCallbacks.Clear();
            OnCalcThreatLateCallbacks.Clear();
            OnCalcScaleThreatCallbacks.Clear();
            OnExitCombatWithCallbacks.Clear();
            OnSetTargetCallbacks.Clear();
            OnLiquidStatusChangedCallbacks.Clear();
            OnOutdoorsChangedCallbacks.Clear();
        }
    } Unit;

    struct CreatureEvents
    {
        CreatureEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSCreature)>;
        TSMappedEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&, std::uint32_t)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, UINT32_MAX);
        }
        void OnReload(std::uint32_t id, OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(id, callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, id);
        }

        TS_ID_EVENT(OnGossipHello, TSCreature, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGossipSelect, TSCreature, TSPlayer, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGossipSelectCode, TSCreature, TSPlayer, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, std::string const&, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCreate, TSCreature, TSMutable<bool, bool>)
        TS_ID_EVENT(OnDeath, TSCreature, TSUnit)
        TS_ID_EVENT(OnDeathEarly, TSCreature, TSUnit)
        TS_ID_EVENT(OnJustEnteredCombat, TSCreature, TSUnit)
        TS_ID_EVENT(OnJustEngagedWith, TSCreature, TSUnit)
        TS_ID_EVENT(OnKilledUnit, TSCreature, TSUnit)
        TS_ID_EVENT(OnSummoned, TSCreature, TSCreature)
        TS_ID_EVENT(OnIsSummoned, TSCreature, TSWorldObject)
        TS_ID_EVENT(OnSummonDespawn, TSCreature, TSCreature)
        TS_ID_EVENT(OnDespawn, TSCreature, TSWorldObject)
        TS_ID_EVENT(OnSummonDies, TSCreature, TSCreature, TSUnit)
        TS_ID_EVENT(OnReceiveEmote, TSCreature, TSPlayer, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnCorpseRemoved, TSCreature, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnPassengerBoarded, TSCreature, TSUnit, TSNumber<std::int8_t>, bool)
        TS_ID_EVENT(OnCharmed, TSCreature, bool)
        TS_ID_EVENT(OnReachedHome, TSCreature)
        TS_ID_EVENT(OnOwnerAttacked, TSCreature, TSUnit)
        TS_ID_EVENT(OnOwnerAttacks, TSCreature, TSUnit)
        TS_ID_EVENT(OnWaypointStarted, TSCreature, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnWaypointReached, TSCreature, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnWaypointPathEnded, TSCreature, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnUpdateAI, TSCreature, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnMoveInLOS, TSCreature, TSUnit)
        TS_ID_EVENT(OnHitBySpell, TSCreature, TSWorldObject, TSSpellInfo)
        TS_ID_EVENT(OnSpellHitTarget, TSCreature, TSWorldObject, TSSpellInfo)
        TS_ID_EVENT(OnSpellCastFinished, TSCreature, TSSpellInfo, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnJustAppeared, TSCreature)
        TS_ID_EVENT(OnSpellClick, TSCreature, TSUnit, bool)
        TS_ID_EVENT(OnGenerateLoot, TSCreature, TSPlayer)
        TS_ID_EVENT(OnRemove, TSCreature)
        TS_ID_EVENT(OnQuestAccept, TSCreature, TSPlayer, TSQuest)
        TS_ID_EVENT(OnQuestReward, TSCreature, TSPlayer, TSQuest, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnCalcGain, TSCreature, TSMutableNumber<std::uint32_t>, TSPlayer)
        TS_ID_EVENT(OnCalcBaseGain, TSCreature, TSMutableNumber<std::uint32_t>, TSPlayer)
        TS_ID_EVENT(OnCalcColorCode, TSCreature, TSMutableNumber<std::uint8_t>, TSPlayer,
            TSNumber<std::uint8_t>, TSNumber<std::uint8_t>)
        TS_ID_EVENT(OnCanGeneratePickPocketLoot, TSCreature, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGeneratePickPocketLoot, TSCreature, TSPlayer, TSLoot)
        TS_ID_EVENT(OnGenerateSkinningLoot, TSCreature, TSPlayer, TSLoot)
        TS_ID_EVENT(OnSendVendorItem, TSCreature, TSItemTemplate, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnUpdateResistance, TSCreature, TSMutableNumber<float>, bool,
            TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnUpdateArmor, TSCreature, TSMutableNumber<float>, bool)
        TS_ID_EVENT(OnUpdateMaxHealth, TSCreature, TSMutableNumber<float>, bool)
        TS_ID_EVENT(OnUpdateMaxPower, TSCreature, TSMutableNumber<float>, bool, TSNumber<std::int8_t>)
        TS_ID_EVENT(OnUpdateAttackPowerDamage, TSCreature, TSMutableNumber<float>, TSMutableNumber<float>,
            TSMutableNumber<float>, bool, bool)
        TS_ID_EVENT(OnUpdateDamagePhysical, TSCreature, TSMutableNumber<float>, TSMutableNumber<float>,
            bool, TSNumber<std::uint8_t>)
        TS_ID_EVENT(OnUpdateLvlDepMaxHealth, TSCreature, TSMutableNumber<std::uint32_t>, TSNumber<float>,
            TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnUpdateLvlDepMaxMana, TSCreature, TSMutableNumber<std::uint32_t>, TSNumber<float>)
        TS_ID_EVENT(OnUpdateLvlDepBaseDamage, TSCreature, TSMutableNumber<float>, TSMutableNumber<float>,
            TSNumber<float>)
        TS_ID_EVENT(OnUpdateLvlDepArmor, TSCreature, TSMutableNumber<float>, TSNumber<float>)
        TS_ID_EVENT(OnUpdateLvlDepAttackPower, TSCreature, TSMutableNumber<std::uint32_t>,
            TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnMovementInform, TSCreature, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)

        void Clear()
        {
            OnReloadCallbacks.Clear();
            OnGossipHelloCallbacks.Clear();
            OnGossipSelectCallbacks.Clear();
            OnGossipSelectCodeCallbacks.Clear();
            OnCreateCallbacks.Clear();
            OnDeathCallbacks.Clear();
            OnDeathEarlyCallbacks.Clear();
            OnJustEnteredCombatCallbacks.Clear();
            OnJustEngagedWithCallbacks.Clear();
            OnKilledUnitCallbacks.Clear();
            OnSummonedCallbacks.Clear();
            OnIsSummonedCallbacks.Clear();
            OnSummonDespawnCallbacks.Clear();
            OnDespawnCallbacks.Clear();
            OnSummonDiesCallbacks.Clear();
            OnReceiveEmoteCallbacks.Clear();
            OnCorpseRemovedCallbacks.Clear();
            OnPassengerBoardedCallbacks.Clear();
            OnCharmedCallbacks.Clear();
            OnReachedHomeCallbacks.Clear();
            OnOwnerAttackedCallbacks.Clear();
            OnOwnerAttacksCallbacks.Clear();
            OnWaypointStartedCallbacks.Clear();
            OnWaypointReachedCallbacks.Clear();
            OnWaypointPathEndedCallbacks.Clear();
            OnUpdateAICallbacks.Clear();
            OnMoveInLOSCallbacks.Clear();
            OnHitBySpellCallbacks.Clear();
            OnSpellHitTargetCallbacks.Clear();
            OnSpellCastFinishedCallbacks.Clear();
            OnJustAppearedCallbacks.Clear();
            OnSpellClickCallbacks.Clear();
            OnGenerateLootCallbacks.Clear();
            OnRemoveCallbacks.Clear();
            OnQuestAcceptCallbacks.Clear();
            OnQuestRewardCallbacks.Clear();
            OnCalcGainCallbacks.Clear();
            OnCalcBaseGainCallbacks.Clear();
            OnCalcColorCodeCallbacks.Clear();
            OnCanGeneratePickPocketLootCallbacks.Clear();
            OnGeneratePickPocketLootCallbacks.Clear();
            OnGenerateSkinningLootCallbacks.Clear();
            OnSendVendorItemCallbacks.Clear();
            OnUpdateResistanceCallbacks.Clear();
            OnUpdateArmorCallbacks.Clear();
            OnUpdateMaxHealthCallbacks.Clear();
            OnUpdateMaxPowerCallbacks.Clear();
            OnUpdateAttackPowerDamageCallbacks.Clear();
            OnUpdateDamagePhysicalCallbacks.Clear();
            OnUpdateLvlDepMaxHealthCallbacks.Clear();
            OnUpdateLvlDepMaxManaCallbacks.Clear();
            OnUpdateLvlDepBaseDamageCallbacks.Clear();
            OnUpdateLvlDepArmorCallbacks.Clear();
            OnUpdateLvlDepAttackPowerCallbacks.Clear();
            OnMovementInformCallbacks.Clear();
        }
    } Creature;

    struct GameObjectEvents
    {
        GameObjectEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSGameObject)>;
        TSMappedEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&, std::uint32_t)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, UINT32_MAX);
        }
        void OnReload(std::uint32_t id, OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(id, callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, id);
        }

        TS_ID_EVENT(OnUpdate, TSGameObject, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnDialogStatus, TSGameObject, TSPlayer)
        TS_ID_EVENT(OnDestroyed, TSGameObject, TSWorldObject)
        TS_ID_EVENT(OnDamaged, TSGameObject, TSWorldObject)
        TS_ID_EVENT(OnLootStateChanged, TSGameObject, TSNumber<std::uint32_t>, TSUnit)
        TS_ID_EVENT(OnGOStateChanged, TSGameObject, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnGossipHello, TSGameObject, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGossipSelect, TSGameObject, TSPlayer, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGossipSelectCode, TSGameObject, TSPlayer, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, std::string const&, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCreate, TSGameObject, TSMutable<bool, bool>)
        TS_ID_EVENT(OnRemove, TSGameObject)
        TS_ID_EVENT(OnUse, TSGameObject, TSUnit, TSMutable<bool, bool>)
        TS_ID_EVENT(OnQuestAccept, TSGameObject, TSPlayer, TSQuest)
        TS_ID_EVENT(OnQuestReward, TSGameObject, TSPlayer, TSQuest, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnGenerateLoot, TSGameObject, TSPlayer)
        TS_ID_EVENT(OnGenerateFishLoot, TSGameObject, TSPlayer, TSLoot, bool)

        void Clear()
        {
            OnReloadCallbacks.Clear();
            OnUpdateCallbacks.Clear();
            OnDialogStatusCallbacks.Clear();
            OnDestroyedCallbacks.Clear();
            OnDamagedCallbacks.Clear();
            OnLootStateChangedCallbacks.Clear();
            OnGOStateChangedCallbacks.Clear();
            OnGossipHelloCallbacks.Clear();
            OnGossipSelectCallbacks.Clear();
            OnGossipSelectCodeCallbacks.Clear();
            OnCreateCallbacks.Clear();
            OnRemoveCallbacks.Clear();
            OnUseCallbacks.Clear();
            OnQuestAcceptCallbacks.Clear();
            OnQuestRewardCallbacks.Clear();
            OnGenerateLootCallbacks.Clear();
            OnGenerateFishLootCallbacks.Clear();
        }
    } GameObject;

    struct MapEvents
    {
        MapEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSMap)>;
        TSMappedEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&, std::uint32_t)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, UINT32_MAX);
        }
        void OnReload(std::uint32_t id, OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(id, callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, id);
        }

        TS_ID_EVENT(OnCreate, TSMap)
        TS_ID_EVENT(OnUpdate, TSMap, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnUpdateDelayed, TSMap, TSNumber<std::uint32_t>, TSMainThreadContext)
        TS_ID_EVENT(OnPlayerEnter, TSMap, TSPlayer)
        TS_ID_EVENT(OnPlayerLeave, TSMap, TSPlayer)
        TS_ID_EVENT(OnCreatureCreate, TSMap, TSCreature, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCreatureRemove, TSMap, TSCreature)
        TS_ID_EVENT(OnGameObjectCreate, TSMap, TSGameObject, TSMutable<bool, bool>)
        TS_ID_EVENT(OnGameObjectRemove, TSMap, TSGameObject)
        TS_ID_EVENT(OnWeatherUpdate, TSMap, TSWeather)
        TS_ID_EVENT(OnWeatherChange, TSMap, TSWeather)
        TS_ID_EVENT(OnCheckEncounter, TSMap, TSPlayer)

        void Clear()
        {
            OnReloadCallbacks.Clear();
            OnCreateCallbacks.Clear();
            OnUpdateCallbacks.Clear();
            OnUpdateDelayedCallbacks.Clear();
            OnPlayerEnterCallbacks.Clear();
            OnPlayerLeaveCallbacks.Clear();
            OnCreatureCreateCallbacks.Clear();
            OnCreatureRemoveCallbacks.Clear();
            OnGameObjectCreateCallbacks.Clear();
            OnGameObjectRemoveCallbacks.Clear();
            OnWeatherUpdateCallbacks.Clear();
            OnWeatherChangeCallbacks.Clear();
            OnCheckEncounterCallbacks.Clear();
        }
    } Map;

    struct InstanceEvents
    {
        InstanceEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSInstance)>;
        TSMappedEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&, std::uint32_t)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, UINT32_MAX);
        }
        void OnReload(std::uint32_t id, OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(id, callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, id);
        }

        TS_ID_EVENT(OnCreate, TSInstance)
        TS_ID_EVENT(OnLoad, TSInstance, bool)
        TS_ID_EVENT(OnSave, TSInstance)
        TS_ID_EVENT(OnUpdate, TSInstance, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnPlayerEnter, TSInstance, TSPlayer)
        TS_ID_EVENT(OnPlayerLeave, TSInstance, TSPlayer)
        TS_ID_EVENT(OnBossStateChange, TSInstance, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnFillInitialWorldStates, TSInstance, TSWorldStatePacket)
        TS_ID_EVENT(OnCanKillBoss, TSInstance, TSNumber<std::uint32_t>, TSPlayer,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnLoadBossBoundaries, TSInstance)
        TS_ID_EVENT(OnLoadMinionData, TSInstance)
        TS_ID_EVENT(OnLoadDoorData, TSInstance)
        TS_ID_EVENT(OnLoadObjectData, TSInstance)
        TS_ID_EVENT(OnSetBossNumber, TSInstance, TSMutableNumber<std::uint32_t>)

        void Clear()
        {
            OnCreateCallbacks.Clear();
            OnReloadCallbacks.Clear();
            OnLoadCallbacks.Clear();
            OnSaveCallbacks.Clear();
            OnUpdateCallbacks.Clear();
            OnPlayerEnterCallbacks.Clear();
            OnPlayerLeaveCallbacks.Clear();
            OnBossStateChangeCallbacks.Clear();
            OnFillInitialWorldStatesCallbacks.Clear();
            OnCanKillBossCallbacks.Clear();
            OnLoadBossBoundariesCallbacks.Clear();
            OnLoadMinionDataCallbacks.Clear();
            OnLoadDoorDataCallbacks.Clear();
            OnLoadObjectDataCallbacks.Clear();
            OnSetBossNumberCallbacks.Clear();
        }
    } Instance;

    struct ItemEvents
    {
        ItemEvents* operator->() { return this; }

        TS_ID_EVENT(OnUse, TSItem, TSPlayer, void*, TSMutable<bool, bool>)
        TS_ID_EVENT(OnExpire, TSItemTemplate, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnRemove, TSItem, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCastSpell, TSItem, TSPlayer, TSUnit, TSSpellInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnQuestAccept, TSItem, TSPlayer, TSQuest)
        TS_ID_EVENT(OnGossipSelect, TSItem, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnGossipSelectCode, TSItem, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            std::string const&, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCanEquip, TSItem, TSPlayer, TSNumber<std::uint8_t>, bool, bool,
            TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnUnequip, TSItem, TSPlayer, bool, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnEquip, TSItem, TSPlayer, TSNumber<std::uint8_t>, bool)
        TS_ID_EVENT(OnCanChangeEquipState, TSItemTemplate, TSMutable<bool, bool>)
        TS_ID_EVENT(OnBank, TSItem, TSPlayer, TSNumber<std::uint8_t>, TSNumber<std::uint8_t>, bool,
            TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnCanUse, TSItem, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnLFGRollEarly, TSItemTemplate, TSWorldObject, TSPlayer,
            TSMutableNumber<std::int32_t>)
        TS_ID_EVENT(OnDestroyEarly, TSItem, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCanUseType, TSItemTemplate, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnCalculateFeralAttackPower, TSItemTemplate, TSNumber<std::int32_t>,
            TSMutableNumber<std::int32_t>)
        TS_ID_EVENT(OnTakenAsLoot, TSItem, TSLootItem, TSLoot, TSPlayer)

        void Clear()
        {
            OnUseCallbacks.Clear();
            OnExpireCallbacks.Clear();
            OnRemoveCallbacks.Clear();
            OnCastSpellCallbacks.Clear();
            OnQuestAcceptCallbacks.Clear();
            OnGossipSelectCallbacks.Clear();
            OnGossipSelectCodeCallbacks.Clear();
            OnCanEquipCallbacks.Clear();
            OnUnequipCallbacks.Clear();
            OnEquipCallbacks.Clear();
            OnCanChangeEquipStateCallbacks.Clear();
            OnBankCallbacks.Clear();
            OnCanUseCallbacks.Clear();
            OnLFGRollEarlyCallbacks.Clear();
            OnDestroyEarlyCallbacks.Clear();
            OnCanUseTypeCallbacks.Clear();
            OnCalculateFeralAttackPowerCallbacks.Clear();
            OnTakenAsLootCallbacks.Clear();
        }
    } Item;

    struct QuestEvents
    {
        QuestEvents* operator->() { return this; }

        TS_ID_EVENT(OnRewardXP, TSQuest, TSPlayer, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnAccept, TSQuest, TSPlayer, TSObject)
        TS_ID_EVENT(OnReward, TSQuest, TSPlayer, TSObject, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnObjectiveProgress, TSQuest, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint16_t>)
        TS_ID_EVENT(OnStatusChanged, TSQuest, TSPlayer)
        TS_ID_EVENT(OnSpellFinish, TSQuest, TSPlayer, TSSpell)

        void Clear()
        {
            OnRewardXPCallbacks.Clear();
            OnAcceptCallbacks.Clear();
            OnRewardCallbacks.Clear();
            OnObjectiveProgressCallbacks.Clear();
            OnStatusChangedCallbacks.Clear();
            OnSpellFinishCallbacks.Clear();
        }
    } Quest;

    struct AreaTriggerEvents
    {
        AreaTriggerEvents* operator->() { return this; }

        TS_ID_EVENT(OnTrigger, TSAreaTriggerEntry, TSPlayer, TSMutable<bool, bool>)

        void Clear() { OnTriggerCallbacks.Clear(); }
    } AreaTrigger;

    struct WorldPacketEvents
    {
        WorldPacketEvents* operator->() { return this; }

        TS_ID_EVENT(OnReceive, TSNumber<std::uint32_t>, TSWorldPacket, TSPlayer)
        TS_ID_EVENT(OnSend, TSWorldPacket, TSPlayer)

        void Clear()
        {
            OnReceiveCallbacks.Clear();
            OnSendCallbacks.Clear();
        }
    } WorldPacket;

    struct SpellEvents
    {
        SpellEvents* operator->() { return this; }

        TS_ID_EVENT(OnCast, TSSpell)
        TS_ID_EVENT(OnCheckCast, TSSpell, TSMutableNumber<std::uint8_t>)
        TS_ID_EVENT(OnCancel, TSSpell, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnHit, TSSpell)
        TS_ID_EVENT(OnAfterCast, TSSpell, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterHit, TSSpell, TSMutable<bool, bool>)
        TS_ID_EVENT(OnBeforeCast, TSSpell, TSMutable<bool, bool>)
        TS_ID_EVENT(OnBeforeHit, TSSpell, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectApplyGlyph, TSSpell, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCalcCrit, TSSpell, TSMutableNumber<float>)
        TS_ID_EVENT(OnTrainerSend, TSSpellInfo, TSNumber<std::uint32_t>, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCalcMiss, TSSpell, TSUnit, TSMutableNumber<std::uint32_t>,
            TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnCalcAuraCrit, TSAuraEffect, TSMutableNumber<float>)
        TS_ID_EVENT(OnPeriodicDamage, TSAuraEffect, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnCalcSpellPowerLevelPenalty, TSSpellInfo, TSMutableNumber<float>, TSUnit)
        TS_ID_EVENT(OnCalcReflect, TSSpellInfo, TSMutableNumber<std::int32_t>, TSWorldObject, TSUnit)
        TS_ID_EVENT(OnCalcHit, TSSpellInfo, TSMutableNumber<std::int32_t>, TSWorldObject, TSUnit)
        TS_ID_EVENT(OnCalcResist, TSSpellInfo, TSMutableNumber<std::int32_t>, TSWorldObject, TSUnit)
        TS_ID_EVENT(OnCalcMeleeMiss, TSSpellInfo, TSMutableNumber<float>, TSUnit, TSUnit,
            TSNumber<std::uint8_t>, TSNumber<std::int32_t>)
        TS_ID_EVENT(OnEffect, TSSpell, TSMutable<bool, bool>, TSSpellEffectInfo,
            TSNumber<std::uint32_t>, TSUnit, TSItem, TSGameObject, TSCorpse)
        TS_ID_EVENT(OnDamageEarly, TSSpell, TSMutableNumber<std::int32_t>, TSSpellDamageInfo,
            TSNumber<std::uint32_t>, bool, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnDamageLate, TSSpell, TSMutableNumber<std::uint32_t>, TSSpellDamageInfo,
            TSNumber<std::uint32_t>, bool, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnSuccessfulDispel, TSSpell, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnObjectAreaTargetSelect, TSSpell, TSWorldObjectCollection,
            TSNumber<std::uint32_t>, TSSpellImplicitTargetInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnObjectTargetSelect, TSSpell, TSMutableWorldObject,
            TSNumber<std::uint32_t>, TSSpellImplicitTargetInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnDestinationTargetSelect, TSSpell, TSSpellDestination,
            TSNumber<std::uint32_t>, TSSpellImplicitTargetInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnLearn, TSSpellInfo, TSPlayer, bool, bool, bool, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnUnlearn, TSSpellInfo, TSPlayer, bool, bool)
        TS_ID_EVENT(OnUnlearnTalent, TSSpellInfo, TSPlayer, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, bool)
        TS_ID_EVENT(OnOnResistAbsorbCalculate, TSSpell, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutableNumber<std::int32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnLearnTalent, TSSpellInfo, TSPlayer, TSNumber<std::uint32_t>, TSNumber<std::uint32_t>,
            TSNumber<std::uint32_t>, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCheckAreaTarget, TSAura, TSUnit, TSMutable<bool, bool>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnDispel, TSAura, TSDispelInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterDispel, TSAura, TSDispelInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnApply, TSAuraEffect, TSAuraApplication, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnRemove, TSAuraEffect, TSAuraApplication, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterEffectApply, TSAuraEffect, TSAuraApplication, TSNumber<std::uint32_t>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterEffectRemove, TSAuraEffect, TSAuraApplication, TSNumber<std::uint32_t>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectPeriodic, TSAuraEffect, TSAuraApplication, TSMutable<bool, bool>)
        TS_ID_EVENT(OnTick, TSAuraEffect)
        TS_ID_EVENT(OnEffectCalcAmount, TSAuraEffect, TSMutableNumber<std::int32_t>, TSMutable<bool, bool>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectCalcPeriodic, TSAuraEffect, TSMutable<bool, bool>, TSMutableNumber<std::int32_t>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectCalcSpellMod, TSAuraEffect, TSSpellModifier, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectAbsorb, TSAuraEffect, TSAuraApplication, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectAfterAbsorb, TSAuraEffect, TSAuraApplication, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectManaShield, TSAuraEffect, TSAuraApplication, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectAfterManaShield, TSAuraEffect, TSAuraApplication, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectSplit, TSAuraEffect, TSAuraApplication, TSDamageInfo,
            TSMutableNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnCheckProc, TSAuraApplication, TSProcEventInfo, TSMutable<bool, bool>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnCheckEffectProc, TSAuraEffect, TSAuraApplication, TSProcEventInfo,
            TSMutable<bool, bool>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnPrepareProc, TSAuraApplication, TSProcEventInfo, TSMutable<bool, bool>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnProc, TSAuraApplication, TSProcEventInfo, TSMutable<bool, bool>,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterProc, TSAuraApplication, TSProcEventInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnEffectProc, TSAuraEffect, TSAuraApplication, TSProcEventInfo, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAfterEffectProc, TSAuraEffect, TSAuraApplication, TSProcEventInfo,
            TSMutable<bool, bool>)
        TS_ID_EVENT(OnSetDuration, TSAura, TSMutableNumber<std::int32_t>, TSMutable<bool, bool>)

        void Clear()
        {
            OnCastCallbacks.Clear();
            OnCheckCastCallbacks.Clear();
            OnCancelCallbacks.Clear();
            OnHitCallbacks.Clear();
            OnAfterCastCallbacks.Clear();
            OnAfterHitCallbacks.Clear();
            OnBeforeCastCallbacks.Clear();
            OnBeforeHitCallbacks.Clear();
            OnEffectApplyGlyphCallbacks.Clear();
            OnCalcCritCallbacks.Clear();
            OnTrainerSendCallbacks.Clear();
            OnCalcMissCallbacks.Clear();
            OnCalcAuraCritCallbacks.Clear();
            OnPeriodicDamageCallbacks.Clear();
            OnCalcSpellPowerLevelPenaltyCallbacks.Clear();
            OnCalcReflectCallbacks.Clear();
            OnCalcHitCallbacks.Clear();
            OnCalcResistCallbacks.Clear();
            OnCalcMeleeMissCallbacks.Clear();
            OnEffectCallbacks.Clear();
            OnDamageEarlyCallbacks.Clear();
            OnDamageLateCallbacks.Clear();
            OnSuccessfulDispelCallbacks.Clear();
            OnObjectAreaTargetSelectCallbacks.Clear();
            OnObjectTargetSelectCallbacks.Clear();
            OnDestinationTargetSelectCallbacks.Clear();
            OnLearnCallbacks.Clear();
            OnUnlearnCallbacks.Clear();
            OnUnlearnTalentCallbacks.Clear();
            OnOnResistAbsorbCalculateCallbacks.Clear();
            OnLearnTalentCallbacks.Clear();
            OnCheckAreaTargetCallbacks.Clear();
            OnDispelCallbacks.Clear();
            OnAfterDispelCallbacks.Clear();
            OnApplyCallbacks.Clear();
            OnRemoveCallbacks.Clear();
            OnAfterEffectApplyCallbacks.Clear();
            OnAfterEffectRemoveCallbacks.Clear();
            OnEffectPeriodicCallbacks.Clear();
            OnTickCallbacks.Clear();
            OnEffectCalcAmountCallbacks.Clear();
            OnEffectCalcPeriodicCallbacks.Clear();
            OnEffectCalcSpellModCallbacks.Clear();
            OnEffectAbsorbCallbacks.Clear();
            OnEffectAfterAbsorbCallbacks.Clear();
            OnEffectManaShieldCallbacks.Clear();
            OnEffectAfterManaShieldCallbacks.Clear();
            OnEffectSplitCallbacks.Clear();
            OnCheckProcCallbacks.Clear();
            OnCheckEffectProcCallbacks.Clear();
            OnPrepareProcCallbacks.Clear();
            OnProcCallbacks.Clear();
            OnAfterProcCallbacks.Clear();
            OnEffectProcCallbacks.Clear();
            OnAfterEffectProcCallbacks.Clear();
            OnSetDurationCallbacks.Clear();
        }
    } Spell;

    struct BattlegroundEvents
    {
        BattlegroundEvents* operator->() { return this; }

        using OnReloadCallback = std::function<void(TSBattleground)>;
        TSMappedEvent<OnReloadCallback> OnReloadCallbacks;
        std::function<void(OnReloadCallback const&, std::uint32_t)> OnReloadHandler;
        void OnReload(OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, UINT32_MAX);
        }
        void OnReload(std::uint32_t id, OnReloadCallback callback)
        {
            OnReloadCallbacks.Add(id, callback);
            if (OnReloadHandler)
                OnReloadHandler(callback, id);
        }

        TS_ID_EVENT(OnCreate, TSBattleground)
        TS_ID_EVENT(OnAddPlayer, TSBattleground, TSPlayer)
        TS_ID_EVENT(OnUpdateLate, TSBattleground, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnEndLate, TSBattleground, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnOpenDoors, TSBattleground)
        TS_ID_EVENT(OnSendScore, TSBattleground, TSBattlegroundScore, TSWorldPacket, TSMutable<bool, bool>)
        TS_ID_EVENT(OnPlayerLogin, TSBattleground, TSPlayer)
        TS_ID_EVENT(OnPlayerLogout, TSBattleground, TSPlayer)
        TS_ID_EVENT(OnKillPlayer, TSBattleground, TSPlayer, TSPlayer)
        TS_ID_EVENT(OnKillCreature, TSBattleground, TSCreature, TSPlayer)
        TS_ID_EVENT(OnRemovePlayer, TSBattleground, TSNumber<std::uint64_t>, TSPlayer,
            TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnCanCreate, TSBattleground, TSMutable<bool, bool>)
        TS_ID_EVENT(OnUpdateEarly, TSBattleground, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnEndEarly, TSBattleground, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnReset, TSBattleground)
        TS_ID_EVENT(OnUpdateScore, TSBattleground, TSPlayer, TSNumber<std::uint32_t>, bool,
            TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnCloseDoors, TSBattleground)
        TS_ID_EVENT(OnPlayerUnderMap, TSBattleground, TSPlayer, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAreaTrigger, TSBattleground, TSPlayer, TSNumber<std::uint32_t>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnAddGameObject, TSBattleground, TSNumber<std::uint32_t>,
            TSMutableNumber<std::uint32_t>, TSMutableNumber<std::uint8_t>, TSMutableNumber<float>,
            TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<float>,
            TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<float>)
        TS_ID_EVENT(OnAddCreature, TSBattleground, TSNumber<std::uint32_t>,
            TSMutableNumber<std::uint32_t>, TSMutableNumber<float>, TSMutableNumber<float>,
            TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnWeight, TSNumber<std::uint32_t>, TSMutableNumber<float>, TSNumber<std::uint32_t>)
        TS_ID_EVENT(OnSelect, TSMutableNumber<std::uint32_t>)
        TS_ID_EVENT(OnAddSpiritGuide, TSBattleground, TSNumber<std::uint32_t>,
            TSMutableNumber<std::uint32_t>, TSMutableNumber<std::uint8_t>, TSMutableNumber<float>,
            TSMutableNumber<float>, TSMutableNumber<float>, TSMutableNumber<float>)
        TS_ID_EVENT(OnClickFlag, TSBattleground, TSPlayer, TSGameObject)
        TS_ID_EVENT(OnDropFlag, TSBattleground, TSPlayer)
        TS_ID_EVENT(OnDestroyGate, TSBattleground, TSPlayer, TSGameObject)
        TS_ID_EVENT(OnGenericEvent, TSBattleground, TSWorldObject, TSNumber<std::uint32_t>, TSWorldObject)
        TS_ID_EVENT(OnAchievementCriteria, TSBattleground, TSNumber<std::uint32_t>, TSPlayer, TSUnit,
            TSNumber<std::uint32_t>, TSMutable<bool, bool>)

        void Clear()
        {
            OnCreateCallbacks.Clear();
            OnAddPlayerCallbacks.Clear();
            OnUpdateLateCallbacks.Clear();
            OnEndLateCallbacks.Clear();
            OnOpenDoorsCallbacks.Clear();
            OnSendScoreCallbacks.Clear();
            OnPlayerLoginCallbacks.Clear();
            OnPlayerLogoutCallbacks.Clear();
            OnKillPlayerCallbacks.Clear();
            OnKillCreatureCallbacks.Clear();
            OnRemovePlayerCallbacks.Clear();
            OnCanCreateCallbacks.Clear();
            OnReloadCallbacks.Clear();
            OnUpdateEarlyCallbacks.Clear();
            OnEndEarlyCallbacks.Clear();
            OnResetCallbacks.Clear();
            OnUpdateScoreCallbacks.Clear();
            OnCloseDoorsCallbacks.Clear();
            OnPlayerUnderMapCallbacks.Clear();
            OnAreaTriggerCallbacks.Clear();
            OnAddGameObjectCallbacks.Clear();
            OnAddCreatureCallbacks.Clear();
            OnWeightCallbacks.Clear();
            OnSelectCallbacks.Clear();
            OnAddSpiritGuideCallbacks.Clear();
            OnClickFlagCallbacks.Clear();
            OnDropFlagCallbacks.Clear();
            OnDestroyGateCallbacks.Clear();
            OnGenericEventCallbacks.Clear();
            OnAchievementCriteriaCallbacks.Clear();
        }
    } Battleground;

    struct GameEventEvents
    {
        GameEventEvents* operator->() { return this; }

        TS_ID_EVENT(OnStart, TSNumber<std::uint16_t>)
        TS_ID_EVENT(OnUpdateState, TSNumber<std::uint16_t>)
        TS_ID_EVENT(OnEnd, TSNumber<std::uint16_t>)

        void Clear()
        {
            OnStartCallbacks.Clear();
            OnUpdateStateCallbacks.Clear();
            OnEndCallbacks.Clear();
        }
    } GameEvent;

    struct ConditionEvents
    {
        ConditionEvents* operator->() { return this; }

        TS_ID_EVENT(OnCheck, TSCondition, TSConditionSourceInfo, TSMutable<bool, bool>)

        void Clear() { OnCheckCallbacks.Clear(); }
    } Condition;

    struct SmartActionEvents
    {
        SmartActionEvents* operator->() { return this; }

        TS_ID_EVENT(OnActivateEarly, TSSmartScriptValues, TSMutable<bool, bool>, TSMutable<bool, bool>)
        TS_ID_EVENT(OnActivateLate, TSSmartScriptValues, TSMutable<bool, bool>)

        void Clear()
        {
            OnActivateEarlyCallbacks.Clear();
            OnActivateLateCallbacks.Clear();
        }
    } SmartAction;

    struct CustomPacketEvents
    {
        using OnReceiveCallback = std::function<void(TSNumber<std::uint32_t>, TSPacketRead, TSPlayer)>;

        CustomPacketEvents* operator->() { return this; }
        void OnReceive(OnReceiveCallback callback) { _all.push_back(std::move(callback)); }
        void OnReceive(std::uint32_t id, OnReceiveCallback callback)
        {
            _byId[id].push_back(std::move(callback));
        }
        void Fire(std::uint32_t id, TSPacketRead packet, TSPlayer player)
        {
            for (OnReceiveCallback const& callback : _all)
            {
                callback(id, packet, player);
                packet.Reset();
            }
            auto found = _byId.find(id);
            if (found == _byId.end())
                return;
            for (OnReceiveCallback const& callback : found->second)
            {
                callback(id, packet, player);
                packet.Reset();
            }
        }
        void Clear()
        {
            _all.clear();
            _byId.clear();
        }

    private:
        std::vector<OnReceiveCallback> _all;
        std::unordered_map<std::uint32_t, std::vector<OnReceiveCallback>> _byId;
    } CustomPacket;

    void Clear()
    {
        Achievement.Clear();
        Auction.Clear();
        Vehicle.Clear();
        World.Clear();
        Account.Clear();
        Player.Clear();
        Guild.Clear();
        Group.Clear();
        Unit.Clear();
        Creature.Clear();
        GameObject.Clear();
        Map.Clear();
        Instance.Clear();
        Item.Clear();
        Quest.Clear();
        AreaTrigger.Clear();
        WorldPacket.Clear();
        Spell.Clear();
        Battleground.Clear();
        GameEvent.Clear();
        Condition.Clear();
        SmartAction.Clear();
        CustomPacket.Clear();
    }
};

#undef TS_EVENT
#undef TS_ID_EVENT

extern TSEvents ts_events;

#endif
