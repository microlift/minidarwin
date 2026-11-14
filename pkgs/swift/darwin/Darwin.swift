// What Apple's Darwin overlay defines beyond swift's Platform.swift, as far as
// minidarwin's overlays use it. Apple's is not released.

// <sys/mman.h>'s ((void *)-1), a macro Swift cannot import.
public let MAP_FAILED: UnsafeMutableRawPointer! = UnsafeMutableRawPointer(bitPattern: -1)
