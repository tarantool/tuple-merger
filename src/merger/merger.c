/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
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

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>             /* lua_*() */
#include <lauxlib.h>         /* luaL_*() */

#include <module.h>
#include <msgpuck/msgpuck.h> /* mp_*() */

#include "compat/diag.h"
#include "compat/utils.h"

#include "merger-source.h" /* merge_source_*, merger_*() */

static uint32_t CTID_STRUCT_KEY_DEF_REF = 0;
static uint32_t CTID_STRUCT_TUPLE_KEYDEF_PTR = 0;
static uint32_t CTID_STRUCT_TUPLE_MERGE_SOURCE_REF = 0;

/**
 * A type of a function to create a source from a Lua iterator on
 * a Lua stack.
 *
 * Such function is to be passed to lbox_merge_source_new() as
 * a parameter.
 */
typedef struct merge_source *(*luaL_merge_source_new_f)(struct lua_State *L);

/* {{{ Helpers */

static box_key_def_t *
luaT_check_key_def(struct lua_State *L, int idx)
{
	if (! luaL_iscdata(L, idx))
		return NULL;

	uint32_t cdata_type;
	box_key_def_t **key_def_ptr = luaL_checkcdata(L, idx, &cdata_type);
	if (key_def_ptr == NULL)
		return NULL;
	if (cdata_type != CTID_STRUCT_KEY_DEF_REF &&
	    cdata_type != CTID_STRUCT_TUPLE_KEYDEF_PTR)
		return NULL;
	return *key_def_ptr;
}

/**
 * Extract a merge source from the Lua stack.
 */
static struct merge_source *
luaT_check_merge_source(struct lua_State *L, int idx)
{
	uint32_t cdata_type;
	struct merge_source **source_ptr = luaL_checkcdata(L, idx, &cdata_type);
	if (source_ptr == NULL ||
	    cdata_type != CTID_STRUCT_TUPLE_MERGE_SOURCE_REF)
		return NULL;
	return *source_ptr;
}

/* 
 * Check validity for given pointers
 * and calculated the buffer used if those pointers
 * look sane
 */
static inline ptrdiff_t
ibuf_used(char *rpos, char *wpos)
{
	assert(rpos != NULL);
	assert(wpos != NULL);
	if (rpos == NULL || wpos == NULL)
		return 0;

	return wpos - rpos;
}

/**
 * Skip an array around tuples and save its length.
 */
static int
decode_header(box_ibuf_t *buf, size_t *len_p)
{
	char **rpos;
	char **wpos;
	box_ibuf_read_range(buf, &rpos, &wpos);

	ptrdiff_t used = ibuf_used(*rpos, *wpos);
	/* Check the buffer is correct. */
	if (used < 0)
		return -1;

	/* Skip decoding if the buffer is empty. */
	if (used == 0) {
		*len_p = 0;
		return 0;
	}

	/* Check and skip the array around tuples. */
	int ok = mp_typeof(**rpos) == MP_ARRAY;
	if (ok)
		ok = mp_check_array(*rpos, *wpos) <= 0;
	if (ok)
		*len_p = mp_decode_array((const char **)rpos);

	return ok ? 0 : -1;
}

/**
 * Encode an array around tuples.
 */
static void
encode_header(box_ibuf_t *output_buffer, uint32_t result_len)
{
	char **wpos;
	box_ibuf_write_range(output_buffer, &wpos, NULL);
	box_ibuf_reserve(output_buffer, mp_sizeof_array(result_len));
	*wpos = mp_encode_array(*wpos, result_len);
}

/**
 * Get a tuple from a Lua stack.
 *
 * If a Lua table is on a specified index, create a tuple with
 * provided format and return. If format is NULL use the runtime
 * format.
 *
 * If a tuple is on a specified index, validate it against
 * provided format (if it is not NULL) and return.
 *
 * In case of an error return NULL and set a diag.
 */
static box_tuple_t *
luaT_gettuple(struct lua_State *L, int idx, box_tuple_format_t *format)
{
	box_tuple_t *tuple = luaT_istuple(L, idx);
	if (tuple == NULL) {
		/* Create a tuple from a Lua table. */
		if (format == NULL)
			format = box_tuple_format_default();
		if ((tuple = luaT_tuple_new(L, idx, format)) == NULL)
			return NULL;
	} else {
		/* Validate a tuple. */
		if (format != NULL && box_tuple_validate(tuple, format) != 0)
			return NULL;
	}
	return tuple;
}

