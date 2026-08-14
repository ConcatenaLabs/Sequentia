# Release publisher

Publishes a Sequentia Core release to the download page without anyone touching
the box. It watches the repository for a new version tag and, when one appears,
builds the Linux tarball and the Windows installer **from that tag** and puts both
where the download page serves them.

Cutting a release is therefore two commands on a laptop, and nothing else:

```
git tag -a v24.0.1 <commit> -m "Sequentia Core 24.0.1"
git push origin v24.0.1
```

Within ten minutes the box starts building. It publishes when the build finishes.

## Why it is built this way

**It builds from a tag, not from `master`.** What the download page offers is the
one artifact a stranger runs without reading the source, so it should be something
we deliberately named. A tag is also the only way the script can distinguish "there
is a new release" from "someone pushed a commit".

**It has its own clone.** The committee is started from
`/root/SequentiaByClaude/src/sequentiad`. A release build in that tree would swap
the binary twenty running nodes were started from, and the swap would only surface
at the next restart. The publisher builds in `/root/sequentia/release-build` and
writes nowhere else except the download directory.

**It is niced and job-limited.** This box also produces blocks. `SEQ_BUILD_JOBS`
defaults to 4 and the whole build runs at `nice 19` with idle I/O, because starving
this machine is how the testnet stalls.

**It rewrites the page from what exists, not from what it just built.** Each card
on the download page is pointed at the newest file actually present. If the Windows
half is skipped or fails, that card keeps offering the previous installer instead of
becoming a 404.

## One-time setup

The build toolchain has to exist on the box first. This is the slow part, and it is
only done once:

```
apt-get install -y g++-mingw-w64-x86-64 nsis
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix

git clone https://github.com/GracedEternalKingCabbageMan/Sequentia.git \
  /root/sequentia/release-build/Sequentia
cd /root/sequentia/release-build/Sequentia
nice -n 19 make -C depends HOST=x86_64-pc-linux-gnu -j4   # Linux prefix, incl. Qt
nice -n 19 make -C depends HOST=x86_64-w64-mingw32 -j4    # Windows cross prefix

install -m644 contrib/sequentia/release-publisher/seq-release-publisher.{service,timer} \
  /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now seq-release-publisher.timer
```

Both prefixes include Qt, so the GUI is part of the published artifacts.

## Operating it

```
systemctl list-timers seq-release-publisher.timer   # when it next checks
journalctl -u seq-release-publisher.service -f      # follow a build
systemctl start seq-release-publisher.service       # check right now
```

State is one file, `/root/sequentia/release-build/state/published-tag`. Delete it to
force the current tag to be built and published again.

## Settings

All optional; the defaults are what the box uses.

| Variable | Default | |
|---|---|---|
| `SEQ_REPO_URL` | the GitHub repo | where tags are read from |
| `SEQ_BUILD_ROOT` | `/root/sequentia/release-build` | clone, state and staging live here |
| `SEQ_DOWNLOAD_DIR` | `/root/sequentia/downloads` | what the page serves |
| `SEQ_BUILD_JOBS` | `4` | raise only if the committee is not running |
| `SEQ_BUILD_NICE` | `19` | |
| `SEQ_BUILD_WINDOWS` | `1` | `0` publishes the Linux tarball only |
| `SEQ_CONFIGURE_ARGS` | `--disable-tests --disable-bench --enable-any-asset-fees` | `--enable-any-asset-fees` is what gives fee amounts their reference units instead of labelling them BTC/sat |

## Failure

A failed build leaves the download page untouched: artifacts are written to a
`.part` file and moved into place only once complete, and the state file is written
last, so a failure means the next tick retries the same tag rather than skipping it.
`systemctl status seq-release-publisher.service` shows the failure and the journal
has the build output.
