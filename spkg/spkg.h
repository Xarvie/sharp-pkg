/*
 * spkg.h — Sharp Package Manager: shared declarations.
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