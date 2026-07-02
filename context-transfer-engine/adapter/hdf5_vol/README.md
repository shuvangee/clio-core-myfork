# iowarp HDF5 VOL connector

A pass-through HDF5 VOL connector that transparently caches HDF5 data in the
clio-core CTE (Context Transfer Engine) while preserving native HDF5 file
semantics. It wraps a native under-VOL: every operation delegates to native, and
whole-dataset `H5Dwrite`/`H5Dread` transfers of flat datatypes are additionally
mirrored to CTE via async `PutBlob`/`GetBlob`. Loaded transparently through the
standard HDF5 plugin mechanism — no application source changes.

The native file is always written synchronously and remains authoritative;
variable-length/reference datatypes and partial (hyperslab/point) selections
delegate to native and are not CTE-cached. **Safe mode:** `H5Fflush` and `H5Fclose`
drain all in-flight async CTE puts before returning, so a successful flush/close is
a real durability barrier for the cache path (no async write outlives it).

Files: `iowarp_vol.cc`, `iowarp_vol.h`. Connector name: **`iowarp`**.

## Enabling and building

The adapter is OFF by default (`CLIO_CTE_ENABLE_HDF5_VOL`, see the top-level
`CMakeLists.txt`). Enable and build it:

```bash
cmake -S <repo> -B <build> -DCLIO_CTE_ENABLE_HDF5_VOL=ON
cmake --build <build> --target iowarp_hdf5_vol -j
# -> <build>/bin/libiowarp_hdf5_vol.so
```

**HDF5 linkage matters.** A VOL connector is ABI-coupled to the `libhdf5` of the
application that `dlopen`s it. The `.so` must link the *same* HDF5 the loading app
(and `h5dump`/`h5ls`) use, or HDF5 will refuse to load it or mismatch the VOL ABI.
Confirm with `ldd <build>/bin/libiowarp_hdf5_vol.so | grep hdf5`. The connector
requires HDF5 >= 1.14 and does not require ELF support (unlike the VFD adapter).

## Using it

```bash
export HDF5_PLUGIN_PATH=<build>/bin
export HDF5_VOL_CONNECTOR=iowarp        # under-VOL defaults to native
# a clio_run runtime must be reachable for the CTE cache path
```
Any HDF5 application (h5py, C, tools) then routes through the connector. Files it
writes are valid native HDF5 files readable by standard tools.

## Testing and current status

Compatibility is verified by a differential suite (native VOL as the oracle):
each feature case is written/read through the connector and compared to native,
plus `h5diff`/`h5dump`. It is registered as the CTest test
`cte_hdf5_vol_compat_suite` (gated on `CLIO_CTE_ENABLE_HDF5_VOL`; `RUN_SERIAL`
because it manages its own `clio_run`).

```bash
ctest -R cte_hdf5_vol_compat_suite --output-on-failure
# or directly:
python3 benchmarks/hdf5-ingest/vol_compat_suite.py --bin <build>/bin
```

The test runs in **honest mode** (no `--expect-fail` allowlist): it reports the
connector's real compatibility state rather than masking gaps, so any future
regression fails the test. For the authoritative, always-current status run the
test; do not rely on numbers copied into prose. As of this writing it is **green
(18/18)** — compatible for dataset I/O (signed/unsigned int, float, compound, enum,
array-element, scalar, and variable-length string datatypes; contiguous and
chunked; shuffle/fletcher32 filters; whole-dataset, hyperslab, and point
selections; extendible datasets with reopen+append), attributes, group/link
structure, modern-API iteration (C `H5Ovisit3`/`H5Literate2`), and Safe-mode
`H5Fflush` durability.

Two feature areas are exercised only through the C API (h5py has poor non-native
VOL support): iteration (`vol_c_iteration_test.c`) and Safe-mode flush
(`vol_c_safeflush_test.c`), both run automatically by the suite.

See `benchmarks/hdf5-ingest/VOL_COMPAT_FINDINGS.md` for the detailed matrix,
method, and the vlen/Safe-mode implementation notes.
