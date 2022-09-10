#!/usr/bin/env tarantool

local buffer = require('buffer')
local msgpack = require('msgpack')
local keydef = require('tuple.keydef')
local merger = require('tuple.merger')

local tap = require('tap')
local test = tap.test('gh-7657-merger-use-after-free-in-gen')
test:plan(4)

-- There are four types of merge sources: tuple, table, buffer and
-- merger. The test cases below are constructed in the same way,
-- each for its own type of a merge source.
--
-- It would be enough to test only one source type: all were
-- affected, because the problem was in the common code. However,
-- we shouldn't make such assumptions in the testing code.

test:test('test_tuple_source', function(test)
    test:plan(4)

    local source = merger.new_tuple_source(pairs({{1, '1'}, {2, '2'}}))
    local gen, param, state = source:pairs():unwrap()

    -- At this point we have two references to the merge source
    -- object: `source` and `state`.
    --
    -- The buggy `gen` function returns a new `state` object,
    -- which holds the same pointer, but is not the same cdata
    -- object. So we loss the `state` reference.
    --
    -- In fact, the problem wouldn't occur if the new `state`
    -- would be pushed to the Lua stack together with increasing
    -- of the refcounter of the merge source structure and would
    -- have a GC handler that decreases it. The buggy `gen`
    -- doesn't touch the refcounter.
    --
    -- We should call :unwrap(), because the luafun iterator holds
    -- the original `state` object.
    --
    -- The fixed code returns the same cdata object, so `state`
    -- remains the same. The reference isn't lost.
    local tuple
    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {1, '1'}, '1st iteration (tuple)')

    -- Loss the second reference to the merge source object. If it
    -- is the last reference to this cdata object in Lua, the GC
    -- handler will be called and will decrease the refcounter in
    -- the merge source structure to zero. The structure is freed
    -- in this case.
    --
    -- If the `gen` function returns correct `state` above, we
    -- still have a valid reference and the GC handler will not
    -- be called.
    source = nil -- luacheck: no unused
    collectgarbage()

    -- Access the source to trigger the crash if it was freed.
    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {2, '2'}, '2nd iteration (tuple)')

    -- Finish reading of the tuples. It is not necessary for
    -- reproducing the problem, but will cause a feeling of
    -- a finished action in one who will read it. Positive
    -- emotions are important.
    state, tuple = gen(param, state)
    test:is(state, nil, '3rd iteration (state)')
    test:is(tuple, nil, '3rd iteration (tuple)')
end)

test:test('test_table_source', function(test)
    test:plan(4)

    local source = merger.new_source_fromtable({{1, '1'}, {2, '2'}})
    local gen, param, state = source:pairs():unwrap()

    local tuple
    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {1, '1'}, '1st iteration (tuple)')

    source = nil -- luacheck: no unused
    collectgarbage()

    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {2, '2'}, '2nd iteration (tuple)')

    state, tuple = gen(param, state)
    test:is(state, nil, '3rd iteration (state)')
    test:is(tuple, nil, '3rd iteration (tuple)')
end)

test:test('test_buffer_source', function(test)
    test:plan(4)

    local buf = buffer.ibuf()
    msgpack.encode({{1, '1'}, {2, '2'}}, buf)
    local source = merger.new_source_frombuffer(buf)
    local gen, param, state = source:pairs():unwrap()

    local tuple
    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {1, '1'}, '1st iteration (tuple)')

    source = nil -- luacheck: no unused
    collectgarbage()

    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {2, '2'}, '2nd iteration (tuple)')

    state, tuple = gen(param, state)
    test:is(state, nil, '3rd iteration (state)')
    test:is(tuple, nil, '3rd iteration (tuple)')
end)

test:test('test_merger', function(test)
    test:plan(4)

    local kd = keydef.new({{fieldno = 1, type = 'unsigned'}})
    local source = merger.new_source_fromtable({{1, '1'}, {2, '2'}})
    local gen, param, state = merger.new(kd, {source}):pairs():unwrap()

    local tuple
    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {1, '1'}, '1st iteration (tuple)')

    source = nil -- luacheck: no unused
    collectgarbage()

    state, tuple = gen(param, state)
    test:is_deeply(tuple:totable(), {2, '2'}, '2nd iteration (tuple)')

    state, tuple = gen(param, state)
    test:is(state, nil, '3rd iteration (state)')
    test:is(tuple, nil, '3rd iteration (tuple)')
end)

os.exit(test:check() and 0 or 1)
