# Vendored build dependencies (not committed)

The Aroma module and the IOSU patcher need two prebuilt libraries from
the wiiu-env project. They are GPL binaries, so they are kept here for a
reproducible local build but are **gitignored, not committed**.

- `wums/`     — WiiUModuleSystem (`libwums`, `wums_rules`, headers).
                Build from https://github.com/wiiu-env/WiiUModuleSystem
- `libmocha/` — libmocha (`libmocha.a`, `mocha/mocha.h`), the IOSU kernel
                read/write IPC used to apply the endpoint-ownership patch.
                Build from https://github.com/wiiu-env/libmocha
- `functionpatcher/` — libfunctionpatcher (`libfunctionpatcher.a`,
                `function_patcher/*.h`), the client for the FunctionPatcher
                module used to intercept nsysnet exports.
                Build from https://github.com/wiiu-env/libfunctionpatcher

Each Makefile takes `WUMS_ROOT` / `MOCHA_ROOT` / `FUNCTIONPATCHER_ROOT`
and defaults to these paths, so once they are present here, `make` works
with no arguments.
