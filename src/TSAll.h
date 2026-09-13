#ifndef MOD_TSWOW_ALL_H
#define MOD_TSWOW_ALL_H

#include "TSEvents.h"

#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

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