/*
 * Buffer for the part of the module written in Lua.
 *
 * Note: It is prefixed with the project name to don't clash with
 * tarantool symbol.
 */
extern const char tuple_merger_postload_lua[];

/* {{{ Helpers */

/**
 * Execute the postload code written on Lua.
 *
 * Expects the module table on top of the Lua stack.
 */
void
execute_postload_lua(struct lua_State *L, bool first_load)
{
	int top = lua_gettop(L);

	const char *modsrc = tuple_merger_postload_lua;
	const char *modfile = "@tuple.merger/postload.lua";

	if (luaL_loadbuffer(L, modsrc, strlen(modsrc), modfile) != 0)
		luaL_error(L, "Unable to load %s", modfile);
	/*
	 * Copy the module table to top of the Lua stack: it will
	 * be passed to the script and will be accessible as
	 * `...`.
	 */
	lua_pushvalue(L, -2);
	lua_pushboolean(L, first_load);

	lua_call(L, 2, 1);

	/* Ignore Lua return value. */
	lua_settop(L, top);
}

/* }}} */

/* {{{ Create, destroy structures from Lua */

/**
 * Free a merge source from a Lua code.
 */
static int
lbox_merge_source_gc(struct lua_State *L)
{
	struct merge_source *source = luaT_check_merge_source(L, 1);
	assert(source != NULL);
	merge_source_unref(source);
	return 0;
}

/**
 * Create a new source from a Lua iterator and push it onto the
 * Lua stack.
 *
 * It is the helper for lbox_merger_new_buffer_source(),
 * lbox_merger_new_table_source() and
 * lbox_merger_new_tuple_source().
 */
