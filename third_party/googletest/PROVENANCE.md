This is a trimmed copy of googletest v1.8.1:
https://github.com/google/googletest/releases/tag/release-1.8.1

Only what the tests build is kept: googletest/include, googletest/src, and the
license and contributor files. Google Mock, the upstream build systems (CMake,
Bazel, autotools, legacy MSVC projects), docs, samples, and googletest's own
tests were removed. The full upstream tree is available at the link above.

googletest_main.vcxproj is a custom project derived from the ones CMake
generates; it compiles googletest/src/gtest-all.cc into a static library.

If you update googletest, update cgmanifest.json in the repository root and
THIRD-PARTY-NOTICES.md accordingly.
