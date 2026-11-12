# Install

stest builds your firmware into a host executable, so it needs a host toolchain: upstream LLVM
(clang plus the LLVM development package, for the pass plugin), CMake and Ninja. The test side
needs Python and [uv](https://docs.astral.sh/uv/).

| Requirement | Version | Notes |
|---|---|---|
| LLVM (clang, `llvm-config`, the LLVM CMake package) | 18 or newer | Upstream LLVM only. Apple clang cannot load pass plugins and is rejected. |
| CMake | 3.23 or newer | |
| Ninja | any recent | |
| Python | 3.11 or newer | |
| uv | any recent | Creates the test environment and runs pytest. |

Supported hosts: macOS arm64, and Linux x86-64 or arm64. On Windows, use the Docker image or the
dev container (see the Windows tab below).

## Install the toolchain

=== "macOS"

    ```sh
    brew install llvm cmake ninja uv
    ```

    Homebrew's LLVM is keg-only (not on `PATH`). That is fine: stest finds it through
    `brew --prefix llvm`. Xcode or the Command Line Tools must be installed, because stest
    links with Xcode's `ld` (see [Install failures](#install-failures)).

=== "Ubuntu 24.04"

    ```sh
    sudo apt install clang-18 llvm-18-dev lld-18 cmake ninja-build python3
    curl -LsSf https://astral.sh/uv/install.sh | sh    # or: pipx install uv
    export STEST_LLVM_ROOT=/usr/lib/llvm-18
    ```

    Debian's LLVM lives under `/usr/lib/llvm-18`. Put the `export` in your shell profile, or
    pass `-DSTEST_LLVM_ROOT=/usr/lib/llvm-18` to the first `cmake` configure instead (it is
    then remembered in the build directory's cache).

=== "Windows / Docker"

    stest does not run natively on Windows. Use the Docker image, which is what CI uses on
    Linux: Ubuntu 24.04 with LLVM 18, CMake, Ninja, Python and uv, and `STEST_LLVM_ROOT`
    already set.

    ```sh
    docker build -t stest-ci docker
    docker run --rm -v "$PWD":/w -w /w stest-ci bash -euxo pipefail -c '
      cmake -S . -B build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release
      cmake --build build-docker
    '
    ```

    Run it from the stest checkout. Use a separate build directory (`build-docker` here) if you
    also build natively from the same tree: the container's build is Linux-only.

    For an editor that works inside the image, open the repository in the **dev container**
    (`.devcontainer/devcontainer.json`, with VS Code's Dev Containers extension or any tool
    that reads that file). It builds the same image and mounts the checkout at `/w`; every
    command on these pages then works unchanged in its terminal.

## Build stest and run its tests

From a clone of stest:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
uv run pytest python/tests -q
```

These are the commands CI runs on macOS and inside the Docker image. The first configure
downloads ST's STM32L4 HAL, the CMSIS device files and CMSIS-Core at pinned commits (checked by
SHA-256), so it needs network access once.

`ctest` runs the C++ unit tests, the self-test firmware checks and the Python tests (it creates
its own virtual environment under `build/`). `uv run pytest python/tests -q` runs the Python
tests alone; uv creates `.venv/` in the checkout on first use.

## Verify the install

If `ctest` reports `100% tests passed`, the toolchain, the pass plugin, the runtime and the
Python package all work. For a quick look at a single firmware, continue with the
[Quickstart](quickstart.md).

## Install failures

Errors from the first `cmake -S . -B build ...` configure, and what to do about each.

**Apple clang found.**

```text
stest: /usr/bin/clang is Apple clang, which cannot load LLVM pass plugins. Use upstream LLVM (brew install llvm) via -DSTEST_LLVM_ROOT.
```

(The path is the clang stest found.) Apple's clang cannot load the MMIO pass. Install Homebrew's LLVM (`brew install llvm`). If it is
installed but not found, pass its prefix: `-DSTEST_LLVM_ROOT=$(brew --prefix llvm)`. Delete the
build directory first if it was configured with the wrong compiler, since CMake caches compiler
choices.

**LLVM not found.**

```text
stest: upstream clang not found. Set -DSTEST_LLVM_ROOT=<llvm prefix>.
```

stest looks for `llvm-config` and `clang` in `STEST_LLVM_ROOT` (the CMake variable, then the
environment variable), in `brew --prefix llvm`, and on `PATH` (also as `clang-18` ...
`clang-23`). Set `STEST_LLVM_ROOT` to the directory that contains `bin/clang`, for example
`/usr/lib/llvm-18` on Ubuntu. If clang is found but the LLVM CMake package is not (on Ubuntu,
`llvm-18-dev` is missing), CMake fails in `find_package(LLVM)`; install the dev package. An LLVM
older than 18 fails with `stest requires LLVM >= 18, found <version>`.

**Anaconda (or another cctools) `ld` on `PATH` (macOS).** A cctools-port `ld` earlier in `PATH`,
as Anaconda installs, produces pass plugins that macOS refuses to load and firmware executables
whose debug info is empty, so tests cannot read firmware variables. stest avoids it: it asks
`xcrun -f ld` for Xcode's linker and links the plugin and every firmware with it. If you still
see the pass plugin fail to load, or tests fail to look up firmware variables by name, check that
`xcrun -f ld` works (install the Command Line Tools with `xcode-select --install`), then delete
the build directory and configure again.

**clang and LLVM versions differ.** When `CMAKE_C_COMPILER` points at a different clang than the
LLVM the pass is built against, `stest_add_firmware` stops with a message naming both
(`... the C compiler is <path> but firmware must be built with <clang> (STEST_CLANG) ...` or
`... clang major version <n> ... differs from LLVM <m> the MMIO pass was built against ...`).
Leave `CMAKE_C_COMPILER` unset; stest chooses the matching clang.

Errors after the install (a firmware that aborts, a test that cannot find an executable) are
covered in [Getting started](guide/getting-started.md#first-errors) and
[Troubleshooting](troubleshooting.md).
