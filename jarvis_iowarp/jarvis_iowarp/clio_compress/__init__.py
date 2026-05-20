"""
IOWarp transparent compression service.

Deploys the clio_cte_compressor module as the entrypoint for CTE I/O so
that adapters (FUSE, ADIOS2, HDF5 VOL) hit compressor pool first and
data is compressed before being forwarded to the CTE core.
"""
