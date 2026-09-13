#ifndef MOD_TSWOW_DATABASE_H
#define MOD_TSWOW_DATABASE_H

#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

enum class TSDatabaseType : std::uint8_t { WORLD = 0, AUTH = 1, CHARACTERS = 2 };

struct FieldSpec
{
    std::string m_name;
    std::string m_typeName;
    bool m_isPrimaryKey;
    bool m_autoIncrements;
};

struct TSDatabaseApi
{
    void* (*Query)(std::uint8_t, char const*);
    void (*QueryAsync)(std::uint8_t, char const*);
    void (*DestroyResult)(void*);
    bool (*IsValid)(void const*);
    bool (*GetRow)(void*);
    std::uint64_t (*GetUInt)(void const*, std::uint32_t, std::uint8_t);
    std::int64_t (*GetInt)(void const*, std::uint32_t, std::uint8_t);
    double (*GetDouble)(void const*, std::uint32_t, bool);
    std::string (*GetString)(void const*, std::uint32_t);
    std::vector<std::uint8_t> (*GetBinary)(void const*, std::uint32_t);
    std::string (*EscapeString)(std::uint8_t, std::string const&);
    std::string (*ConnectionInfo)(std::uint8_t, std::uint8_t);
    void (*CreateDatabaseSpec)(std::uint8_t, std::string const&, std::string const&,
        std::vector<FieldSpec> const&);
};

namespace TSWoWDatabase
{
struct ResultState
{
    TSDatabaseApi const* api = nullptr;
    void* handle = nullptr;
    TSDatabaseType database = TSDatabaseType::CHARACTERS;
    std::string pendingSql;

    ~ResultState()
    {
        if (handle && api)
            api->DestroyResult(handle);
    }

    void Resolve(TSDatabaseApi const* value)
    {
        if (handle || pendingSql.empty() || !value)
            return;
        api = value;
        handle = api->Query(static_cast<std::uint8_t>(database), pendingSql.c_str());
        pendingSql.clear();
    }
};

inline TSDatabaseApi const* Api = nullptr;
inline std::vector<std::weak_ptr<ResultState>> PendingResults;

inline void Initialize(TSEvents* events)
{
    Api = events ? static_cast<TSDatabaseApi const*>(events->DatabaseApi) : nullptr;
    if (!Api)
        throw std::runtime_error("TSWoW database service is unavailable");
    for (auto const& weak : PendingResults)
        if (auto state = weak.lock())
            state->Resolve(Api);
    PendingResults.clear();
}

inline std::string Number(double value)
{
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}
}

inline void TSInitialize(TSEvents* events)
{
    TSOutfitApiStorage() = events ? static_cast<TSOutfitApi const*>(events->OutfitApi) : nullptr;
    TSWoWDatabase::Initialize(events);
}

class TSDatabaseResult
{
public:
    explicit TSDatabaseResult(std::shared_ptr<TSWoWDatabase::ResultState> state) : _state(std::move(state)) { }
    TSDatabaseResult* operator->() { return this; }

    bool IsValid() { Resolve(); return _state->api->IsValid(_state->handle); }
    bool GetRow() { Resolve(); return _state->api->GetRow(_state->handle); }
    TSNumber<std::uint8_t> GetUInt8(int index) { return GetUInt(index, 1); }
    TSNumber<std::uint16_t> GetUInt16(int index) { return GetUInt(index, 2); }
    TSNumber<std::uint32_t> GetUInt32(int index) { return GetUInt(index, 4); }
    TSNumber<std::uint64_t> GetUInt64(int index) { return GetUInt(index, 8); }
    std::uint64_t GetUInt64Raw(int index)
    {
        Resolve();
        return _state->api->GetUInt(_state->handle, static_cast<std::uint32_t>(index), 8);
    }
    TSNumber<std::int8_t> GetInt8(int index) { return GetInt(index, 1); }
    TSNumber<std::int16_t> GetInt16(int index) { return GetInt(index, 2); }
    TSNumber<std::int32_t> GetInt32(int index) { return GetInt(index, 4); }
    TSNumber<std::int64_t> GetInt64(int index) { return GetInt(index, 8); }
    std::int64_t GetInt64Raw(int index)
    {
        Resolve();
        return _state->api->GetInt(_state->handle, static_cast<std::uint32_t>(index), 8);
    }
    TSNumber<float> GetFloat(int index)
    {
        Resolve();
        return _state->api->GetDouble(_state->handle, static_cast<std::uint32_t>(index), true);
    }
    TSNumber<double> GetDouble(int index)
    {
        Resolve();
        return _state->api->GetDouble(_state->handle, static_cast<std::uint32_t>(index), false);
    }
    TSGUID GetGUIDNumber(int index) { return TSGUID(GetUInt64Raw(index)); }
    std::string GetString(int index)
    {
        Resolve();
        return _state->api->GetString(_state->handle, static_cast<std::uint32_t>(index));
    }
    TSArray<std::uint8_t> GetBinary(int index)
    {
        Resolve();
        return TSArray<std::uint8_t>(_state->api->GetBinary(_state->handle,
            static_cast<std::uint32_t>(index)));
    }

private:
    void Resolve()
    {
        _state->Resolve(TSWoWDatabase::Api);
        if (!_state->api || !_state->handle)
            throw std::runtime_error("TSWoW database query ran before runtime initialization");
    }
    double GetUInt(int index, std::uint8_t width)
    {
        Resolve();
        return static_cast<double>(_state->api->GetUInt(_state->handle, static_cast<std::uint32_t>(index), width));
    }
    double GetInt(int index, std::uint8_t width)
    {
        Resolve();
        return static_cast<double>(_state->api->GetInt(_state->handle, static_cast<std::uint32_t>(index), width));
    }
    std::shared_ptr<TSWoWDatabase::ResultState> _state;
};

