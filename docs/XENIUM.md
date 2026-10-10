# Xenium dependency

Xenium supports the general ready-job queue and immutable snapshot publication.
Dense ECS component storage does not use its hash map or LFRC reclamation.

Xenium is an official Git submodule at `third_party/xenium`, pinned by this
repository's gitlink to commit `1c449ae953ce2a440b0d16c5ed1181d2754860ab`.
The complete upstream license is in `third_party/xenium/LICENSE`; the core
notice is retained in `third_party/NOTICES.txt`.

The core applies `dependencies/patches/xenium/wasm32.patch` to the clean
submodule checkout. It adjusts tagged pointers and rotation for 32-bit pointer
widths and supplies a monotonic clock source on WASM. Native 64-bit tagged
pointer behavior remains unchanged. The patch only modifies
`xenium/marked_ptr.hpp` and `xenium/utils.hpp`.

Run `make deps` to initialize Xenium and apply the patch. Run `make deps-check`
to verify the local checkout offline. Both commands use the pinned gitlink;
the build never fetches dependencies implicitly. Optional upstream test
submodules are not initialized.
