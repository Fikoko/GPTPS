# Getting GPTPS and its add-ons

Four ways in. Pick by how much build system you want to involve.

## 1. Amalgamation — no build system, no clone

Two files for the core, plus one `.c`/`.h` pair per add-on you want.

```sh
curl -LO https://github.com/Fikoko/GPTPS/releases/latest/download/gptps.c
curl -LO https://github.com/Fikoko/GPTPS/releases/latest/download/gptps.h
curl -LO https://github.com/Fikoko/GPTPS/releases/latest/download/gptps_pool.c
curl -LO https://github.com/Fikoko/GPTPS/releases/latest/download/gptps_pool.h

cc -std=c99 myapp.c gptps.c gptps_pool.c -I. -lpthread -ldl     # macOS: drop -ldl
```

Or generate them from a checkout:

```sh
sh tools/amalgamate.sh out --addons pool,durable_queue
sh tools/amalgamate.sh --list          # what is available
```

**`gptps.c` is byte-identical no matter which add-ons you select.** Its SHA256 is the
thing a Dockerfile or a vcpkg portfile pins, so one release must not produce different
bytes for one filename. Every asset is covered by the release's `SHA256SUMS`.

## 2. CMake, installed package

```cmake
find_package(gptps 1.0 REQUIRED COMPONENTS durable_queue pool)
target_link_libraries(myapp PRIVATE gptps::durable_queue gptps::pool)
```

Core only: drop `COMPONENTS` and link `gptps::gptps`. Asking for an add-on this install
does not have fails with a sentence naming what it *does* have — an install built with
`-DGPTPS_ADDONS=none` has none, and no install on Windows has `xport` (it needs `fork`
and `socketpair`).

## 3. CMake, `add_subdirectory` / `FetchContent`

A consumer gets the **library only** — zero CTest targets, zero add-on builds, no
example binaries. Opting in is two lines *before* `FetchContent_MakeAvailable`:

```cmake
set(GPTPS_BUILD_ADDONS ON)
set(GPTPS_ADDONS "durable_queue;pool")     # or "all"
```

## 4. pkg-config

```sh
cc -std=c99 myapp.c $(pkg-config --cflags --libs gptps-durable_queue gptps-pool) -o myapp
pkg-config --variable=plugindir gptps      # where a binary plug-in should install
```

The generated `.pc` files are **relocatable** — the prefix is derived from where the
file actually sits, not baked in at configure time. That matters because
`cmake --install --prefix` is honoured at *install* time: with a baked-in prefix the
two disagree the moment anyone stages an install, and every `-I` points somewhere that
does not exist.

---

## The install tree

```
include/gptps.h                     the public header
include/gptps/gptps_pool.h          add-on headers (namespaced directory)
lib/libgptps.a                      the core
lib/libgptps_pool.a                 one library per add-on
lib/cmake/gptps/                    find_package support
lib/pkgconfig/gptps.pc              + one .pc per add-on
lib/gptps/                          plugindir: where binary plug-ins go
bin/gptps_conformance               prove a plug-in before shipping it
```

## Build options

| Option | Default | Meaning |
|---|---|---|
| `GPTPS_BUILD_TESTS` | ON at top level | the CTest suite |
| `GPTPS_BUILD_EXAMPLES` | ON at top level | the example binaries |
| `GPTPS_BUILD_ADDONS` | ON at top level | build the add-on libraries |
| `GPTPS_ADDONS` | `all` | `all`, `none`, or a `;`-list |
| `GPTPS_ADDON_<NAME>` | from the list | per-add-on override, for packagers |
| `GPTPS_HAL_FAST` | OFF | adaptive mutexes on glibc — a latency knob |

A typo in `GPTPS_ADDONS` is a **hard error**, not an empty selection. A selector that
silently selects nothing reports success for work it never did.

## Static-only, on purpose

Add-on libraries are always `STATIC`, and `BUILD_SHARED_LIBS` does not change that. A
compiled-in module calls core symbols directly — that is what defines its tier — so a
shared add-on over a static core would carry its own copy of the core's file-statics:
two engines in one process. The `dlopen` tier is the supported way to get a binary
boundary, and it works precisely *because* a plug-in links no core symbols at all. See
[PLUGINS.md](PLUGINS.md).
