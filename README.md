# Tarantool `tuple.merger` module for versions 1.10.x and 2.x


## Overview

The module provides convenient access to streams of tuples
via generic, table-like interfaces. This allows to implement
efficient merge/combine operations on a stream of data.

## API

The functional interface is that we used to have in [built-in merger
module](https://www.tarantool.io/en/doc/latest/reference/reference_lua/merger/), available in Tarantool 2.x version 

## Prerequisites

Prerequisite is the "Module API" of corresponding installed Tarantool version and their headers available. Such package usually named as `tarantool-dev`, or headers may be generated from [Tarantool sources](https://www.tarantool.io/en/doc/latest/dev_guide/building_from_source/) as side effect of `module_api` target build.

## Build and install tuple.merger locally

Please use `tarantoolctl` utility installed with corresponding
Tarantool version installation, which allows you to fetch all
dependencies (i.e. `tuple-keydef`) and build module using the
proper, Lua way

```bash

tarantoolctl rocks make

```

## Simple usage examples/tests

```
tarantool test/merger-test.lua

make -C examples/chnked_example_fast test
```

## Backward and forward compatibility guarantees

At the moment of writing, supported Tarantool versions are:

- 1.10 since 1.10.7-91-g9ee14eca5;
- 2.4 since 2.4.2-131-g8a2028cca;
- 2.5 since 2.5.1-150-g98ff9aae0;
- 2.6 since 2.6.0-193-g3dc6a76c8;
- all future versions 2.7 and onward.

The older tarantool versions are not supported, because they
lack of necessary `module api` headers.

## When you need to use module, and not built-in module?

The key difference here - it works not only for all supported 
2.x versions (where we do have builtin `merger` module), but also
for recent 1.10 version (see version information above).

_So you may use this module if you need to have efficient merger 
implementation for code which has to run in both, 1.10.* and 2.* 
versions._

Also external module usually has shorter release cycle, and may 
be updated independently of Tarantool kernel, thus updated more frequently than corresponding builtin module.
