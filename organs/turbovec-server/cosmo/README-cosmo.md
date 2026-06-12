# turbovec-server as an Actually Portable Executable (APE)

Status: **works**. Built 2026-06-13 with the [ahgamut/rust-ape-example]
technique (repo state of 2026-05-07), adapted for the prebuilt `~/cosmocc`
toolchain (cosmocc 14.1.0) and `rustc 1.98.0-nightly (2026-06-11)`.

[ahgamut/rust-ape-example]: https://github.com/ahgamut/rust-ape-example

## Artifacts

| file | what | size |
|---|---|---|
| `turbovec-server.com` | x86_64-only APE | ~2.1 MB |
| `turbovec-server.fat.com` | fat APE (x86_64 + aarch64) | ~4.1 MB |

Both start with the `MZqFpD` APE magic, are identified by `file` as
"DOS/MBR boot sector", run directly on this Linux host, and pass the full
`smoke.sh` suite (all 14 checks) when substituted for the release binary.
The aarch64 half of the fat APE links cleanly but was **not** runtime-tested
(no qemu-aarch64 on this host).

## How to build

```bash
./cosmo/build-ape.sh
```

That is equivalent to:

```bash
export COSMO=$HOME/cosmocc
ARCH=x86_64  cargo +nightly build -Zjson-target-spec -Zbuild-std=panic_abort,std \
    --release --target=./cosmo/x86_64-unknown-linux-cosmo.json
ARCH=aarch64 cargo +nightly build -Zjson-target-spec -Zbuild-std=panic_abort,std \
    --release --target=./cosmo/aarch64-unknown-linux-cosmo.json

$COSMO/bin/apelink -l $COSMO/bin/ape-x86_64.elf \
    -o cosmo/turbovec-server.com \
    target/x86_64-unknown-linux-cosmo/release/turbovec-server.com.dbg

$COSMO/bin/apelink -l $COSMO/bin/ape-x86_64.elf -l $COSMO/bin/ape-aarch64.elf \
    -M $COSMO/bin/ape-m1.c \
    -o cosmo/turbovec-server.fat.com \
    target/x86_64-unknown-linux-cosmo/release/turbovec-server.com.dbg \
    target/aarch64-unknown-linux-cosmo/release/turbovec-server.com.dbg
```

`cargo` must be invoked from the crate root (the target specs reference the
linker wrapper by the relative path `./cosmo/gcc-linker-wrapper.bash`).

## How it works

- `{x86_64,aarch64}-unknown-linux-cosmo.json` — custom Rust target specs
  derived from the musl targets: static, no-PIE, `panic=abort`,
  `has-thread-local: false`, `no-default-libraries`, and the linker pointed
  at `cosmo/gcc-linker-wrapper.bash`.
- `gcc-linker-wrapper.bash` — filters linker args cosmocc rejects
  (`-lunwind`, `-Wl,-Bstatic/-Bdynamic`, `-Wl,--strip-all`), appends the
  compiled `stubs.c`, and delegates to
  `$COSMO/bin/$ARCH-unknown-cosmo-cc`. `ARCH` (default `x86_64`) selects the
  per-arch driver.
- `stubs.c` — defines `waitid()` returning `ENOSYS`. Rust std references it
  for pidfd support (`std::sys::pal::unix::linux::pidfd`); Cosmopolitan Libc
  does not provide it. turbovec-server never spawns processes, so the dead
  reference just needs to resolve.
- `-Zbuild-std=panic_abort,std` recompiles std for the custom target.
  Note: **do not** include `libc` in the build-std list (as upstream
  rust-ape-example does) — this crate graph already pulls `libc` from
  crates.io and the duplicate causes `E0464: multiple candidates for rmeta
  dependency libc`.
- `apelink` fuses the per-arch ELF `.com.dbg` files into the polyglot APE.

## Deviations from upstream rust-ape-example (as of 2026-05-07)

1. `-Zjson-target-spec` is now required by cargo for `.json` targets.
2. The `allows-weak-linkage` field was removed from rustc target specs;
   current nightly rejects it. Deleted from both JSONs.
3. `target-pointer-width` must be the integer `64`, not the string `"64"`.
4. Added the `-Wl,--strip-all` filter: this crate's release profile sets
   `strip = true`, and the cosmocc driver's `fixupobj` postprocessing fails
   with "missing elf symbol table" if the symtab is stripped at link time.
   (apelink strips for the final APE anyway: 4.7 MB ELF -> 2.1 MB APE.)
5. Added the `waitid` stub (upstream's hello-world examples never pull in
   std's pidfd module; a real server binary does).
6. `aarch64` spec uses `max-atomic-width: 128` matching the current built-in
   aarch64-musl target (upstream says 64).

## Portability caveat

The APE container is fully portable, but per the upstream README the *Rust*
payload bakes in Linux ABI constants (errno values, `ioctl` numbers, etc.)
at compile time via the `libc` crate, because the target spec claims
`os = "linux"`. Cosmopolitan's runtime translates syscalls, but constants
compiled into Rust code are not translated. Expect the binary to be reliable
on x86_64/aarch64 **Linux**; on macOS/Windows/BSD it will start (APE loader
works) but std code paths that compare errno values or pass Linux-specific
flags may misbehave. This matches upstream issue
ahgamut/rust-ape-example#3. Cross-OS behavior was not tested here.

## Verification performed

```text
file cosmo/turbovec-server.com      -> DOS/MBR boot sector ...
head -c8                            -> MZqFpD='
./cosmo/turbovec-server.com --help  -> usage line, exit 0
smoke.sh (binary substituted)       -> ALL CHECKS PASSED (both thin and fat)
```
