/*
 * spkg.h — Sharp Package Manager: shared declarations.
 *
 * spkg.run_cmd(cmd)           → {ok, out, code}
 * spkg.file_exists(path)      → bool
 * spkg.dir_exists(path)       → bool
 * spkg.mkdir_p(path)          → bool
 * spkg.glob(pattern)          → {file1, file2, ...}
 * spkg.read_file(path)        → string | nil
 * spkg.write_file(path, s)    → bool
 * spkg.find_sharpc()          → string | nil
 * spkg.find_zigcc()           → string | nil
 * spkg.home_dir()             → string
 * spkg.cwd()                  → string
 * spkg.get_mtime(path)        → number | nil
 * spkg.current_platform()     → string
 */
#ifndef SPKG_H
#define SPKG_H

#include <stdbool.h>

/* Maximum sizes */
#define SPKG_MAX_PATH   1024
#define SPKG_MAX_CMD    8192

/* Register native C functions in the Lua "spkg" module */
void spkg_register_native(struct lua_State *L);

#endif /* SPKG_H */