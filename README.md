# minimal-android-ndk.cpp

Builds an Android toolchain out of Android's own sources: no Gradle, no
proprietary SDK command-line tools, no NDK binary distribution. The host
compiler, CMake, Ninja, Python, Java, `aapt2` and `zipalign` come from the
Linux distribution; everything else is checked out at a pinned commit and
built here.

It targets one ABI at a time. The tree it produces is the one described in
[osu-cpp's Android notes][notes], so a tree assembled by hand and a tree
built by this tool are the same tree.

[notes]: https://github.com/j4niwzis/osu-cpp/blob/android/android/BUILDING_WITHOUT_OFFICIAL_NDK.md

## Building the tool

It is C++23 modules against `import std`, so it needs CMake 4.2 or newer,
a Clang new enough to ship `libc++.modules.json`, and **the Ninja generator**
-- the Makefile generators do not scan modules.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Using it

```sh
minimal-android-ndk pin --platform android-15.0.0_r1  # resolve every source to a commit
minimal-android-ndk plan                              # what will run, in what order
minimal-android-ndk build                             # run it
minimal-android-ndk env                               # the flags to configure against the result
```

`pin` resolves AOSP repositories through the release manifest rather than by
tag. A platform tag names a commit for every project the manifest carries,
and that is the only thing the rest of the tool accepts -- the tag itself
need not exist in each repository, which is what makes
`git clone -b android-14.0.0_r75 platform/ndk` fail.

Nothing is checked out that a step does not need: repositories with a
`paths` list are fetched as a partial clone with a sparse checkout.

## What it does so far

| step | |
| --- | --- |
| `sources` | check out every pinned repository |
| `ndk-compat` | native_app_glue and cpu-features where the builds look for them |
| `sysroot-headers` | assemble `usr/include` from the platform checkouts |
| `api-stubs` | generate the link stubs with `ndkstubgen` and verify them |
| `hashes` | record the commits and the digests of what was produced |

`minimal-android-ndk plan` also lists the steps that are declared and not implemented
yet: `compiler-rt`, `runtimes`, `third-party`, `skia`, `framework-res` and
`apksigner`. They refuse to run rather than reporting a success they did not
have.

## Two things worth knowing

**Header paths are searched for, not remembered.** A rule says
`libc/kernel/uapi`, not a full path, and the checkout is walked for a
directory whose path ends that way. Modern AOSP does not have
`bionic/libc/arch-arm64/include` or `frameworks/native/include` where the old
instructions put them, and a rule written against those copies nothing
without saying so. Every rule also states what must exist afterwards, so a
rule that matched the wrong directory is an error rather than a compiler
failure a hundred steps later.

**The API stubs are generated, not faked.** An empty shared object lets
`-lfoo` succeed and leaves every platform call undefined; a fabricated
`libc.so` cannot even link libc++, which needs `malloc`, `free` and the
pthread functions to be present. Each stub is generated from the platform's
own symbol map by Soong's `ndkstubgen` and then read back with `readelf` to
confirm that the symbols and the `SONAME` arrived. They are link-time stubs:
Android supplies the real libraries, and none of them belongs in an APK.

The C++ inside is in namespace `mandk`, and its modules are `mandk.*`: a
dash cannot appear in an identifier.

## Licence

AGPL-3.0-or-later. See [LICENSE](LICENSE).
