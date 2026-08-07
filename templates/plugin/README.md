# GPTPS plug-in template

A complete, buildable binary plug-in. Copy this directory, rename, edit.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=$(pkg-config --variable=prefix gptps)
cmake --build build
gptps_conformance build/myplugin.so          # prove it before shipping
```

Load it by naming its path — from a config file:

```toml
addons = ["/usr/local/lib/gptps/myplugin.so"]
```

or from the host: `gptps_load_addon(engine, "/path/to/myplugin.so")`.

**There is no search path, and that is deliberate.** `docs/SECURITY.md` is explicit
that an add-on path is code: whoever can write the config can execute code in your
process. Scanning a directory would widen that quietly, so GPTPS makes you name the
file.

Where to install: `pkg-config --variable=plugindir gptps`.

See [`docs/PLUGINS.md`](../../docs/PLUGINS.md) for the full contract — which tier you
want, the `struct_size` guard, threading per seam, and what the core does when your
`setup()` fails.