static int
lbox_merge_source_new(struct lua_State *L, const char *func_name,
		      luaL_merge_source_new_f luaL_merge_source_new)
{
	int top = lua_gettop(L);
	if (top < 1 || top > 3 || !luaL_iscallable(L, 1))
		return luaL_error(L, "Usage: %s(gen, param, state)", func_name);

	/*
	 * luaL_merge_source_new() reads exactly three top values.
	 */
	while (lua_gettop(L) < 3)
		lua_pushnil(L);

	struct merge_source *source = luaL_merge_source_new(L);
	if (source == NULL)
		return luaT_error(L);
	*(struct merge_source **)
		luaL_pushcdata(L, CTID_STRUCT_TUPLE_MERGE_SOURCE_REF) = source;
	lua_pushcfunction(L, lbox_merge_source_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

/**
 * Raise a Lua error with merger.new() usage info.
 */
static int
lbox_merger_new_usage(struct lua_State *L, const char *param_name)
{
	static const char *usage = "merger.new(key_def, "
				   "{source, source, ...}[, {"
				   "reverse = <boolean> or <nil>}])";
	if (param_name == NULL)
		return luaL_error(L, "Bad params, use: %s", usage);
	else
		return luaL_error(L, "Bad param \"%s\", use: %s", param_name,
				  usage);
}

/**
 * Parse a second parameter of merger.new() into an array of
 * sources.
 *
 * Return an array of pointers to sources and set @a
 * source_count_ptr. In case of an error set a diag and return
 * NULL.
 *
 * It is the helper for lbox_merger_new().
 */
static struct merge_source **
luaT_merger_new_parse_sources(struct lua_State *L, int idx,
			      uint32_t *source_count_ptr)
{
	/* Allocate sources array. */
	uint32_t source_count = lua_objlen(L, idx);
	const size_t sources_size = sizeof(struct merge_source *) *
		source_count;
	struct merge_source **sources = malloc(sources_size);
	if (sources == NULL) {
		diag_set_oom(sources_size, "malloc", "sources");
		return NULL;
	}

	/* Save all sources. */
	for (uint32_t i = 0; i < source_count; ++i) {
		lua_pushinteger(L, i + 1);
		lua_gettable(L, idx);

		/* Extract a source from a Lua stack. */
		struct merge_source *source = luaT_check_merge_source(L, -1);
		if (source == NULL) {
			free(sources);
			diag_set_illegal("Unknown source type at index %d", i + 1);
			return NULL;
		}
		sources[i] = source;
	}
	lua_pop(L, source_count);

	*source_count_ptr = source_count;
	return sources;
}

/**
 * Create a new merger and push it to a Lua stack as a merge
 * source.
 *
 * Expect cdata<struct key_def>, a table of sources and
 * (optionally) a table of options on a Lua stack.
 */
static int
lbox_merger_new(struct lua_State *L)
{
	struct key_def *key_def;
	int top = lua_gettop(L);
	bool ok = (top == 2 || top == 3) &&
		/* key_def. */
		(key_def = luaT_check_key_def(L, 1)) != NULL &&
		/* Sources. */
		lua_istable(L, 2) == 1 &&
		/* Opts. */
		(lua_isnoneornil(L, 3) == 1 || lua_istable(L, 3) == 1);
	if (!ok)
		return lbox_merger_new_usage(L, NULL);

	/* Options. */
	bool reverse = false;

	/* Parse options. */
	if (!lua_isnoneornil(L, 3)) {
		/* Parse reverse. */
		lua_pushstring(L, "reverse");
		lua_gettable(L, 3);
		if (!lua_isnil(L, -1)) {
			if (lua_isboolean(L, -1))
				reverse = lua_toboolean(L, -1);
			else
				return lbox_merger_new_usage(L, "reverse");
		}
		lua_pop(L, 1);
	}

	uint32_t source_count = 0;
	struct merge_source **sources = luaT_merger_new_parse_sources(L, 2,
		&source_count);
	if (sources == NULL)
		return luaT_error(L);

	struct merge_source *merger = merger_new(key_def, sources, source_count,
						 reverse);
	free(sources);
	if (merger == NULL)
		return luaT_error(L);

	*(struct merge_source **)
		luaL_pushcdata(L, CTID_STRUCT_TUPLE_MERGE_SOURCE_REF) = merger;
	lua_pushcfunction(L, lbox_merge_source_gc);
	luaL_setcdatagc(L, -2);

	return 1;
}

/* }}} */

/* {{{ Buffer merge source */

struct merge_source_buffer {
	struct merge_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
	/*
	 * A reference to a buffer storing the current chunk of
	 * tuples. It is needed to prevent LuaJIT from collecting
	 * the buffer while the source consider it as the current
	 * one.
	 */
	int ref;
	/*
	 * A buffer with a current chunk of tuples.
	 */
	box_ibuf_t *buf;
	/*
	 * A merger stops before end of a buffer when it is not
	 * the last merger in the chain.
	 */
	size_t remaining_tuple_count;
};

/* Virtual methods declarations */

static void
luaL_merge_source_buffer_destroy(struct merge_source *base);
static int
luaL_merge_source_buffer_next(struct merge_source *base,
			      box_tuple_format_t *format,
			      box_tuple_t **out);

/* Non-virtual methods */

/**
 * Create a new merge source of the buffer type.
 *
 * Reads gen, param, state from the top of a Lua stack.
 *
 * In case of an error it returns NULL and sets a diag.
 */
static struct merge_source *
luaL_merge_source_buffer_new(struct lua_State *L)
{
	static struct merge_source_vtab merge_source_buffer_vtab = {
		.destroy = luaL_merge_source_buffer_destroy,
		.next = luaL_merge_source_buffer_next,
	};

	struct merge_source_buffer *source = malloc(
		sizeof(struct merge_source_buffer));
	if (source == NULL) {
		diag_set_oom(sizeof(struct merge_source_buffer),
			 "malloc", "merge_source_buffer");
		return NULL;
	}

	merge_source_create(&source->base, &merge_source_buffer_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);
	source->ref = 0;
	source->buf = NULL;
	source->remaining_tuple_count = 0;

	return &source->base;
}

/**
 * Helper for `luaL_merge_source_buffer_fetch()`.
 */
static int
luaL_merge_source_buffer_fetch_impl(struct merge_source_buffer *source,
				    struct lua_State *L)
{
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set_illegal("Expected <state>, <buffer>, got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new buffer as the current chunk. */
	if (source->ref > 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, source->ref);
		source->ref = 0;
	}
	lua_pushvalue(L, -nresult + 1); /* Popped by luaL_ref(). */
	source->buf = luaT_toibuf(L, -1);
	if (source->buf == NULL) {
		diag_set_illegal("Expected <state>, <buffer>");
		return -1;
	}
	source->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pop(L, nresult);

	/* Update remaining_tuple_count and skip the header. */
	if (decode_header(source->buf, &source->remaining_tuple_count) != 0) {
		diag_set_illegal("Invalid merge source %p",
			 &source->base);
		return -1;
	}
	return 1;
}

/**
 * Call a user provided function to get a next data chunk (a
 * buffer).
 *
 * Return 1 when a new buffer is received, 0 when a buffers
 * iterator ends and -1 at error and set a diag.
 */
static int
luaL_merge_source_buffer_fetch(struct merge_source_buffer *source)
{
	int coro_ref = LUA_REFNIL;
	int top = -1;
	struct lua_State *L = luaT_temp_luastate(&coro_ref, &top);
	if (L == NULL)
		return -1;
	int rc = luaL_merge_source_buffer_fetch_impl(source, L);
	luaT_release_temp_luastate(L, coro_ref, top);
	return rc;
}

/* Virtual methods */

/**
 * destroy() virtual method implementation for a buffer source.
 *
 * @see struct merge_source_vtab
 */
static void
luaL_merge_source_buffer_destroy(struct merge_source *base)
{
	struct merge_source_buffer *source = container_of(base,
		struct merge_source_buffer, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);
	if (source->ref > 0)
		luaL_unref(luaT_state(), LUA_REGISTRYINDEX, source->ref);

	free(source);
}

/**
 * next() virtual method implementation for a buffer source.
 *
 * @see struct merge_source_vtab
 */
static int
luaL_merge_source_buffer_next(struct merge_source *base,
			      box_tuple_format_t *format,
			      box_tuple_t **out)
{
	struct merge_source_buffer *source = container_of(base,
		struct merge_source_buffer, base);

	/*
	 * Handle the case when all data were processed: ask a
	 * next chunk until a non-empty chunk is received or a
	 * chunks iterator ends.
	 */
	while (source->remaining_tuple_count == 0) {
		int rc = luaL_merge_source_buffer_fetch(source);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			*out = NULL;
			return 0;
		}
	}
	char **rpos;
	char **wpos;
	box_ibuf_read_range(source->buf, &rpos, &wpos);
	if (ibuf_used(*rpos, *wpos) == 0) {
		diag_set_illegal("Unexpected msgpack buffer end");
		return -1;
	}
	const char *tuple_beg = *rpos;
	const char *tuple_end = tuple_beg;
	if (mp_check(&tuple_end, *wpos) != 0) {
		diag_set_illegal("Unexpected msgpack buffer end");
		return -1;
	}
	--source->remaining_tuple_count;
	*rpos = (char *)tuple_end;
	if (format == NULL)
		format = box_tuple_format_default();
	box_tuple_t *tuple = box_tuple_new(format, tuple_beg, tuple_end);
	if (tuple == NULL)
		return -1;

	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/* Lua functions */

/**
 * Create a new buffer source and push it onto the Lua stack.
 */
static int
lbox_merger_new_buffer_source(struct lua_State *L)
{
	return lbox_merge_source_new(L, "merger.new_buffer_source",
				     luaL_merge_source_buffer_new);
}

/* }}} */

/* {{{ Table merge source */

struct merge_source_table {
	struct merge_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
	/*
	 * A reference to a table with a current chunk of tuples.
	 */
	int ref;
	/* An index of current tuples within a current chunk. */
	int next_idx;
};

/* Virtual methods declarations */

static void
luaL_merge_source_table_destroy(struct merge_source *base);
static int
luaL_merge_source_table_next(struct merge_source *base,
			     box_tuple_format_t *format,
			     box_tuple_t **out);

/* Non-virtual methods */

/**
 * Create a new merge source of the table type.
 *
 * In case of an error it returns NULL and set a diag.
 */
static struct merge_source *
luaL_merge_source_table_new(struct lua_State *L)
{
	static struct merge_source_vtab merge_source_table_vtab = {
		.destroy = luaL_merge_source_table_destroy,
		.next = luaL_merge_source_table_next,
	};

	struct merge_source_table *source = malloc(
		sizeof(struct merge_source_table));
	if (source == NULL) {
		diag_set_oom(sizeof(struct merge_source_table),
			 "malloc", "merge_source_table");
		return NULL;
	}

	merge_source_create(&source->base, &merge_source_table_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);
	source->ref = 0;
	source->next_idx = 1;

	return &source->base;
}

/**
 * Call a user provided function to fill the source.
 *
 * Return 0 when a tables iterator ends, 1 when a new table is
 * received and -1 at an error (set a diag).
 */
static int
luaL_merge_source_table_fetch(struct merge_source_table *source,
			      struct lua_State *L)
{
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set_illegal("Expected <state>, <table>, got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new table as the current chunk. */
	if (source->ref > 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, source->ref);
		source->ref = 0;
	}
	lua_pushvalue(L, -nresult + 1); /* Popped by luaL_ref(). */
	if (lua_istable(L, -1) == 0) {
		diag_set_illegal("Expected <state>, <table>");
		return -1;
	}
	source->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	source->next_idx = 1;
	lua_pop(L, nresult);

	return 1;
}

/* Virtual methods */

/**
 * destroy() virtual method implementation for a table source.
 *
 * @see struct merge_source_vtab
 */
static void
luaL_merge_source_table_destroy(struct merge_source *base)
{
	struct merge_source_table *source = container_of(base,
		struct merge_source_table, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);
	if (source->ref > 0)
		luaL_unref(luaT_state(), LUA_REGISTRYINDEX, source->ref);

	free(source);
}

/**
 * Helper for `luaL_merge_source_table_next()`.
 */
static int
luaL_merge_source_table_next_impl(struct merge_source *base,
				  box_tuple_format_t *format,
				  box_tuple_t **out,
				  struct lua_State *L)
{
	struct merge_source_table *source = container_of(base,
		struct merge_source_table, base);

	if (source->ref > 0) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, source->ref);
		lua_pushinteger(L, source->next_idx);
		lua_gettable(L, -2);
	}
	/*
	 * If all data were processed, try to fetch more.
	 */
	while (source->ref == 0 || lua_isnil(L, -1)) {
		if (source->ref > 0)
			lua_pop(L, 2);
		int rc = luaL_merge_source_table_fetch(source, L);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			*out = NULL;
			return 0;
		}
		/*
		 * Retry tuple extracting when a next table is
		 * received.
		 */
		lua_rawgeti(L, LUA_REGISTRYINDEX, source->ref);
		lua_pushinteger(L, source->next_idx);
		lua_gettable(L, -2);
	}

	box_tuple_t *tuple = luaT_gettuple(L, -1, format);
	if (tuple == NULL)
		return -1;

	++source->next_idx;
	lua_pop(L, 2);

	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/**
 * next() virtual method implementation for a table source.
 *
 * @see struct merge_source_vtab
 */
