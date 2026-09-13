#ifndef MOD_TSWOW_ALL_H
#define MOD_TSWOW_ALL_H

#include "TSEvents.h"

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using int64 = std::int64_t;
using int32 = std::int32_t;
using int16 = std::int16_t;
using int8 = std::int8_t;
using uint64 = std::uint64_t;
using uint32 = std::uint32_t;
using uint16 = std::uint16_t;
using uint8 = std::uint8_t;

template <typename T>
using TSNumber = double;

using __RESERVED__ = std::uint32_t;

inline std::string spaces(int count)
{
    return std::string(count * 4, ' ');
}

inline std::string ToStr(std::string const& value, int = 0)
{
    return value;
}

inline std::string ToStr(char const* value, int = 0)
{
    return value;
}

template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
std::string ToStr(T value, int = 0)
{
    return std::to_string(value);
}

template <typename T, std::enable_if_t<!std::is_arithmetic_v<T>, int> = 0>
std::string ToStr(T const& value, int indentation = 0)
{
    return value->stringify(indentation);
}

template <typename T>
std::string TSArray<T>::stringify(int indentation) const
{
    std::string result = "[";
    for (std::size_t index = 0; index < get_length(); ++index)
    {
        if (index)
            result += ",";
        result += ToStr((*this)[static_cast<int>(index)], indentation + 1);
    }
    return result + "]";
}

class TSClass : public std::enable_shared_from_this<TSClass>
{
public:
    virtual ~TSClass() = default;
    virtual std::string stringify(int = 0) { return "[TSClass (stringify not implemented)]"; }
    void ts_constructor() { }

    template <typename T>
    std::shared_ptr<T> ts_shared_from_this()
    {
        return std::dynamic_pointer_cast<T>(shared_from_this());
    }
};

template <typename K, typename V>
class TSDictionary
{
public:
    TSDictionary() : _values(std::make_shared<std::map<K, V>>()) { }
    TSDictionary(std::initializer_list<std::pair<K, V>> values) : TSDictionary()
    {
        for (auto const& value : values)
            _values->insert(value);
    }

    TSDictionary* operator->() { return this; }
    TSDictionary const* operator->() const { return this; }
    V& operator[](K const& key) { return (*_values)[key]; }
    bool contains(K const& key) const { return _values->find(key) != _values->end(); }
    double get_length() const { return static_cast<double>(_values->size()); }
    V get(K const& key) { return (*_values)[key]; }
    void set(K const& key, V const& value) { (*_values)[key] = value; }

    template <typename Callback>
    void forEach(Callback callback)
    {
        for (auto const& [key, value] : *_values)
            callback(key, value);
    }

    TSArray<K> keys() const
    {
        TSArray<K> result;
        for (auto const& [key, value] : *_values)
            result.push(key);
        return result;
    }

    std::string stringify(int indentation = 0) const
    {
        std::string result = "{";
        for (auto const& [key, value] : *_values)
            result += "\n" + spaces(indentation + 1) + ToStr(key, indentation + 1) + ":" +
                ToStr(value, indentation + 1);
        return result + (_values->empty() ? "" : "\n") + spaces(indentation) + "}";
    }

private:
    std::shared_ptr<std::map<K, V>> _values;
};

#define CreateDictionary TSDictionary

inline std::string __ts_string_substring(std::string const& value, double begin, double end = -1)
{
    std::size_t const first = begin < 0 ? 0 : static_cast<std::size_t>(begin);
    if (first >= value.size())
        return "";
    std::size_t const last = end < 0 ? value.size() : std::min(value.size(), static_cast<std::size_t>(end));
    return value.substr(first, last > first ? last - first : 0);
}

inline bool __ts_string_startsWith(std::string const& value, std::string const& prefix)
{
    return value.compare(0, prefix.size(), prefix) == 0;
}

inline double __ts_string_length(std::string const& value)
{
    return static_cast<double>(value.size());
}

struct MathClass
{
    MathClass* operator->() { return this; }
    double floor(double value) const { return std::floor(value); }
    double random() const { return static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0); }
};

inline MathClass Math;

struct TSConsole
{
    TSConsole* operator->() { return this; }

    template <typename T>
    void log(T const& value) { std::cout << value << std::endl; }

