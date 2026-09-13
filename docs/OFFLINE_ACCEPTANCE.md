# Offline acceptance evidence

Validated on Windows against these pinned inputs:

- AzerothCore fork: `fa4502b4e952a048e51974557dbde63c912c8677`
- official `mod-ale`: `bd74eae623ca63154d3eb49e1d187e872ef13370`
- `tswow-tests`: `748820cdf2b90e61c0f4c415bac53de4ff173551`
- compatibility reference TSWoW: `00608832b426acf09c00183857856cc16fe0f250`
- compatibility reference TrinityCore: `c797f13b2c9f5a1a2fadef03eab660ffb675800d`

The isolated TSWoW generator rebuilt 994 TypeScript files and produced `global.d.ts` with generated enums and
the restored `TSGUID.GetLow()` declaration. The pinned TypeScript 4.7.3 and TypeScriptToLua 1.6.2 compiler then
compiled an unchanged copy of the test module with zero diagnostics. The compatibility patch retains normal
method calls while emitting function-valued properties without an extra Lua `self` argument.

The offline tag resolver replaced all 15 tags used by the livescripts, including the one multi-value tag, and
verified that no `TAG`, `UTAG`, `GetIDTag`, or `GetIDTagUnique` calls remained. The IDs are deterministic fixture
values in the 910001-910016 range. They do not represent generated game data and cannot prove a datascript run.

The harness uses a private Lua 5.4 state, the real `mod-tswow` loader and database compatibility source, and
in-memory ALE database functions. It loaded every resolved Lua module, invoked every exported `Main`, cleared
callbacks, unloaded the runtime, closed the state, and exited with code 0. A focused fixture also passed plain
query result behavior, prepared numeric and string parameters, prepared async execution, ORM loading, explicit
`UUID_SHORT()` allocation, prepared array-row save, dirty-state clearing, and shutdown.

The combined static AzerothCore, official `mod-ale`, and `mod-tswow` RelWithDebInfo build produced
`worldserver.exe` with SHA-256
`A6EC4D683317C23B5933543486B2166C9BAE30A4F6E043269FC764A2D4431786`.
AzerothCore's C++ codestyle checks and `git diff --check` passed.

No server, client, or external database was started or contacted. The following items remain outside this
offline milestone:

- generate real test data, SQL rows, and production tag IDs from an isolated AzerothCore dataset;
- run database initialization and exercise SQL/ORM against the isolated world, characters, and auth schemas;
- validate binary fields, SQL NULL parameters, and existing-table schema migration;
- perform isolated startup, login, reload, restart persistence, Test Island, and Windows client checks;
- build and run on Linux and repeat acceptance against the second pinned AzerothCore revision.