static int
luaL_merge_source_table_next(struct merge_source *base,
			     box_tuple_format_t *format,
			     box_tuple_t **out)
{
	int coro_ref = LUA_REFNIL;
	int top = -1;
	struct lua_State *L = luaT_temp_luastate(&coro_ref, &top);
	if (L == NULL)
		return -1;
	int rc = luaL_merge_source_table_next_impl(base, format, out, L);
	luaT_release_temp_luastate(L, coro_ref, top);
	return rc;
}

/* Lua functions */

/**
 * Create a new table source and push it onto the Lua stack.
 */
static int
lbox_merger_new_table_source(struct lua_State *L)
{
	return lbox_merge_source_new(L, "merger.new_table_source",
				     luaL_merge_source_table_new);
}

/* }}} */

/* {{{ Tuple merge source */

struct merge_source_tuple {
	struct merge_source base;
	/*
	 * A reference to a Lua iterator to fetch a next chunk of
	 * tuples.
	 */
	struct luaL_iterator *fetch_it;
};

/* Virtual methods declarations */

static void
luaL_merge_source_tuple_destroy(struct merge_source *base);
static int
luaL_merge_source_tuple_next(struct merge_source *base,
			     box_tuple_format_t *format,
			     box_tuple_t **out);

/* Non-virtual methods */

