# Building the Windows installer

This runbook produces the Windows setup executable
(`elements-X.Y.Z-win64-setup.exe`, typically published as
`sequentia-core-X.Y.Z-win64-setup.exe`) containing the GUI node
(`elements-qt`), the daemon and CLI tools, and the bundled price server with
its own Python runtime.

The installer is **cross-compiled from Linux** with MinGW and packaged with
NSIS. You cannot build it natively on Windows with MSVC — the NSIS packaging
(`make deploy`) is wired to the autotools build. Any recent Ubuntu works;
building inside **WSL on a Windows machine** is the tested path and is what
this document assumes (Ubuntu 24.04 and 26.04 both verified). Budget roughly
an hour for the first build on a modern machine — most of it in `depends` —
and a few minutes for every rebuild after that.

## 1. One-time setup

Install the toolchain (as root):

```sh
apt update
apt install -y build-essential libtool autotools-dev automake pkg-config \
    bsdmainutils bison python3 zip \
    g++-mingw-w64-x86-64-posix nsis
```

The MinGW compiler must use **POSIX threads**, not win32 threads. The
`-posix` package above normally selects this by itself; verify with:

```sh
x86_64-w64-mingw32-g++ --version   # first line should end in "-posix"
```

and if it does not, switch the alternatives:

```sh
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
```

Clone the repository somewhere inside the Linux filesystem (not under
`/mnt/c/...` — the 9p filesystem makes the build many times slower):

```sh
git clone https://github.com/GracedEternalKingCabbageMan/Sequentia.git
cd Sequentia
```

## 2. WSL pitfall: clean your PATH first

**This is the single most common failure.** By default WSL appends the
Windows PATH (entries like `/mnt/c/Program Files/...`) to the Linux PATH.
The embedded spaces break several `configure` scripts inside `depends` —
the visible symptom is Boost failing with a bare `error 127` deep into an
otherwise healthy build. Before building, in the same shell (or at the top
of your build script):

```sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
```

Related WSL trap: if you write your build script with a Windows editor, it
will have CRLF line endings and fail with cryptic `\r` errors. Strip them
(`tr -d '\r' < script.sh > script-fixed.sh`) or configure your editor for LF.

## 3. Build

From the repository root, in a shell with the clean PATH from §2:

```sh
# 1. Cross-compiled dependency libraries (Qt, Boost, libevent, ...).
#    SOURCES_PATH caches the downloaded tarballs OUTSIDE the tree so a later
#    `git clean -fdx` does not force a re-download.
make -C depends HOST=x86_64-w64-mingw32 SOURCES_PATH=$HOME/depends-sources-cache -j$(nproc)

# 2. Configure the main build against those depends.
./autogen.sh
CONFIG_SITE=$PWD/depends/x86_64-w64-mingw32/share/config.site \
    ./configure --prefix=/ --disable-tests --disable-bench

# 3. Fetch the embeddable Python runtime for the bundled price server.
#    REQUIRED before `make deploy`: without it the installer ships the
#    price server with no interpreter and it will not start on user machines.
#    Downloads into contrib/price-server/python/ (git-ignored, kept across
#    rebuilds).
bash contrib/price-server/fetch-embeddable-python.sh

# 4. Compile and package. `make deploy` runs NSIS and writes the installer
#    into the repository root.
make -j$(nproc)
make deploy

ls -lh ./*.exe   # -> elements-X.Y.Z-win64-setup.exe (~37 MB)
```

## 4. Things that will bite you

- **A `git reset --hard` / `git clean` silently un-configures the cross
  build.** The next bare `make` re-runs configure as a *native Linux* build:
  `EXEEXT` becomes empty and `make deploy` later fails looking for
  `elements-qt` without `.exe`. After any git operation that touches the
  tree, always re-run step 2 of §3 (`./configure` with the MinGW
  `CONFIG_SITE`) before `make deploy`.
- **Don't skip the embeddable-Python fetch** (step 3 of §3). The build and
  the installer succeed without it; the bug only appears on the user's
  machine when the price server won't launch.
- **`make deploy` without a prior full `make`** works but compiles
  everything inside the deploy step, hiding progress; run them separately as
  above so failures are attributable.
- **Log the build.** Redirect to a file (`... > ~/build-win.log 2>&1`);
  with `-j12` an error scrolls past instantly and the log is the only way to
  find the first real failure.

## 5. Releasing

If the installer is going to be published or handed to testers as *the*
build, follow [`release-versioning.md`](release-versioning.md): bump the
version in `configure.ac` in the commit you build from, and git-tag that
exact commit (`git tag -a vX.Y.Z <commit>`). Private rebuilds keep the
version number and identify with `-uacomment` instead.
