# Third-party notices

StudyBoard is licensed under the MIT License (see [LICENSE](LICENSE)). It uses the
following third-party software, each under its own license. Versions are pinned in
[`cmake/Dependencies.cmake`](cmake/Dependencies.cmake).

| Component | Version | License | Used for | Shipped in the application? |
|---|---|---|---|---|
| [Qt](https://www.qt.io/) | 6.8 (LTS) | LGPL-3.0 (open-source edition; commercial licenses also exist) | UI, platform integration, OpenGL context | Yes — dynamically linked, deployed as shared libraries |
| [SQLite](https://sqlite.org/) | 3.50.4 | Public domain | Persistence | Yes — statically linked (unless `STUDYAPP_USE_SYSTEM_SQLITE=ON`) |
| [tl::expected](https://github.com/TartanLlama/expected) | 1.1.0 | CC0-1.0 | `Result<T>` in `core` | Yes — header-only |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | BSD-3-Clause | Unit tests | No — test builds only |
| [Google Benchmark](https://github.com/google/benchmark) | 1.9.1 | Apache-2.0 | Micro-benchmarks (`bench/`) | No — optional developer builds only |

## Qt (LGPL-3.0)

StudyBoard links Qt dynamically, so users can replace the Qt libraries shipped with the
application with their own builds, as the LGPL requires. Qt's source code is available
from <https://download.qt.io/> and <https://code.qt.io/>. Installed builds include Qt's
license texts via Qt's deployment tooling.

## tl::expected (CC0-1.0)

Written by Sy Brand. Dedicated to the public domain under CC0 1.0 Universal
(<https://creativecommons.org/publicdomain/zero/1.0/>).

## SQLite (public domain)

The SQLite amalgamation is in the public domain (<https://sqlite.org/copyright.html>).

## GoogleTest (BSD-3-Clause)

Copyright 2008, Google Inc. All rights reserved. Used only to build and run tests; not
distributed with the application.

## Google Benchmark (Apache-2.0)

Copyright 2015 Google Inc. Licensed under the Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>). Fetched only when
`STUDYAPP_BUILD_BENCHMARKS=ON` to build the developer micro-benchmarks; not distributed
with the application.
