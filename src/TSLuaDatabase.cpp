#include "TSLuaDatabase.h"

#include <sol/sol.hpp>

#include <stdexcept>

void BindLuaDatabaseCompatibility(sol::state_view& lua, sol::environment& environment)
{
    sol::protected_function_result result = lua.safe_script(R"lua(
local function wrapResult(raw)
    local started = false
    local result = {}
    function result:GetRow()
        if raw == nil then return false end
        if not started then started = true; return true end
        return raw:NextRow()
    end
    function result:IsValid() return raw ~= nil end
    function result:GetUInt8(index) return raw:GetUInt8(index) end
    function result:GetUInt16(index) return raw:GetUInt16(index) end
    function result:GetUInt32(index) return raw:GetUInt32(index) end
    function result:GetUInt64(index) return raw:GetUInt64(index) end
    function result:GetInt8(index) return raw:GetInt8(index) end
    function result:GetInt16(index) return raw:GetInt16(index) end
    function result:GetInt32(index) return raw:GetInt32(index) end
    function result:GetInt64(index) return raw:GetInt64(index) end
    function result:GetFloat(index) return raw:GetFloat(index) end
    function result:GetDouble(index) return raw:GetDouble(index) end
    function result:GetString(index) return raw:GetString(index) end
    function result:GetBinary()
        error("TSWoW binary database fields require an ALE binary-result binding")
    end
    function result:GetGUIDNumber(index) return CreateGUID(0, raw:GetUInt64(index)) end
    return result
end

function QueryWorld(sql) return wrapResult(WorldDBQuery(sql)) end
function QueryCharacters(sql) return wrapResult(CharDBQuery(sql)) end
function QueryAuth(sql) return wrapResult(AuthDBQuery(sql)) end
function QueryWorldAsync(sql) WorldDBExecute(sql) end
function QueryCharactersAsync(sql) CharDBExecute(sql) end
function QueryAuthAsync(sql) AuthDBExecute(sql) end

local function prepare(query, execute, sql)
    local parameterCount = select(2, string.gsub(sql, "?", ""))
    local prepared = {}
    function prepared:Create()
        local parameters = {}
        local statement = {}
        local function set(index, value)
            parameters[index + 1] = value
            return statement
        end
        function statement:SetNull() error("TSWoW SetNull requires an ALE SQL-null binding") end
        function statement:SetUInt8(index, value) return set(index, value) end
        function statement:SetUInt16(index, value) return set(index, value) end
        function statement:SetUInt32(index, value) return set(index, value) end
        function statement:SetUInt64(index, value) return set(index, value) end
        function statement:SetInt8(index, value) return set(index, value) end
        function statement:SetInt16(index, value) return set(index, value) end
        function statement:SetInt32(index, value) return set(index, value) end
        function statement:SetInt64(index, value) return set(index, value) end
        function statement:SetFloat(index, value) return set(index, value) end
        function statement:SetDouble(index, value) return set(index, value) end
        function statement:SetString(index, value) return set(index, value) end
        function statement:SetGUIDNumber(index, value)
            return set(index, tonumber(value:stringify()))
        end
        function statement:SetBinary() error("TSWoW SetBinary requires an ALE binary binding") end
        function statement:Send(connection)
            if connection ~= nil then
                error("TSWoW pinned database connections are not implemented on AzerothCore")
            end
            return wrapResult(query(sql, table.unpack(parameters, 1, parameterCount)))
        end
        function statement:SendAsync()
            execute(sql, table.unpack(parameters, 1, parameterCount))
        end
        return statement
    end
    return prepared
end

function PrepareWorldQuery(sql) return prepare(WorldDBQuery, WorldDBExecute, sql) end
function PrepareCharactersQuery(sql) return prepare(CharDBQuery, CharDBExecute, sql) end
function PrepareAuthQuery(sql) return prepare(AuthDBQuery, AuthDBExecute, sql) end

local function unsupportedConnection()
    error("TSWoW pinned database connections are unnecessary for newly compiled Lua ORM output and are not exposed by ALE")
end
GetWorldDBConnection = unsupportedConnection
GetCharactersDBConnection = unsupportedConnection
GetAuthDBConnection = unsupportedConnection

local function unsupportedDatabaseInfo()
    error("ALE does not expose database connection credentials to Lua")
end
WorldDatabaseInfo = unsupportedDatabaseInfo
CharactersDatabaseInfo = unsupportedDatabaseInfo
AuthDatabaseInfo = unsupportedDatabaseInfo

function CreateDatabaseSpec(databaseType, _, name, fields)
    local columns = {}
    local primary = {}
    for _, field in ipairs(fields) do
        local column = "`" .. field[1] .. "` " .. field[2]
        if field[4] then column = column .. " AUTO_INCREMENT" end
        columns[#columns + 1] = column
        if field[3] then primary[#primary + 1] = "`" .. field[1] .. "`" end
    end
    if #primary > 0 then
        columns[#columns + 1] = "PRIMARY KEY (" .. table.concat(primary, ",") .. ")"
    end
    local sql = "CREATE TABLE IF NOT EXISTS `" .. name .. "` (" ..
        table.concat(columns, ",") .. ")"
    if databaseType == 0 then QueryWorld(sql)
    elseif databaseType == 1 then QueryAuth(sql)
    elseif databaseType == 2 then QueryCharacters(sql)
    else error("Unknown TSWoW database type " .. tostring(databaseType)) end
end

function CharactersTable(target) return target end
function WorldTable(target) return target end
function AuthTable(target) return target end
function DBPrimaryKey() end
function DBField() end
function LoadDBEntry(value) value:Load(); return value end
function QueryDBEntry(value, sql) return value.LoadSQL(sql) end
function LoadDBArrayEntry(value, ...) return value.Load(...) end

function CreateDBContainer()
    local values = {}
    local container = {}
    function container:__Add(value)
        values[#values + 1] = value
        value.__dirty = false
        return value
    end
    function container:Add(value)
        values[#values + 1] = value
        value.__dirty = true
        return value
    end
    function container:Save()
        local retained = {}
        for _, value in ipairs(values) do
            if value.__deleted then
                value:_Delete()
            else
                if value.__dirty then value:Save(); value.__dirty = false end
                retained[#retained + 1] = value
            end
        end
        values = retained
    end
    function container:forEach(callback)
        for _, value in ipairs(values) do
            if not value.__deleted then callback(value) end
        end
    end
    function container:reduce(callback, initial)
        local result = initial
        self:forEach(function(value) result = callback(result, value) end)
        return result
    end
    function container:find(callback)
        for _, value in ipairs(values) do
            if not value.__deleted and callback(value) then return value end
        end
    end
    function container:ToArray()
        local result = {}
        self:forEach(function(value) result[#result + 1] = value end)
        return result
    end
    function container:Size() return #self:ToArray() end
    function container:TotalSize() return #values end
    return container
end
)lua", environment, sol::script_pass_on_error);

    if (!result.valid())
    {
        sol::error error = result;
        throw std::runtime_error(error.what());
    }
}
