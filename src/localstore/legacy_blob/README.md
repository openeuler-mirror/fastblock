# legacy_blob (archived, not built)

SPDK blobstore-based localstore: `object_store`, `rolling_blob`, `blob_pool`,
`kv_checkpoint`, and the full `blob_manager` implementation.

**Production no longer compiles or links these files** (design v2 P4 physical
cut). The live tree uses AOS for object / log / kv.

Demos under `localstore/demo` and `raft/demo` that still call blob APIs are
disabled in CMake until rewritten for AOS.

Do not re-add these sources to `localstore/CMakeLists.txt` without an explicit
opt-in legacy target and demo migration plan.