/**
 * Create a new merge source of the tuple type.
 *
 * In case of an error it returns NULL and set a diag.
 */
static struct merge_source *
luaL_merge_source_tuple_new(struct lua_State *L)
{
	static struct merge_source_vtab merge_source_tuple_vtab = {
		.destroy = luaL_merge_source_tuple_destroy,
		.next = luaL_merge_source_tuple_next,
	};

	struct merge_source_tuple *source = malloc(
		sizeof(struct merge_source_tuple));
	if (source == NULL) {
		diag_set_oom(sizeof(struct merge_source_tuple),
			 "malloc", "merge_source_tuple");
		return NULL;
	}

	merge_source_create(&source->base, &merge_source_tuple_vtab);

	source->fetch_it = luaL_iterator_new(L, 0);

	return &source->base;
}

/**
 * Call a user provided function to fill the source.
 *
 * This function does not check whether a user-provided value
 * is a tuple. A called should check it on its side.
 *
 * Return 1 at success and push a resulting tuple to a the Lua
 * stack.
 * Return 0 when no more data.
 * Return -1 at error (set a diag).
 */
static int
luaL_merge_source_tuple_fetch(struct merge_source_tuple *source,
			       struct lua_State *L)
{
	int nresult = luaL_iterator_next(L, source->fetch_it);

