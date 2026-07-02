# CLIO Runtime Modules (ChiMods) Directory
# This directory contains standard CLIO Runtime modules

cmake_minimum_required(VERSION 3.10)

message(STATUS "Building Clio modules (ChiMods)...")

# Add individual module subdirectories
add_subdirectory(MOD_NAME)  # Test module for Wait functionality

# Add admin module (critical system module)
add_subdirectory(admin)

# Add bdev module (block device operations)
add_subdirectory(bdev)

# Add safe-bdev module (declustered, erasure-coded block device)
# TEMPORARILY DISABLED (PR #663 WIP): the safe-bdev Runtime + autogen still use
# the pre-migration Container task API (FullPtr<Task>+RunContext) and must be
# ported to dev's shared_ptr<Task> API (Run/SaveTask/LoadTask/NewTask/…, DelTask
# removed) + autogen regeneration before it will build. All other 543 work is
# integrated and compiles; re-enable once the runtime port lands.
option(CLIO_ENABLE_SAFE_BDEV "Build the safe-bdev EC module (WIP: needs Container-API port)" OFF)
if(CLIO_ENABLE_SAFE_BDEV)
  add_subdirectory(safe-bdev)
endif()

# Additional modules can be added here as they are developed
# add_subdirectory(other_module_name)