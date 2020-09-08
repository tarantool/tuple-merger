local ffi = require('ffi')
local mergerx = require('mergerx')
local key_def = mergerx.key_def
local key_def_t = ffi.typeof('struct key_def_key_def')

local methods = {
    ['extract_key'] = key_def.extract_key,
    ['compare'] = key_def.compare,
    ['compare_with_key'] = key_def.compare_with_key,
    ['merge'] = key_def.merge,
    ['totable'] = key_def.totable,
    ['__serialize'] = key_def.totable,
}

ffi.metatype(key_def_t, {
    __index = function(self, key)
        return methods[key]
    end,
    __tostring = function(key_def) return "<struct key_def_key_def &>" end,
})

return key_def