	/* Handle a Lua error in a gen function. */
	if (nresult == -1)
		return -1;

	/* No more data: do nothing. */
	if (nresult == 0)
		return 0;

	/* Handle incorrect results count. */
	if (nresult != 2) {
		diag_set_illegal("Expected <state>, <tuple>, got %d "
			 "return values", nresult);
		return -1;
	}

	/* Set a new tuple as the current chunk. */
	lua_insert(L, -2); /* Swap state and tuple. */
	lua_pop(L, 1); /* Pop state. */

	return 1;
}

/* Virtual methods */

/**
 * destroy() virtual method implementation for a tuple source.
 *
 * @see struct merge_source_vtab
 */
static void
luaL_merge_source_tuple_destroy(struct merge_source *base)
{
	struct merge_source_tuple *source = container_of(base,
		struct merge_source_tuple, base);

	assert(source->fetch_it != NULL);
	luaL_iterator_delete(source->fetch_it);

	free(source);
}

/**
 * Helper for `luaL_merge_source_tuple_next()`.
 */
static int
luaL_merge_source_tuple_next_impl(struct merge_source *base,
				  box_tuple_format_t *format,
				  box_tuple_t **out,
				  struct lua_State *L)
{
	struct merge_source_tuple *source = container_of(base,
		struct merge_source_tuple, base);

	int rc = luaL_merge_source_tuple_fetch(source, L);
	if (rc < 0)
		return -1;
	/*
	 * Check whether a tuple appears after the fetch.
	 */
	if (rc == 0) {
		*out = NULL;
		return 0;
	}

	box_tuple_t *tuple = luaT_gettuple(L, -1, format);
	if (tuple == NULL)
		return -1;

	lua_pop(L, 1);
	box_tuple_ref(tuple);
	*out = tuple;
	return 0;
}

/**
 * next() virtual method implementation for a tuple source.
 *
 * @see struct merge_source_vtab
 */
static int
luaL_merge_source_tuple_next(struct merge_source *base,
			     box_tuple_format_t *format,
			     box_tuple_t **out)
{
	int coro_ref = LUA_REFNIL;
	int top = -1;
	struct lua_State *L = luaT_temp_luastate(&coro_ref, &top);
	if (L == NULL)
		return -1;
	int rc = luaL_merge_source_tuple_next_impl(base, format, out, L);
	luaT_release_temp_luastate(L, coro_ref, top);
	return rc;
}

/* Lua functions */

/**
 * Create a new tuple source and push it onto the Lua stack.
 */
static int
lbox_merger_new_tuple_source(struct lua_State *L)
{
	return lbox_merge_source_new(L, "merger.new_tuple_source",
				     luaL_merge_source_tuple_new);
}

/* }}} */

/* {{{ Merge source Lua methods */

/**
 * Iterator gen function to traverse source results.
 *
 * Expected a nil as the first parameter (param) and a
 * merge_source as the second parameter (state) on a Lua stack.
 *
 * Push the original merge_source (as a new state) and a next
 * tuple.
 */
