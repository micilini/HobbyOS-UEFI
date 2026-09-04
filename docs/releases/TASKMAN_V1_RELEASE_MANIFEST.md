# TASKMAN V1 release manifest

Status: CLOSED

| Item | Value |
|---|---|
| CL13 commit | `f9d47db7e4332fc2d88dce2842f997a9b3f82e57` |
| CL14 parent | `f9d47db7e4332fc2d88dce2842f997a9b3f82e57` |
| CL12/base | `4e700a9b38653fb57ecb0f107a18f87508db0736` |
| CL14 commit | the commit containing this manifest |
| Release tree hash | recorded in packaged `CL14_TREE.txt` and the post-commit receipt |
| CL13 certified `kernel.elf` | `118284945ca3b469cba586cf5334ddedd28fb73baec61f130784a5b9d719bc1e` |
| CL13 certified `hobbyos.img` | `46c36ec6298c533d82ddc122494dcb10953c4d60b08fa45ae85fe59036605105` |
| CL14 release `kernel.elf` | `9f8fcc9417e68118ef8fcde4c2af8f9dadd5eb138397b7e4ef15585e9463b973` |
| CL14 release `hobbyos.img` | `424b2b6656d9d8cf772bb173f4f99ccb347d1e005ef3cb1e0db4cb0d80c27b48` |
| SMP4/TCG soak | `fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8` |
| SMP8/KVM soak | `0431d80191c769121b1d22cc7c3dd5703c66a55a2faef5603a6a39794c69ab66` |
| SMP8 stage ledger | `e751035b1286724a6ef431a291a40271390eee969bf590ed885e8ed13c22dff7` |
| CL13 report | `9f8a396dce7855e9b88195fa5072e4ad76159a8fc8b8d2d7617ea6686d4b2819` |
| FIX15 report | `c209a5b00b8a8319d945fa9ecac391e6269f6ae57081dd5b14e169706c76e34b` |
| Runtime manifest digest | `db0e1c114a9ba5b8ddbde5f8310448de9fb43007e8e976e39646d268241bd50a` |
| Special manifest digest | `c39379719eb54d0463dd03831411952ab21db58f9ed98dfdf15fdc2c65bcbf12` |

The four before/focused/soak/build copies of each source manifest are byte
identical. The certified stack maximum is 1984 bytes and final `nm -u` is
empty. Toolchain evidence records GCC, GNU ld/objcopy, GNU-EFI, mtools, QEMU,
OVMF, and KVM access in the build logs.

Primary source documents are `docs/taskman-v1-*.md`. Evidence is under
`artifacts/build`; release payload is under `artifacts/release/TASKMAN_V1`.
The package contains only the selected binaries, docs, reports, source/tree
identifiers, and `SHA256SUMS`; it excludes `.git`, objects, transient QEMU
state, and unrelated historical artifacts.
