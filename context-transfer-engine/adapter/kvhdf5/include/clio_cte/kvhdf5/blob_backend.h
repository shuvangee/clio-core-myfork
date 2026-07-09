#pragma once

#include "defines.h"
#include <cuda/std/span>
#include <cuda/std/expected>
#include <cuda/std/concepts>

namespace kvhdf5 {

enum class BlobError : uint8_t { NotFound, NotEnoughSpace, BackendFailure };

// A chunk store keyed by a host-formatted blob name (chunk-coord text). The
// value is raw chunk bytes. InMemBlobBackend satisfies this for runtime-free
// CPU tests; GpuCteBlobStore (Slice 2) is the real iowarp producer path.
template<typename T>
concept BlobBackend = requires(T s,
        cstd::span<const char> name,
        cstd::span<const byte_t> in,
        cstd::span<byte_t> out) {
    { s.WriteChunk(name, in) } -> cstd::same_as<bool>;
    { s.ReadChunk(name, out) } -> cstd::same_as<cstd::expected<cstd::span<byte_t>, BlobError>>;
    { s.ChunkExists(name) } -> cstd::same_as<bool>;
};

}  // namespace kvhdf5