inline std::shared_ptr<TSDatabaseResult> TSQuery(TSDatabaseType database, std::string const& sql)
{
    auto state = std::make_shared<TSWoWDatabase::ResultState>();
    state->database = database;
    state->pendingSql = sql;
    if (TSWoWDatabase::Api)
        state->Resolve(TSWoWDatabase::Api);
    else
        TSWoWDatabase::PendingResults.emplace_back(state);
    return std::make_shared<TSDatabaseResult>(std::move(state));
}

inline std::shared_ptr<TSDatabaseResult> QueryWorld(std::string const& sql) { return TSQuery(TSDatabaseType::WORLD, sql); }
inline std::shared_ptr<TSDatabaseResult> QueryAuth(std::string const& sql) { return TSQuery(TSDatabaseType::AUTH, sql); }
inline std::shared_ptr<TSDatabaseResult> QueryCharacters(std::string const& sql) { return TSQuery(TSDatabaseType::CHARACTERS, sql); }

inline void TSQueryAsync(TSDatabaseType database, std::string const& sql)
{
    if (!TSWoWDatabase::Api)
        throw std::runtime_error("TSWoW database service is unavailable");
    TSWoWDatabase::Api->QueryAsync(static_cast<std::uint8_t>(database), sql.c_str());
}
inline void QueryWorldAsync(std::string const& sql) { TSQueryAsync(TSDatabaseType::WORLD, sql); }
inline void QueryAuthAsync(std::string const& sql) { TSQueryAsync(TSDatabaseType::AUTH, sql); }
inline void QueryCharactersAsync(std::string const& sql) { TSQueryAsync(TSDatabaseType::CHARACTERS, sql); }

