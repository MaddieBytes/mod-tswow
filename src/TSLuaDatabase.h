#ifndef MOD_TSWOW_LUA_DATABASE_H
#define MOD_TSWOW_LUA_DATABASE_H

#include <sol/forward.hpp>

void BindLuaDatabaseCompatibility(sol::state_view& lua, sol::environment& environment);

#endif
