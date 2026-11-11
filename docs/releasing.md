# Releasing

For the maintainer. A release is made from `main` when CI is green.

1. **Read the log of a clean run.** On a fresh game folder, install the player zip and start a level: no errors in
   `gridnode.log`, the menu opens, the trainer loads.
2. **Test what is new.** Every API added since the last release has a numbered test in `progress.md`; run each in
   game. Something that fails or is not tested is marked *experimental* in [Modding](modding.md) and listed in
   [Known issues](known-issues.md), or taken out.
3. **CI**: host tests, `validate --strict`, `rust --check`, docs, the MinGW build and the template build. Locally:
   `ctest`, `python3 tools/gen/gridnode.py validate --strict`, `gridnode.py rust --check`, `gridnode.py docs --build`,
   `cargo fmt --check` and `cargo clippy -- -D warnings` in `bindings/rust/gridnode` and `examples/hello-rust`.
   With your own dump: `python3 tools/gen/gridnode.py check --dump bin/gridnode-reflected.json` (validate against the
   dump, `types.hpp`, its layout checks: what CI cannot do without a dump; also before any push that touches
   `db/annotations/`), and `gridnode.py verify-image bin/dumps/<dump>.exe`.
4. **Nothing from the game in the tree**: no image dumps, `db/reflected/`, `event-names.txt`, logs or recordings.
5. **Versions agree**: `GN_API_VERSION` (`api.h`, `sys.rs`), the README status, [Release notes](release-notes.md),
   the trainer's version, `docs/modding.md`.
6. **Tag** `vX.Y.Z`. CI builds the zips (`GridNode-<version>.zip`, `GridNode-player-<version>.zip`); download them,
   unzip one, and run the template mod build against it.
7. **Publish** the release with the notes and the two zips.
