# iowarp HDF5 VOL connector

A pass-through HDF5 VOL connector that transparently caches HDF5 data in the
clio-core CTE (Context Transfer Engine) while preserving native HDF5 file
semantics. It wraps a native under-VOL: every operation delegates to native, and
whole-dataset `H5Dwrite`/`H5Dread` transfers are additionally mirrored to CTE via
async `PutBlob`/`GetBlob`. Loaded transparently through the standard HDF5 plugin
mechanism — no application source changes.

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

**The test runs in honest mode and currently FAILS** — that is intentional: it
reports the connector's real compatibility state rather than masking gaps. For the
authoritative, always-current status run the test; do not rely on numbers copied
into prose. As of this writing it is broadly compatible for dataset I/O (signed/
unsigned int, float, compound, enum, array-element, scalar datatypes; contiguous
and chunked; shuffle/fletcher32 filters; whole-dataset, hyperslab, and point
selections; extendible datasets with reopen+append) with these **known gaps**:

- **string datatypes (write)** — VOL-written string datasets are malformed
- **attributes (write)** — VOL-written files carrying attributes are malformed
- **groups/links (write fidelity)** — structure differs from native
- **object iteration** — `visititems` / `H5Ovisit` / `H5Literate` fails

See `benchmarks/hdf5-ingest/VOL_COMPAT_FINDINGS.md` for the detailed matrix and
method. Closing these gaps is the current VOL work; each removes one failing case
from the suite until it goes green.
