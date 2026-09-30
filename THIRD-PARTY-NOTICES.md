# Third-Party Notices

DxTimingCaptureLibrary is licensed under the MIT License (see `LICENSE`). It
incorporates or depends on the components below. Exact source revisions and
restored package versions are recorded in `cgmanifest.json`.

## WinPixEventRuntime decoder headers

The headers in `third_party/PixEventDecoder/include/` come from
[Microsoft PixEvents](https://github.com/microsoft/pixevents) commit
`3a7e70dde7bf54f02f9d2e9dd6d3350c6cfb962f`.

Copyright (c) Microsoft Corporation.

License: MIT. The applicable license text is reproduced in the repository root
`LICENSE` file.

Only the decoder headers are included. Standalone builds use
`lib/PixEventDecoderStub.cpp` rather than the upstream decoder implementation.

## DXGK ETW manifest definitions

`third_party/dxgk/lddmcoreeventdefs.h` contains the subset of DXGK /
LDDMCore provider constants used by the library. The values originate from
`LDDMCore.man`, distributed with GPUView in the Windows Performance Toolkit.

Copyright (c) Microsoft Corporation.

License: MIT. The applicable license text is reproduced in the repository root
`LICENSE` file.

## DirectStorage ETW definitions

`third_party/directstorage/dstorage_etw.h` contains the subset of
DirectStorage ETW provider definitions used by the library. These definitions
are not included in the public `Microsoft.Direct3D.DirectStorage` NuGet
package.

Copyright (c) Microsoft Corporation.

License: MIT. The applicable license text is reproduced in the repository root
`LICENSE` file.

## GoogleTest

GoogleTest v1.8.1 is vendored under `third_party/googletest/` for tests.

- Project: https://github.com/google/googletest
- Commit: `2fe3bd994b3189899d93f1d5a881e725e046fdc2`
- License: BSD 3-Clause
- License text: `third_party/googletest/LICENSE`

Copyright 2008, Google Inc. All rights reserved.

## Perfetto

The Perfetto C++ tracing SDK v56.1 is vendored as generated amalgamated sources
under `third_party/perfetto/sdk/`.

- Project: https://github.com/google/perfetto
- Commit: `c794fceabe584dc9172e5512aaaeecc21019a635`
- Source artifact:
  https://github.com/google/perfetto/releases/download/v56.1/perfetto-cpp-sdk-src.zip
- License: Apache License 2.0
- License text: `third_party/perfetto/LICENSE`

The Perfetto amalgamation contains code inspired by OpenBSD `tree.h`:

```text
$OpenBSD: tree.h,v 1.31 2023/03/08 04:43:09 guenther Exp $

Copyright 2002 Niels Provos <provos@citi.umich.edu>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Dear ImGui and stb

Dear ImGui v1.91.8 is vendored under `third_party/imgui/` for the memory-map
sample.

- Project: https://github.com/ocornut/imgui
- Commit: `dbb5eeaadffb6a3ba6a60de1290312e5802dba5a`
- License: MIT
- License text: `third_party/imgui/LICENSE.txt`

The bundled `imstb_rectpack.h`, `imstb_textedit.h`, and `imstb_truetype.h`
include software by Sean Barrett, copyright (c) 2017, offered under either the
MIT License or the public-domain dedication reproduced at the end of each
header. This distribution uses the MIT option.

`imgui_draw.cpp` embeds Japanese glyph range data derived from the Joyo and
Jinmeiyo kanji lists published by Japan's Agency for Cultural Affairs and
Ministry of Justice. That data is licensed under the Creative Commons
Attribution 4.0 International License
(https://creativecommons.org/licenses/by/4.0/legalcode). The data was generated
with https://github.com/vaiorabbit/everyday_use_kanji.

## Restored Microsoft packages and Windows SDK

The build restores these packages under their own Microsoft license terms:

- `Microsoft.Direct3D.D3D12` 1.619.5
- `Microsoft.Direct3D.DirectStorage` 1.3.0

They are development dependencies and are not relicensed under this
repository's MIT License. The library also compiles against Windows SDK headers.
The repository does not redistribute those restored packages or Windows SDK
headers.
