#pragma once

#include <cstdint>

// Deterministic syscall failure seam used by atomic-save regression tests.
enum class AtomicSaveFailurePoint : uint8_t {
    None,
    TempOpen,
    Write,
    FileFsync,
    Rename,
    DirectoryOpen,
    DirectoryFsync,
};
