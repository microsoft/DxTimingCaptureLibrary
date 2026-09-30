The Perfetto C++ tracing SDK, amalgamated into two files (perfetto.h and
perfetto.cc) under sdk/. Copied from the perfetto-cpp-sdk-src.zip artifact of a
Perfetto GitHub release:

  https://github.com/google/perfetto  (release v56.1)
  https://github.com/google/perfetto/releases

The amalgamated SDK is no longer checked into the Perfetto git tree; it is
produced by `tools/gen_amalgamated` and published on the release pages.

Used by sample.perfetto to write D3D12 ETW events into a Perfetto trace.
License: Apache-2.0 (see LICENSE). The amalgamation includes code derived from
OpenBSD tree.h; its BSD attribution is reproduced in THIRD-PARTY-NOTICES.md.
If you update these files, update cgmanifest.json and the notices accordingly.
