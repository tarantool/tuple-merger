package = 'tuple-merger'
version = 'scm-1'

-- url and branch of the package's repository at GitHub
source  = {
    url    = 'git+https://github.com/tarantool/tuple-merger.git',
    branch = 'master',
}

description = {
    summary  = "Cross-version merger support module for Tarantool",
    detailed = [[
    External Luarock module for efficient merger. 
    Supposed to be used for any LTS and fresh version available
    be it 1.x or 2.x. Created upon Tarantool 2.x box version.
    ]],
    homepage = 'https://github.com/tarantool/tuple-merger.git',
    license  = 'BSD2',
}

dependencies = {
    'lua >= 5.1',
    'tuple-keydef',
}

external_dependencies = {
    TARANTOOL = {
        header = 'tarantool/module.h',
    };
}

build = {
    type = 'cmake',
    variables = {
        CMAKE_BUILD_TYPE="RelWithDebInfo",
        TARANTOOL_DIR="$(TARANTOOL_DIR)",
        TARANTOOL_INSTALL_LIBDIR="$(LIBDIR)",
        TARANTOOL_INSTALL_LUADIR="$(LUADIR)",
    },
}
-- vim: syntax=lua