static int
lbox_merge_source_gen(struct lua_State *L)
{
	struct merge_source *source;
	bool ok = lua_gettop(L) == 2 && lua_isnil(L, 1) &&
		(source = luaT_check_merge_source(L, 2)) != NULL;
	if (!ok)
		return luaL_error(L, "Bad params, use: lbox_merge_source_gen("
				  "nil, merge_source)");

#ifndef NDEBUG
	int top = lua_gettop(L);
#endif

	box_tuple_t *tuple;
	if (merge_source_next(source, NULL, &tuple) != 0)
		return luaT_error(L);
	if (tuple == NULL) {
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}

	/*
	 * It is crucial to have the merge_source at the topmost
	 * slot to actually return merge_source and tuple as the
	 * result.
	 *
	 * Despite that merge_source_next() does not accept a Lua
	 * state as an argument, it may use a temporary Lua state
	 * internally. This state may be the same as L.
	 *
	 * merge_source_next() must return the Lua stack to the
	 * original state if it uses the stack.
	 */
	assert(lua_gettop(L) == top);
	/*
	 * We want the consistent function behaviour in Debug and
	 * Release builds. If the top value is not cdata
	 * luaT_check_merge_source() will raise the error in Debug
	 * build. So add the assert with luaL_iscdata() check.
	 */
	assert(luaL_iscdata(L, -1));
	assert(luaT_check_merge_source(L, -1) == source);

	/*
	 * luaT_pushtuple() references the tuple, so we
	 * unreference it on merger's side.
	 */
	luaT_pushtuple(L, tuple);
	box_tuple_unref(tuple);

	/* Return merge_source and tuple. */
	return 2;
}

/**
 * Iterate over merge source results from Lua.
 *
 * Push three values to the Lua stack:
 *
 * 1. gen (lbox_merge_source_gen wrapped by fun.wrap());
 * 2. param (nil);
 * 3. state (merge_source).
 */
static int
lbox_merge_source_ipairs(struct lua_State *L)
{
	struct merge_source *source;
	bool ok = lua_gettop(L) == 1 &&
		(source = luaT_check_merge_source(L, 1)) != NULL;
	if (!ok)
		return luaL_error(L, "Usage: merge_source:ipairs()");
	/* Stack: merge_source. */

	luaL_loadstring(L, "return require('fun').wrap");
	lua_call(L, 0, 1);
	lua_insert(L, -2); /* Swap merge_source and wrap. */
	/* Stack: wrap, merge_source. */

	lua_pushcfunction(L, lbox_merge_source_gen);
	lua_insert(L, -2); /* Swap merge_source and gen. */
	/* Stack: wrap, gen, merge_source. */

	/*
	 * Push nil as an iterator param, because all needed state
	 * is in a merge source.
	 */
	lua_pushnil(L);
	/* Stack: wrap, gen, merge_source, nil. */

	lua_insert(L, -2); /* Swap merge_source and nil. */
	/* Stack: wrap, gen, nil, merge_source. */

	/* Call fun.wrap(gen, nil, merge_source). */
	lua_call(L, 3, 3);
	return 3;
}

/**
 * Write source results into ibuf.
 *
 * It is the helper for lbox_merge_source_select().
 */
static int
encode_result_buffer(struct lua_State *L, struct merge_source *source,
		     box_ibuf_t *output_buffer, uint32_t limit)
{
	uint32_t result_len = 0;
	uint32_t result_len_offset = 4;
	char **wpos;
	box_ibuf_write_range(output_buffer, &wpos, NULL);

	/*
	 * Reserve maximum size for the array around resulting
	 * tuples to set it later.
	 */
	encode_header(output_buffer, UINT32_MAX);

	/* Fetch, merge and copy tuples to the buffer. */
	box_tuple_t *tuple;
	int rc = 0;
	while (result_len < limit && (rc =
	       merge_source_next(source, NULL, &tuple)) == 0 &&
	       tuple != NULL) {
		uint32_t bsize = box_tuple_bsize(tuple);
		box_ibuf_reserve(output_buffer, bsize);
		box_tuple_to_buf(tuple, *wpos, bsize);
		*wpos += bsize;
		result_len_offset += bsize;
		++result_len;

		/* The received tuple is not needed anymore */
		box_tuple_unref(tuple);
	}

	if (rc != 0)
		return luaT_error(L);

	/* Write the real array size. */
	mp_store_u32(*wpos - result_len_offset, result_len);

	return 0;
}

/**
 * Write source results into a new Lua table.
 *
 * It is the helper for lbox_merge_source_select().
 */
static int
create_result_table(struct lua_State *L, struct merge_source *source,
		    uint32_t limit)
{
	/* Create result table. */
	lua_newtable(L);

	uint32_t cur = 1;

	/* Fetch, merge and save tuples to the table. */
	box_tuple_t *tuple;
	int rc = 0;
	while (cur - 1 < limit && (rc =
	       merge_source_next(source, NULL, &tuple)) == 0 &&
	       tuple != NULL) {
		luaT_pushtuple(L, tuple);
		lua_rawseti(L, -2, cur);
		++cur;

		/*
		 * luaT_pushtuple() references the tuple, so we
		 * unreference it on merger's side.
		 */
		box_tuple_unref(tuple);
	}

	if (rc != 0)
		return luaT_error(L);

	return 1;
}