class TSPreparedStatementBase
{
public:
    TSPreparedStatementBase(TSDatabaseType database, std::string sql) : _database(database), _sql(std::move(sql)) { }
    TSPreparedStatementBase* operator->() { return this; }
    TSPreparedStatementBase* SetNull(std::uint8_t index) { return Set(index, "NULL"); }
    TSPreparedStatementBase* SetUInt8(std::uint8_t index, std::uint8_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetUInt16(std::uint8_t index, std::uint16_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetUInt32(std::uint8_t index, std::uint32_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetUInt64(std::uint8_t index, std::uint64_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetInt8(std::uint8_t index, std::int8_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetInt16(std::uint8_t index, std::int16_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetInt32(std::uint8_t index, std::int32_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetInt64(std::uint8_t index, std::int64_t value) { return Set(index, std::to_string(value)); }
    TSPreparedStatementBase* SetGUIDNumber(std::uint8_t index, TSGUID value) { return SetUInt64(index, value.GetRawValue()); }
    TSPreparedStatementBase* SetFloat(std::uint8_t index, float value) { return Set(index, TSWoWDatabase::Number(value)); }
    TSPreparedStatementBase* SetDouble(std::uint8_t index, double value) { return Set(index, TSWoWDatabase::Number(value)); }
    TSPreparedStatementBase* SetString(std::uint8_t index, std::string const& value)
    {
        if (!TSWoWDatabase::Api)
            throw std::runtime_error("TSWoW database service is unavailable");
        return Set(index, "'" + TSWoWDatabase::Api->EscapeString(static_cast<std::uint8_t>(_database), value) + "'");
    }
    TSPreparedStatementBase* SetBinary(std::uint8_t index, TSArray<std::uint8_t> const& value)
    {
        static char const digits[] = "0123456789ABCDEF";
        std::string literal = "X'";
        for (std::size_t i = 0; i < value.get_length(); ++i)
        {
            std::uint8_t byte = value[static_cast<int>(i)];
            literal += digits[byte >> 4];
            literal += digits[byte & 15];
        }
        return Set(index, literal + "'");
    }
    std::shared_ptr<TSDatabaseResult> Send() { return TSQuery(_database, BuildSql()); }
    void SendAsync() { TSQueryAsync(_database, BuildSql()); }

private:
    TSPreparedStatementBase* Set(std::uint8_t index, std::string value)
    {
        if (_parameters.size() <= index)
            _parameters.resize(index + 1);
        _parameters[index] = std::move(value);
        return this;
    }
    std::string BuildSql() const
    {
        std::string result;
        result.reserve(_sql.size() + _parameters.size() * 8);
        std::size_t parameter = 0;
        char quote = 0;
        bool escaped = false;
        for (char current : _sql)
        {
            if (quote)
            {
                result += current;
                if (escaped) escaped = false;
                else if (current == '\\') escaped = true;
                else if (current == quote) quote = 0;
            }
            else if (current == '\'' || current == '"' || current == '`')
            {
                quote = current;
                result += current;
            }
            else if (current == '?')
            {
                if (parameter >= _parameters.size() || _parameters[parameter].empty())
                    throw std::runtime_error("TSWoW prepared statement has an unset parameter");
                result += _parameters[parameter++];
            }
            else result += current;
        }
        if (parameter != _parameters.size())
            throw std::runtime_error("TSWoW prepared statement has too many parameters");
        return result;
    }
    TSDatabaseType _database;
    std::string _sql;
    std::vector<std::string> _parameters;
};

template <TSDatabaseType Database>
class TSPreparedStatement
{
public:
    explicit TSPreparedStatement(std::string sql = {}) : _sql(std::move(sql)) { }
    TSPreparedStatement* operator->() { return this; }
    TSPreparedStatementBase Create() const { return TSPreparedStatementBase(Database, _sql); }
private:
    std::string _sql;
};

using TSPreparedStatementWorld = TSPreparedStatement<TSDatabaseType::WORLD>;
using TSPreparedStatementAuth = TSPreparedStatement<TSDatabaseType::AUTH>;
using TSPreparedStatementCharacters = TSPreparedStatement<TSDatabaseType::CHARACTERS>;
inline TSPreparedStatementWorld PrepareWorldQuery(std::string const& sql) { return TSPreparedStatementWorld(sql); }
inline TSPreparedStatementAuth PrepareAuthQuery(std::string const& sql) { return TSPreparedStatementAuth(sql); }
inline TSPreparedStatementCharacters PrepareCharactersQuery(std::string const& sql) { return TSPreparedStatementCharacters(sql); }

class TSDatabaseConnectionInfo
{
public:
    explicit TSDatabaseConnectionInfo(TSDatabaseType database) : _database(database) { }
    TSDatabaseConnectionInfo* operator->() { return this; }
    std::string User() { return Get(0); }
    std::string Password() { return Get(1); }
    std::string Database() { return Get(2); }
    std::string Host() { return Get(3); }
    std::string PortOrSocket() { return Get(4); }
    std::string SSL() { return Get(5); }
private:
    std::string Get(std::uint8_t field) const
    {
        if (!TSWoWDatabase::Api)
            throw std::runtime_error("TSWoW database service is unavailable");
        return TSWoWDatabase::Api->ConnectionInfo(static_cast<std::uint8_t>(_database), field);
    }
    TSDatabaseType _database;
};

inline std::shared_ptr<TSDatabaseConnectionInfo> WorldDatabaseInfo() { return std::make_shared<TSDatabaseConnectionInfo>(TSDatabaseType::WORLD); }
inline std::shared_ptr<TSDatabaseConnectionInfo> AuthDatabaseInfo() { return std::make_shared<TSDatabaseConnectionInfo>(TSDatabaseType::AUTH); }
inline std::shared_ptr<TSDatabaseConnectionInfo> CharactersDatabaseInfo() { return std::make_shared<TSDatabaseConnectionInfo>(TSDatabaseType::CHARACTERS); }

inline void CreateDatabaseSpec(std::uint32_t type, std::string const& database, std::string const& table,
    std::vector<FieldSpec> const& fields)
{
    if (!TSWoWDatabase::Api)
        throw std::runtime_error("TSWoW database service is unavailable");
    TSWoWDatabase::Api->CreateDatabaseSpec(static_cast<std::uint8_t>(type), database, table, fields);
}

#define LoadRows(cls, query) cls::Load(query)

#endif
