# Chimaera Runtime

<p align="center">
  <strong>High-Performance Modular Runtime for Scientific Computing and Storage Systems</strong>
  <br />
  <br />
  <a href="#getting-started">Getting Started</a> ·
  <a href="#external-integration">External Integration</a> ·
  <a href="#chimod-development">Module Development</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="#contributing">Contributing</a>
</p>

---

**Chimaera** is a high-performance, distributed task execution runtime designed for scientific computing, storage systems, and near-data processing applications. Built with a modular architecture, Chimaera enables developers to create custom processing modules (ChiMods) that can be dynamically loaded and executed with minimal overhead.

## What is Chimaera?

Chimaera provides:
- **High-Performance Task Execution**: Coroutine-based task scheduling with microsecond-level latencies
- **Modular Architecture**: Extensible Module system for custom functionality
- **Advanced Synchronization**: CoMutex and CoRwLock for coroutine-aware synchronization
- **Distributed Computing**: Seamless scaling from single node to cluster deployments
- **Storage Integration**: Built-in support for block devices, file systems, and custom storage backends
- **Memory Management**: Shared memory IPC with ClioCtp for optimal performance

## Key Features

- 🚀 **Ultra-High Performance**: Coroutine-based execution with shared memory IPC for microsecond-level task latencies
- 🧩 **Modular Module System**: Dynamically loadable modules for custom functionality without core modifications
- 🔄 **Advanced Task Management**: Asynchronous and fire-and-forget task execution patterns
- 🔐 **Coroutine-Aware Synchronization**: CoMutex and CoRwLock primitives for deadlock-free coordination
- 💾 **Flexible Storage Backends**: Built-in support for RAM, file-based, and custom block device operations
- 🌐 **Distributed Architecture**: Seamless scaling from development to production clusters
- 🔧 **Developer-Friendly**: Comprehensive APIs, extensive documentation, and external project integration

## Architecture

Chimaera follows a modular semi-microkernel design:

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│  Client App 1   │    │  Client App 2   │    │  External App   │
├─────────────────┤    ├─────────────────┤    ├─────────────────┤
│ Module Clients  │    │ Module Clients  │    │ Module Clients  │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 │
                    ┌─────────────────┐
                    │ Chimaera Runtime │
                    │                 │
                    │  Core Services: │
                    │  • IPC Manager  │
                    │  • Work Orch.   │
                    │  • Pool Manager │
                    │  • Module Mgr   │
                    └─────────────────┘
                                 │
            ┌────────────────────┼────────────────────┐
            │                    │                    │
    ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
    │ Admin Module  │   │ Bdev Module   │   │ Custom Module │
    │ (Pool Mgmt)   │   │ (Block I/O)   │   │ (User Logic)  │
    └───────────────┘   └───────────────┘   └───────────────┘
```

**Core Components:**
- **Runtime Process**: Central coordinator managing resources and ChiMods
- **ChiMods**: Dynamically loaded modules providing specialized functionality
- **Client Libraries**: Lightweight interfaces for application integration
- **Shared Memory IPC**: High-performance inter-process communication via ClioCtp

## Getting Started

### Prerequisites

Chimaera requires the following dependencies:

**System Requirements:**
- C++17 compatible compiler (GCC >= 9, Clang >= 10)
- CMake >= 3.10
- Linux (Ubuntu 20.04+, CentOS 8+, or similar)

Our docker container has all dependencies installed for you.
```bash
docker pull iowarp/iowarp-build:latest
```

### Installation

#### 1. Clone and Build

```bash
# Clone the repository
git clone https://github.com/iowarp/iowarp-runtime.git
cd iowarp-runtime

# Configure release with CMake preset
cmake --preset release

# Build all components
cmake --build build --parallel $(nproc)

# Install to system or custom prefix
cmake --install build --prefix /usr/local
```

#### 2. Quick Start Example

Create a simple application using the bdev Module:

```cpp
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/bdev/bdev_client.h>
#include <clio_runtime/admin/admin_client.h>

int main() {
  // Initialize Chimaera (client mode with embedded runtime)
  chi::CHIMAERA_INIT(chi::ChimaeraMode::kClient, true);

  // Create admin client (always required)
  clio::run::admin::Client admin_client(chi::PoolId(7000, 0));
  admin_client.Create(chi::PoolQuery::Local());
  
  // Create bdev client for high-speed RAM storage
  clio::run::bdev::Client bdev_client(chi::PoolId(8000, 0));
  bdev_client.Create(chi::PoolQuery::Local(), 
                    clio::run::bdev::BdevType::kRam, "", 1024*1024*1024); // 1GB RAM
  
  // Allocate and use a block
  auto block = bdev_client.Allocate(4096);  // 4KB block
  std::vector<ctp::u8> data(4096, 0xAB);
  bdev_client.Write(block, data);
  auto read_data = bdev_client.Read(block);
  bdev_client.Free(block);
  
  return 0;
}
```

## External Integration

Chimaera is designed to be easily integrated into external projects through its CMake export system.

### For External C++ Projects

**CMakeLists.txt Example:**
```cmake
cmake_minimum_required(VERSION 3.10)
project(MyChimaeraApp)

# Find Chimaera packages
find_package(chimaera-core REQUIRED)
find_package(chimaera-admin REQUIRED)
find_package(chimaera-bdev REQUIRED)  # Optional: for block device operations

# Create your application
add_executable(my_app src/main.cpp)

# Link against Chimaera libraries
target_link_libraries(my_app
  clio::run::cxx              # Core Clio runtime
  clio::run::admin_client     # Admin module (always required)
  clio::run::bdev_client      # Block device operations
  ${CMAKE_THREAD_LIBS_INIT}  # Threading support
)
```

**Build Configuration:**
```bash
# Set CMAKE_PREFIX_PATH to include Chimaera installation
export CMAKE_PREFIX_PATH="/usr/local:/path/to/chimaera/install"

