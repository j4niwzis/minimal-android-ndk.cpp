# minimal-android-ndk.cpp

Builds an Android toolchain out of Android's own sources. Nothing prebuilt
is downloaded: not the NDK archive, not the SDK command-line tools, not
Gradle. The sources of all of those are open -- what is not open is somebody
else's build of them, and this is the build instead. The host compiler,
CMake, Ninja, Python, Java, `aapt2` and `zipalign` come from the Linux
distribution; everything else is checked out at a pinned commit and built
here.

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

Nothing is fetched that a step does not need. Every repository is taken one
commit deep, without tags and without submodules, and one that names a
`paths` list is taken as a partial clone with a sparse checkout on top -- so
`frameworks/base` arrives as `core/res` and `native/android` rather than as
all of it, and `llvm-project` arrives as the runtimes and the CMake they
read rather than as the compiler, which comes from the distribution anyway.
Run with `--verbose` to see what each checkout weighed.

The tags in the manifest are a starting point rather than a promise: `pin`
resolves each one against its remote and says so when there is no such ref.

## Libraries

The toolchain is one thing and the libraries built on top of it are another.
This builds no library unless it is named, and fetches no repository for a
library it is not building.

```sh
minimal-android-ndk list                       # what can be built, and what each needs
minimal-android-ndk build --with skia          # and whatever skia needs
minimal-android-ndk build --with openal-soft --with libsndfile
minimal-android-ndk build --all                # everything in the manifest
```

`--with skia` pulls in zlib, libpng, libjpeg-turbo, libwebp and freetype,
because that is what Skia is configured against. Nothing else comes with it:
another renderer, another set of libraries, and none of these are built.

The list is the manifest, so adding a library is an entry in it rather than a
change to the program: where its source is, which builder it uses, what to
pass that builder, what it needs, and what has to be in the prefix when it is
done. A name that is not a package is refused rather than ignored.

## What it does so far## What it does so far

| step | |
| --- | --- |
| `sources` | check out every pinned repository |
| `ndk-compat` | native_app_glue and cpu-features where the builds look for them |
| `sysroot-headers` | assemble `usr/include` from the platform checkouts |
| `api-stubs` | generate the link stubs with `ndkstubgen` and verify them |
| `compiler-rt` | build the builtins libc++ is going to need |
| `runtimes` | build libc++, libc++abi and libunwind into the sysroot |
| `toolchain-file` | write the CMake toolchain file the dependencies use |
| `third-party` | build the libraries that were asked for, into the prefix |
| `apksigner` | build the signer, the one APK tool a distribution has not got |
| `framework-res` | link the platform's resource package |
| `apk` | build, package and sign an APK out of all of it |
| `check` | build something with the toolchain and read what came out |
| `hashes` | record the commits and the digests of what was produced |

Everything else an APK needs comes from the distribution -- `aapt2`,
`zipalign`, `keytool`, a Java runtime -- except the signer, which is an AOSP
project rather than a package. That one is built here, with `javac` and `jar`
and nothing else: no Gradle, no Maven, nothing downloaded. Its key management
implementations for Amazon and Google are left out, since they need SDKs that
are not here and are not wanted; the interfaces they implement stay, because
the signer names them itself.

The toolchain proper -- everything a library needs before it can be built at
all -- is the first six steps:

```sh
minimal-android-ndk build toolchain-file
```

which pulls in the sources, the headers, the stubs, the builtins and the
runtimes on the way. After that the target has a compiler, a sysroot to
compile against and a C++ standard library, including the module sources and
the `libc++.modules.json` that `import std` needs.

**Skia, when it is asked for, is given the libraries built here.** zlib, libpng, libjpeg-turbo,
libwebp and freetype are built for the target like every other dependency,
and Skia is configured with `skia_use_system_*` so it links those instead of
the copies in its own `third_party/externals`. Its dependency sync is then
not needed at all, and the bundled zlib stops being a problem -- that copy is
Chromium's, and its `zconf.h` includes a `chromeconf.h` which is not part of
zlib. Skia's build files are used unpatched: `ndk-compat` is an NDK-shaped
tree of symlinks pointing at the host compiler and at the sysroot built here,
which is what Skia's Android toolchain expects to find. After `gn gen` the
tool asks GN what the arguments actually came out as, because an argument
that is misspelled or overridden still looks right in a copied command line.

## Three things worth knowing

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
