#ifndef MOD_TSWOW_LUA_RUNTIME_H
#define MOD_TSWOW_LUA_RUNTIME_H

#include <cstdint>
#include <filesystem>

bool LoadLuaLivescripts(std::filesystem::path const& root);
void UnloadLuaLivescripts();
void UpdateLuaLivescripts(std::uint32_t diff);
void RunLuaDelayedCallbacks(void* owner);
void ClearLuaEntityState(void* owner);
bool LuaLivescriptsNeedReload();

#endif