    template <typename T, typename... Rest>
    void log(T const& value, Rest const&... rest)
    {
        std::cout << value;
        ((std::cout << ' ' << rest), ...);
        std::cout << std::endl;
    }
};

inline TSConsole console;

class DBEntry : public TSClass { };

template <typename T>
class DBContainer;

class DBArrayEntry : public TSClass
{
public:
    void MarkDirty() { m_isDirty = true; }
    void Delete();
    bool IsDeleted() const { return _deleted; }
    bool IsDirty() const { return m_isDirty; }

protected:
    uint64 __index = 0;
    bool m_isDirty = true;

private:
    DBContainer<DBArrayEntry>* _container = nullptr;
    bool _deleted = false;
    virtual void Save() = 0;
    virtual void _Delete() = 0;
    template <typename> friend class DBContainer;
};

template <typename T>
class DBContainer
{
public:
    double Size() const { return static_cast<double>(m_size); }
    double TotalSize() const { return static_cast<double>(_values.size()); }

    std::shared_ptr<T> Add(std::shared_ptr<T> value)
    {
        if (value->_container && reinterpret_cast<DBContainer<T>*>(value->_container) != this)
            throw std::runtime_error("Attempted to add DBArrayEntry to multiple containers");
        value->_deleted = false;
        value->_container = reinterpret_cast<DBContainer<DBArrayEntry>*>(this);
        _values.push_back(value);
        ++m_size;
        return value;
    }

    void Save()
    {
        for (auto iterator = _values.begin(); iterator != _values.end();)
        {
            if ((*iterator)->IsDeleted())
            {
                if ((*iterator)->__index > 0)
                    (*iterator)->_Delete();
                iterator = _values.erase(iterator);
            }
            else
            {
                if ((*iterator)->IsDirty())
                {
                    (*iterator)->Save();
                    (*iterator)->m_isDirty = false;
                }
                ++iterator;
            }
        }
    }

    template <typename Callback>
    void forEach(Callback callback)
    {
        for (auto& value : _values)
            if (!value->IsDeleted())
                callback(value);
    }

    template <typename Callback>
    std::shared_ptr<T> find(Callback callback)
    {
        for (auto& value : _values)
            if (!value->IsDeleted() && callback(value))
                return value;
        return nullptr;
    }

    template <typename Result, typename Callback>
    Result reduce(Callback callback, Result initial)
    {
        for (auto& value : _values)
            if (!value->IsDeleted())
                initial = callback(initial, value);
        return initial;
    }

    TSArray<std::shared_ptr<T>> ToArray() const { return TSArray<std::shared_ptr<T>>(_values); }

private:
    std::vector<std::shared_ptr<T>> _values;
    uint32 m_size = 0;
    friend class DBArrayEntry;
};

inline void DBArrayEntry::Delete()
{
    if (_deleted || !_container)
        return;
    _deleted = true;
    --_container->m_size;
}

template <typename T>
std::shared_ptr<T> LoadDBEntry(std::shared_ptr<T> value)
{
    value->Load();
    return value;
}

#define LoadDBArrayEntry(cls, ...) cls::Load(__VA_ARGS__)
#define QueryDBEntry(cls, sql) cls::LoadSQL(sql)
#define DeleteDBEntry(cls, sql) cls::DeleteSQL(sql)
#define DeleteDBArrayEntry(cls, sql) cls::DeleteSQL(sql)

class TSMutex
{
public:
    TSMutex() = default;
    TSMutex(TSMutex const&) { }
    TSMutex* operator->() { return this; }
    void lock() { _lock.lock(); }
    void unlock() { _lock.unlock(); }
    bool try_lock() { return _lock.try_lock(); }
    std::string stringify(int = 0) { return "TSMutex"; }

private:
    std::mutex _lock;
};

#ifdef CreateMutex
#undef CreateMutex
#endif
#define CreateMutexLock TSMutex
#define CreateMutex TSMutex

template <typename T, typename... ArgTypes>
std::shared_ptr<T> ts_make_shared(ArgTypes&&... args)
{
    auto instance = std::make_shared<T>();
    instance->ts_constructor(std::forward<ArgTypes>(args)...);
    return instance;
}

#endif