mkdir build && cd build
cmake ..
make
```

### Available ChiMods

| Module | Purpose | CMake Package | Description |
|--------|---------|---------------|--------------|
| **admin** | Core Management | `chimaera-admin` | Pool creation and system administration (always required) |
| **bdev** | Block I/O | `chimaera-bdev` | High-performance block device operations with RAM/file backends |
| **MOD_NAME** | Template | `chimaera-MOD_NAME` | Example Module template for custom development |

### Runtime vs Client Mode

Chimaera applications can run in two modes:

**Client Mode with Embedded Runtime** (Most Common):
```cpp
// Initialize as client with embedded runtime - starts runtime and connects client
chi::CHIMAERA_INIT(chi::ChimaeraMode::kClient, true);
```

**Client-Only Mode** (Advanced - requires external runtime):
```cpp
// Initialize as client only - connects to existing external runtime
chi::CHIMAERA_INIT(chi::ChimaeraMode::kClient, false);
```

**Runtime/Server Mode** (Advanced - embedded applications):
```cpp
// Initialize as runtime/server - starts runtime only (no client)
chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer, false);
```

## Module Development

The true power of Chimaera lies in developing custom ChiMods. Each Module is a self-contained module providing specialized functionality.

### Module Structure

```
modules/my_module/
├── clio_mod.yaml           # Module metadata
├── CMakeLists.txt              # Build configuration
├── doc/                        # Documentation
│   ├── my_module.md           # API reference
│   └── integration.md         # Integration guide
├── include/chimaera/my_module/ # Headers
│   ├── my_module_client.h     # Client interface
│   ├── my_module_runtime.h    # Runtime implementation
│   └── my_module_tasks.h      # Task definitions
└── src/                        # Implementation
    ├── my_module_client.cc    # Client code
    └── my_module_runtime.cc   # Runtime code
```

### Key Development Concepts

**1. Task-Based Architecture:**
```cpp
// Define custom tasks
struct MyCustomTask : public chi::Task {
  chi::u64 input_data_;
  chi::u64 result_;        // Output parameter
  chi::u32 result_code_;   // Error code
};
```

**2. Client Interface:**
```cpp
class Client : public chi::ContainerClient {
public:
  // Synchronous operations
  chi::u64 ProcessData(const ctp::ipc::MemContext& mctx, chi::u64 data);
  
  // Asynchronous operations
  ctp::ipc::FullPtr<MyCustomTask> AsyncProcessData(const ctp::ipc::MemContext& mctx, chi::u64 data);
};
```

**3. Runtime Implementation:**
```cpp
class Runtime : public chi::Container {
public:
  void ProcessData(ctp::ipc::FullPtr<MyCustomTask> task, chi::RunContext& ctx) {
    // Implement your logic here
    task->result_ = process_algorithm(task->input_data_);
    task->result_code_ = 0;  // Success
  }
};
```

## Documentation

Comprehensive documentation is available in the `docs/` directory:

- **[MODULE_DEVELOPMENT_GUIDE.md](docs/MODULE_DEVELOPMENT_GUIDE.md)**: Complete guide for developing ChiMods
- **[Module Documentation](modules/)**: Individual Module API references:
  - [Admin Module](modules/admin/doc/admin.md): Core system management
  - [Bdev Module](modules/bdev/doc/bdev.md): Block device operations
  - [MOD_NAME Template](modules/MOD_NAME/doc/MOD_NAME.md): Development template

### Testing

Chimaera includes comprehensive test suites:

```bash
# Run all unit tests
ctest --test-dir build

# Run specific test suites
./build/bin/chimaera_bdev_chimod_tests      # Block device tests
./build/bin/chimaera_comutex_tests          # Synchronization tests
./build/bin/chimaera_task_archive_tests     # Serialization tests
```

## Contributing

We welcome contributions to the Chimaera project!

### Development Workflow

1. **Fork** the repository
2. **Create** a feature branch: `git checkout -b feature/amazing-feature`
3. **Follow** the coding standards in [CLAUDE.md](CLAUDE.md)
4. **Test** your changes: `ctest --test-dir build`
5. **Submit** a pull request

### Coding Standards

- Follow Google C++ Style Guide
- Use semantic naming for queue IDs and priorities
- Never use null pool queries - always use `chi::PoolQuery::Local()`
- Implement proper monitor methods with `route_lane_` assignment
- Add comprehensive unit tests for new functionality

## Performance Characteristics

Chimaera is designed for high-performance computing scenarios:

- **Task Latency**: < 10 microseconds for local task execution
- **Memory Bandwidth**: Up to 50 GB/s with RAM-based bdev backend
- **Scalability**: Single node to multi-node cluster deployments
- **Concurrency**: Thousands of concurrent coroutine-based tasks
- **I/O Performance**: Native async I/O with libaio integration

## Use Cases

**Scientific Computing:**
- High-performance data processing pipelines
- Near-data computing for large datasets
- Custom storage engine development

**Storage Systems:**
- Distributed file system backends
- Object storage implementations
- Cache and tiered storage solutions

**Real-Time Applications:**
- Low-latency data processing
- Streaming analytics
- Edge computing deployments

## License

Chimaera is licensed under the **BSD 3-Clause License**. See source files for complete license text.

---

## Acknowledgements

Chimaera is developed at the [GRC lab](https://grc.iit.edu/) at Illinois Institute of Technology as part of the IOWarp project. This work is supported by the National Science Foundation (NSF) and aims to advance next-generation scientific computing infrastructure.

For more information about IOWarp: https://grc.iit.edu/research/projects/iowarp