/**
 * Raise a Lua error with merger_inst:select() usage info.
 */
static int
lbox_merge_source_select_usage(struct lua_State *L, const char *param_name)
{
	static const char *usage = "merge_source:select([{"
				   "buffer = <cdata<struct ibuf>> or <nil>, "
				   "limit = <number> or <nil>}])";
	if (param_name == NULL)
		return luaL_error(L, "Bad params, use: %s", usage);
	else
		return luaL_error(L, "Bad param \"%s\", use: %s", param_name,
				  usage);
}

/**
 * Pull results of a merge source to a Lua stack.
 *
 * Write results into a buffer or a Lua table depending on
 * options.
 *
 * Expected a merge source and options (optional) on a Lua stack.
 *
 * Return a Lua table or nothing when a 'buffer' option is
 * provided.
 */
static int
lbox_merge_source_select(struct lua_State *L)
{
	struct merge_source *source;
	int top = lua_gettop(L);
	bool ok = (top == 1 || top == 2) &&
		/* Merge source. */
		(source = luaT_check_merge_source(L, 1)) != NULL &&
		/* Opts. */
		(lua_isnoneornil(L, 2) == 1 || lua_istable(L, 2) == 1);
	if (!ok)
		return lbox_merge_source_select_usage(L, NULL);

	uint32_t limit = UINT32_MAX;
	box_ibuf_t *output_buffer = NULL;

	/* Parse options. */
	if (!lua_isnoneornil(L, 2)) {
		/* Parse buffer. */
		lua_pushstring(L, "buffer");
		lua_gettable(L, 2);
		if (!lua_isnil(L, -1)) {
			if ((output_buffer = luaT_toibuf(L, -1)) == NULL)
				return lbox_merge_source_select_usage(L,
					"buffer");
		}
		lua_pop(L, 1);

		/* Parse limit. */
		lua_pushstring(L, "limit");
		lua_gettable(L, 2);
		if (!lua_isnil(L, -1)) {
			if (lua_isnumber(L, -1))
				limit = lua_tointeger(L, -1);
			else
				return lbox_merge_source_select_usage(L,
					"limit");
		}
		lua_pop(L, 1);
	}

	if (output_buffer == NULL)
		return create_result_table(L, source, limit);
	else
		return encode_result_buffer(L, source, output_buffer, limit);
}

/* }}} */

/**
 * Register the module.
 */
LUA_API int
luaopen_tuple_merger(struct lua_State *L)
{
	static bool first_load = true;

	/* Built-in key_def module. */
	luaL_cdef(L, "struct key_def;");
	CTID_STRUCT_KEY_DEF_REF = luaL_ctypeid(L, "struct key_def &");

	/* External key_def module. */
	luaL_cdef(L, "struct tuple_keydef;");
	CTID_STRUCT_TUPLE_KEYDEF_PTR = luaL_ctypeid(L, "struct tuple_keydef*");

	luaL_cdef(L, "struct tuple_merge_source;");
	CTID_STRUCT_TUPLE_MERGE_SOURCE_REF =
		luaL_ctypeid(L, "struct tuple_merge_source&");

	/* Create the module table. */
	static const struct luaL_Reg meta[] = {
		{"new_buffer_source", lbox_merger_new_buffer_source},
		{"new_table_source", lbox_merger_new_table_source},
		{"new_tuple_source", lbox_merger_new_tuple_source},
		{"new", lbox_merger_new},
		{NULL, NULL}
	};
	size_t len = sizeof(meta) / sizeof(meta[0]);
	lua_createtable(L, 0, len - 1);
	luaL_register(L, NULL, meta);

	/* Add internal.{select,ipairs}(). */
	lua_newtable(L); /* merger.internal */
	lua_pushcfunction(L, lbox_merge_source_select);
	lua_setfield(L, -2, "select");
	lua_pushcfunction(L, lbox_merge_source_ipairs);
	lua_setfield(L, -2, "ipairs");
	lua_setfield(L, -2, "internal");

	/* Execute Lua part of the module. */
	execute_postload_lua(L, first_load);
	first_load = false;

	return 1;
}
