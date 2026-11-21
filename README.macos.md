# Building and signing GDB on macOS

This is the native debugger build used on this Apple Silicon machine
(arm64, Darwin 25, Apple clang, Homebrew GMP and MPFR). The same
commands work on an Intel Mac if Homebrew is installed under
`/usr/local` instead of `/opt/homebrew`.

The binary that is allowed to debug is the one installed under
`staging/bin/gdb`, signed afterwards. A freshly linked GDB is only
ad-hoc signed and has no debugger entitlement. macOS then refuses
`task_for_pid` with `(os/kern) failure (0x5)`.

Do not sign `/usr/bin/gdb`, a Homebrew GDB, or whatever `gdb` resolves
to on `PATH`.

## A. Configure and build

From the repository root:

```bash
cd "$(git rev-parse --show-toplevel)"

if [ -d /opt/homebrew/opt/gmp ]; then
  BREW=/opt/homebrew
elif [ -d /usr/local/opt/gmp ]; then
  BREW=/usr/local
else
  echo "Install GMP and MPFR first: brew install gmp mpfr" >&2
  exit 1
fi

mkdir -p build-aarch64-darwin staging
cd build-aarch64-darwin

../configure \
  --prefix="$(cd .. && pwd)/staging" \
  --with-gmp="$BREW/opt/gmp" \
  --with-mpfr="$BREW/opt/mpfr" \
  --disable-binutils \
  --disable-gas \
  --disable-ld \
  --disable-gold \
  --disable-gprof \
  --disable-gprofng \
  --disable-sim \
  --disable-gdbserver \
  --disable-nls \
  --without-guile \
  --with-python=no \
  --disable-werror

make -j"$(sysctl -n hw.ncpu)" all-gdb
make install-gdb
```

Leave `--enable-codesign` unset. Signing is the separate step below,
and it has to be repeated after every install because `make install-gdb`
replaces `staging/bin/gdb`.

`--with-python=no` matches this machine: the GDB that was validated
here has no Python pretty-printers. The other `--disable-*` switches
build the debugger only.

If you rebuild just the `gdb` executable later, copy it over the
installed binary and sign that copy again:

```bash
cd "$(git rev-parse --show-toplevel)"
cp build-aarch64-darwin/gdb/gdb staging/bin/gdb
```

## B. Sign the installed GDB

The entitlement file is `gdb/gdb.entitlements`. It contains only:

```xml
<key>com.apple.security.cs.debugger</key>
<true/>
```

The identity must already be a codesigning certificate in the login
keychain. On this machine that identity is `codeblocks-gdb`. See what
this Mac has:

```bash
security find-identity -p codesigning -v
```

If that list is empty, create one once in Keychain Access:

1. Open Keychain Access → Certificate Assistant → Create a Certificate.
2. Name: `codeblocks-gdb`. Identity Type: Self Signed Root.
   Certificate Type: Code Signing.
3. In the login keychain, open the new certificate → Trust →
   When using this certificate: Always Trust.
4. Run `security find-identity -p codesigning -v` again and confirm
   the name appears as a valid identity.

Then, from the repository root, after every `make install-gdb` or
`cp` onto `staging/bin/gdb`:

```bash
cd "$(git rev-parse --show-toplevel)"

CERT=codeblocks-gdb

codesign --force --sign "$CERT" \
  --entitlements gdb/gdb.entitlements \
  staging/bin/gdb

codesign -dv --verbose=4 staging/bin/gdb
codesign -d --entitlements - staging/bin/gdb
staging/bin/gdb --version
```

`codesign -dv` must show `Authority=<the name in CERT>` and
`Identifier=org.gnu.gdb`. The entitlements dump must show
`com.apple.security.cs.debugger` as true. An ad-hoc signature
(`Signature=adhoc`, no Authority) will not debug.

`gdb/darwin-nat.c` also embeds an Info.plist (`CFBundleIdentifier`
`org.gnu.gdb`, `SecTaskAccess`) so the binary can be signed. That
plist is not a substitute for the entitlement. The plist comment
points at `taskgated(8)`; the command that works here is the
`codesign` invocation above.

Check that the signed binary can take a task port. `startup-with-shell`
stays off on this host because `/bin/sh` is arm64e and SIP rejects the
copy GDB would otherwise make:

```bash
cd "$(git rev-parse --show-toplevel)"

staging/bin/gdb -nx -batch \
  -ex "set startup-with-shell off" \
  -ex "set pagination off" \
  -ex "file /bin/echo" \
  -ex "start" \
  -ex "info inferiors"
```

A task-port failure here means the binary you just ran is not the
signed `staging/bin/gdb`, or the signature was lost in a later copy.
