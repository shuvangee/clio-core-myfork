"""CLI entry points for bundled IOWarp binaries.

Locates a named binary bundled in the wheel (under ``iowarp_core/bin/``)
and exec's it after ensuring IOWarp shared libraries are on the library
search path.  Each ``[project.scripts]`` entry in pyproject.toml routes
to a thin wrapper around ``_exec_iowarp_bin``.
"""

import importlib
import os
import sys


def _exec_iowarp_bin(name):
    """Find ``iowarp_core/bin/<name>``, prepend ``lib/`` to LD_LIBRARY_PATH,
    and replace the current process with it.

    In scikit-build-core editable installs, ``__file__`` resolves to the
    workspace source tree, but cmake-built artifacts (binaries, .so libs)
    live in ``site-packages/iowarp_core/``.  Anchor to site-packages by
    importing a cmake-built extension module — the editable finder always
    serves those from site-packages — and walking from its .so location.
    """
    bin_path = None
    lib_dir = None
    for _extmod in ("clio_cte_core_ext", "clio_cee"):
        try:
            _m = importlib.import_module(_extmod)
            _sp = os.path.dirname(os.path.abspath(_m.__file__))
            _candidate = os.path.join(_sp, "iowarp_core", "bin", name)
            if os.path.exists(_candidate):
                bin_path = _candidate
                lib_dir = os.path.join(_sp, "iowarp_core", "lib")
                break
        except (ImportError, AttributeError):
            continue

    if bin_path is None:
        # Fallback for regular (non-editable) installs where _cli.py lives
        # inside site-packages/iowarp_core/ itself.
        package_dir = os.path.dirname(os.path.abspath(__file__))
        bin_path = os.path.join(package_dir, "bin", name)
        lib_dir = os.path.join(package_dir, "lib")

    if not os.path.exists(bin_path):
        print(f"Error: {name} binary not found at {bin_path}", file=sys.stderr)
        sys.exit(1)

    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    if lib_dir not in ld_path:
        os.environ["LD_LIBRARY_PATH"] = (
            lib_dir + ":" + ld_path if ld_path else lib_dir
        )

    os.execve(bin_path, [bin_path] + sys.argv[1:], os.environ)


def main():
    """Entry point for the ``chimaera`` console script."""
    _exec_iowarp_bin("chimaera")


def cte_bench_main():
    """Entry point for the ``clio_cte_bench`` console script."""
    _exec_iowarp_bin("clio_cte_bench")


def run_thrpt_main():
    """Entry point for the ``clio_run_thrpt_bench`` console script."""
    _exec_iowarp_bin("clio_run_thrpt_bench")


if __name__ == "__main__":
    main()
