/*
 * Copyright 2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdlib.h>

#include <lua.h>
#include <lauxlib.h>

#include <module.h>

#include "utils.h"
#include "diag.h"

/**
 * @brief very dumb luaT_temp_state compatibility layer
 * Does not pretend to be fast and effecient, but rather
 * good enough and use only publicly available api calls
 */

/* dirty hack to make code as much resembling to original
   version as possible */
#define tarantool_L luaT_state()

struct lua_State *
luaT_temp_luastate(int *coro_ref, int *top)
{
	/* Popped by luaL_ref(). */
	struct lua_State *L = lua_newthread(tarantool_L);
	if (L == NULL)
		return NULL;
	/*
	 * We should remove the reference to the newly created Lua
	 * thread from tarantool_L, because of two reasons:
	 *
	 * First, if we'll push something to tarantool_L and
	 * yield, then another fiber will not know that a stack
	 * top is changed and may operate on a wrong slot.
	 *
	 * Second, many requests that push a value to tarantool_L
	 * and yield may exhaust available slots on the stack. It
	 * is limited by LUAI_MAXSTACK build time constant (~65K).
	 *
	 * We cannot just pop the value, but should keep the
	 * reference in the registry while it is in use.
	 * Otherwise it may be garbage collected.
	 */
	*coro_ref = luaL_ref(tarantool_L, LUA_REGISTRYINDEX);
	*top = -1;
	return L;
}

void
luaT_release_temp_luastate(struct lua_State *L, int coro_ref, int top)
{
	if (top >= 0)
		lua_settop(L, top);
	luaL_unref(tarantool_L, LUA_REGISTRYINDEX, coro_ref);
}

/* {{{ Helper functions to interact with a Lua iterator from C */

struct luaL_iterator {
	int gen;
	int param;
	int state;
};

struct luaL_iterator *
luaL_iterator_new(lua_State * L, int idx)
{
	struct luaL_iterator *it = malloc(sizeof(struct luaL_iterator));
	if (it == NULL) {
		diag_set_oom(sizeof(struct luaL_iterator), "malloc",
			     "luaL_iterator");
		return NULL;
	}

	if (idx == 0) {
		/* gen, param, state are on top of a Lua stack. */
		lua_pushvalue(L, -3);	/* Popped by luaL_ref(). */
		it->gen = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pushvalue(L, -2);	/* Popped by luaL_ref(). */
		it->param = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pushvalue(L, -1);	/* Popped by luaL_ref(). */
		it->state = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		/*
		 * {gen, param, state} table is at idx in a Lua
		 * stack.
		 */
		lua_rawgeti(L, idx, 1);	/* Popped by luaL_ref(). */
		it->gen = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_rawgeti(L, idx, 2);	/* Popped by luaL_ref(). */
		it->param = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_rawgeti(L, idx, 3);	/* Popped by luaL_ref(). */
		it->state = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	return it;
}

int
luaL_iterator_next(lua_State * L, struct luaL_iterator *it)
{
	int frame_start = lua_gettop(L);

	/* Call gen(param, state). */
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->gen);
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->param);
	lua_rawgeti(L, LUA_REGISTRYINDEX, it->state);
	if (luaT_call(L, 2, LUA_MULTRET) != 0) {
		/*
		 * Pop garbage from the call (a gen function
		 * likely will not leave the stack even when raise
		 * an error), pop a returned error.
		 */
		lua_settop(L, frame_start);
		return -1;
	}
	int nresults = lua_gettop(L) - frame_start;

	/*
	 * gen() function can either return nil when the iterator
	 * ends or return zero count of values.
	 *
	 * In LuaJIT pairs() returns nil, but ipairs() returns
	 * nothing when ends.
	 */
	if (nresults == 0 || lua_isnil(L, frame_start + 1)) {
		lua_settop(L, frame_start);
		return 0;
	}

	/* Save the first result to it->state. */
	luaL_unref(L, LUA_REGISTRYINDEX, it->state);
	lua_pushvalue(L, frame_start + 1);	/* Popped by luaL_ref(). */
	it->state = luaL_ref(L, LUA_REGISTRYINDEX);

	return nresults;
}

void
luaL_iterator_delete(struct luaL_iterator *it)
{
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, it->gen);
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, it->param);
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, it->state);
	free(it);
}

/* }}} */
