# Bugs found in the original libnostr-c code

One entry per bug in upstream [privkeyio/libnostr-c](https://github.com/privkeyio/libnostr-c) code, found while working on this fork. Line numbers refer to upstream `e07af71` (v0.2.2) unless stated otherwise.

Each fix meant for upstream gets its own branch from `upstream/main`, one fix per branch, with a test that fails before the fix and passes after.

Status values: **found** / **fix ready** (committed on a local branch from `upstream/main`, not yet pushed or proposed) / **issue opened** / **PR opened** / **merged**.

| # | Title | Status |
|---|---|---|
| 1 | [secp256k1 ECDH returns a hashed shared secret](#1-secp256k1-ecdh-returns-a-hashed-shared-secret) | issue opened |
| 2 | [SECP256K1_LIB_DIR ignored when pkg-config finds secp256k1](#2-secp256k1_lib_dir-ignored-when-pkg-config-finds-secp256k1) | found |
| 3 | [test_zap_verify uses inconsistent test data](#3-test_zap_verify-uses-inconsistent-test-data) | found |
| 4 | [Fallback test runner does not count failures](#4-fallback-test-runner-does-not-count-failures) | found |
| 5 | [Windows CI ignores test failures](#5-windows-ci-ignores-test-failures) | found |
| 6 | [ESP-IDF smoke app only builds from a folder named libnostr-c](#6-esp-idf-smoke-app-only-builds-from-a-folder-named-libnostr-c) | fix ready |

---

## 1. secp256k1 ECDH returns a hashed shared secret

- **What is wrong:** the secp256k1 path of `nostr_key_ecdh` calls `secp256k1_ecdh(..., NULL, NULL)`. With no hash function given, secp256k1 returns SHA-256 of the compressed shared point. NIP-44 (and NIP-04) need the unhashed 32-byte x-coordinate. The noscrypt path (`NCGetSharedSecret`) already returns the x-coordinate, so the two backends give different results for the same keys.
- **File and line:** `src/key.c:482` (function starts at `src/key.c:441`).
- **Why it went unnoticed:** the only callers (`src/nip44.c:519` and `:631`, NIP-04) are compiled only together with noscrypt, so the secp256k1 path of `nostr_key_ecdh` has no callers in any current build.
- **How to reproduce:** build without noscrypt (`-DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF`), call `nostr_key_ecdh` with the keys from any NIP-44 `get_conversation_key` test vector, and compare the 32 bytes with the vector's shared x-coordinate. They differ. (Test to be written in M4.)
- **Proposed fix:** pass a custom `secp256k1_ecdh_hash_function` that copies its `x32` input (the shared point's 32-byte x coordinate) to the output unchanged. `secp256k1_ecdh.h` defines this hook for turning the shared point into the secret; when `NULL` is passed it uses `secp256k1_ecdh_hash_function_sha256`, which hashes the compressed point. Prove the fix with the NIP-44 conversation-key test vectors.
- **Platform:** every build using the secp256k1 backend instead of noscrypt (macOS, Linux, ESP32 once this fork's noscrypt-free build exists). Not platform-specific.
- **Status:** issue opened. Raised in upstream issue [#75](https://github.com/privkeyio/libnostr-c/issues/75) (2026-11-18); the maintainer confirmed it as a real bug and welcomed a fix with test vectors (2026-11-19). No PR yet.

## 2. SECP256K1_LIB_DIR ignored when pkg-config finds secp256k1

- **What is wrong:** when pkg-config finds secp256k1, `SECP256K1_FOUND` is set and the whole block that honours `-DSECP256K1_LIB_DIR` is skipped. Linking then prefers `SECP256K1_LIBRARIES`, which pkg-config fills with the bare name `secp256k1`, over `SECP256K1_LINK_LIBRARIES`, which holds the full path. If the library is not in a folder the linker searches by default, linking fails.
- **Same cause, second symptom:** the configure check `check_library_exists(${SECP256K1_LIBRARIES} secp256k1_schnorrsig_sign32 "" ...)` gets the bare name and an empty location, fails, and prints "secp256k1 lacks schnorrsig support" even when the library has it. That only changes the label returned by `nostr_feature_crypto_backend_info()`; signing is unaffected.
- **File and line:** `CMakeLists.txt:392` (`if(NOT SECP256K1_FOUND)`, `SECP256K1_LIB_DIR` handled at `:394`), `CMakeLists.txt:905–907` (link order), `CMakeLists.txt:566` (schnorrsig check).
- **How to reproduce:** on Apple silicon with `brew install secp256k1`, run `cmake -S . -B build -DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF -DSECP256K1_LIB_DIR="$(brew --prefix secp256k1)/lib"` and then `cmake --build build`. Configure prints `Linking with secp256k1 library: secp256k1` and the schnorrsig warning; the build fails with `ld: library 'secp256k1' not found`. Workaround: add `-DCMAKE_SHARED_LINKER_FLAGS="-L/opt/homebrew/lib" -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/lib"`.
- **Proposed fix:** when pkg-config found secp256k1, link with `SECP256K1_LINK_LIBRARIES` (full paths) in preference to `SECP256K1_LIBRARIES`, or use pkg-config's `IMPORTED_TARGET`. Let an explicit `SECP256K1_LIB_DIR` take priority over pkg-config. Give the schnorrsig check the library's folder (or use `check_symbol_exists` with the found include and library).
- **Platform:** seen on macOS (Apple silicon, Homebrew in `/opt/homebrew/lib`). The bug is general, but it only shows where pkg-config finds secp256k1 outside the linker's default folders. Ubuntu CI (`/usr/lib`) and upstream's macOS CI (secp256k1 built into `/usr/local/lib`, a default folder) do not hit it.
- **Status:** found. No matching upstream issue or PR.

## 3. test_zap_verify uses inconsistent test data

- **What is wrong:** the test builds a zap request for 21,000 millisats (`tests/test_zap.c:177`), but its receipt's `bolt11` invoice is `lnbc210u...`, which is 21,000,000 millisats (`tests/test_zap.c:151` in another test already expects 21,000,000 from that invoice). `nostr_zap_verify` correctly rejects the mismatch at `src/zap.c:257` and returns `NOSTR_ERR_INVALID_EVENT`. The test's check fails, but the failure is hidden (see #4), so the rest of the test never runs: the receipt's signature check (`src/zap.c:263`) and the wrong-server-key case (`tests/test_zap.c:233`) are never exercised.
- **Second data problem:** the receipt's `p` tag is 62 hex characters (`tests/test_zap.c:213`); a public key is 64.
- **File and line:** `tests/test_zap.c:177`, `:213`, `:230` (the failing check); `src/zap.c:257`.
- **How to reproduce:** build with tests, run `ctest -V` (or `./tests/test_runner`) and look for `Assertion failed: NOSTR_OK != err` just before `Success: test_zap_verify`.
- **Proposed fix:** request 21,000,000 millisats in `test_zap_verify` (or use an invoice for 21 sats) and make the `p` tag 64 characters (matching the request's recipient). Only test data change; the library is behaving correctly. The fix will only show up as a fix once #4 makes failures count.
- **Platform:** all. Seen on macOS in both of this fork's baseline builds, and in upstream's own CI: the latest `main` run of the "CI" workflow ([run 37258828795](https://github.com/privkeyio/libnostr-c/actions/runs/37258828795), v0.2.2, Ubuntu) prints the same line in its valgrind step, which runs `test_runner` directly. The "Build and Test" workflow uses `ctest --output-on-failure`, which prints nothing for programs it counts as passed, so it does not show there.
- **Status:** found. No matching upstream issue or PR.

## 4. Fallback test runner does not count failures

- **What is wrong:** without Unity, failing checks never reach `ctest`.
  - `tests/CMakeLists.txt:3` looks for Unity through pkg-config only; Unity installs a CMake package, not a pkg-config file, so it is never found.
  - Many test files carry their own check macros behind `#ifndef HAVE_UNITY` (for example `tests/test_key.c:8`, `tests/test_zap.c:13`). Nothing defines `HAVE_UNITY`, so these are always used. They print a message and `return` from the test function without recording anything; the caller then prints `Success: ...`.
  - `tests/test_runner.c:101` ends with `return 0` regardless. It runs the event, key (signing and verifying), bech32, NIP-05, NIP-21, relay, zap, NIP-10, NIP-18, NIP-25 and utils tests.
  - `tests/unity.h` is a small imitation of Unity, not Unity.
  - Eight files track failures themselves (`test_filter_clone`, `test_nip18`, `test_nip26`, `test_nip50` and the four `test_relay_protocol_*` files). `test_nip18.c` is also compiled into `test_runner`, where its count is not returned.
- **File and line:** `tests/test_runner.c:101`, `tests/CMakeLists.txt:3`, the `#ifndef HAVE_UNITY` blocks in the test files, `tests/unity.h`.
- **How to reproduce:** change any `TEST_ASSERT_EQUAL` in `tests/test_key.c` so it must fail, rebuild, run `ctest`: it reports 100% passed. Bug #3 is a live example.
- **Proposed fix:** use real Unity (v2.6.0, the version ESP-IDF 5.5.6 bundles), fetched by CMake and pinned by commit, remove the fallback macros and `tests/unity.h`, run each test through `RUN_TEST`, and end every `main` with `return UNITY_END();`. Prove it with a deliberately broken check that makes `ctest` fail. Could be split into several small PRs (harness first, then files in groups).
- **Platform:** all.
- **Status:** found. Flagged once before: in upstream PR [#52](https://github.com/privkeyio/libnostr-c/pull/52) an automated reviewer noted that non-Unity failures don't propagate and can give false-green CI (for `test_nip18.c`, marked minor). There was no reply and no issue; the runner that holds the key and zap tests was not changed.

## 5. Windows CI ignores test failures

- **What is wrong:** the Windows job runs `ctest --preset win-release || echo "Tests completed with warnings"`. If any test fails, `echo` succeeds and the step passes, so a failing Windows test can never turn CI red.
- **File and line:** `.github/workflows/build.yml:309` (job `build-windows`, step "Test using CMake Preset").
- **How to reproduce:** read the step; or make a test that counts failures (for example `test_nip49`) fail and push to a fork: the Windows job stays green. The latest upstream `main` run ([run 37258828793](https://github.com/privkeyio/libnostr-c/actions/runs/37258828793)) shows `100% tests passed out of 14` there, so it is not hiding a failure today, as far as `ctest` can tell (see #4).
- **Proposed fix:** remove `|| echo "Tests completed with warnings"`. If some Windows tests are known to fail, mark those tests as expected failures or skips explicitly instead of masking the whole step.
- **Platform:** Windows CI only.
- **Status:** found. No matching upstream issue or PR.

## 6. ESP-IDF smoke app only builds from a folder named libnostr-c

- **What is wrong:** the smoke app finds the library with `EXTRA_COMPONENT_DIRS ../../`, i.e. the repository folder. ESP-IDF names a component after its folder, but `main` requires the fixed name `libnostr-c`. If the repository is cloned under any other folder name, configuration fails with `Failed to resolve component 'libnostr-c' required by component 'main': unknown name.` Upstream CI passes only because it checks the repository out into a folder named exactly `libnostr-c` (`.github/workflows/build.yml:175`).
- **File and line:** `test/esp-idf/CMakeLists.txt:3` (`EXTRA_COMPONENT_DIRS`), `test/esp-idf/main/CMakeLists.txt:4` (`REQUIRES libnostr-c`). Both date from PR [#25](https://github.com/privkeyio/libnostr-c/pull/25).
- **How to reproduce:** clone the repository into a folder with a different name (for example `git clone https://github.com/privkeyio/libnostr-c.git libnostr-c-test`), place `noscrypt` and `secp256k1-frost` next to it at the commits CI pins, then run `idf.py set-target esp32s3` in `test/esp-idf`. It fails with the error above.
- **Fix:** `test/esp-idf/main/CMakeLists.txt` now derives the component name the way ESP-IDF does (`get_filename_component(... ABSOLUTE)` on `${COMPONENT_DIR}/../../..`, then `NAME`; compare `tools/cmake/component.cmake:152–158` in ESP-IDF 5.5.6) and passes it to `REQUIRES`. Three lines of CMake plus a comment, test app only; the library, CI and the folder layout are unchanged. An `idf_component.yml` local dependency was considered and not used, because it adds a file and brings in the component manager's naming.
- **Proof (2026-11-19, ESP-IDF 5.5.6, target `esp32s3`):** before the fix, `idf.py set-target esp32s3` from a folder named `libnostr-c-esp32` failed with the error above. After it, the smoke app builds from that folder (component registered as `libnostr-c-esp32`) and from a folder named `libnostr-c` (CI's layout); both produce the same 0x3aeb0-byte app. The `libnostr-c-esp32` build prints `SMOKE-PASS` (11/11) on board A.
- **Platform:** all (any checkout whose folder is not named `libnostr-c`). Seen on macOS with ESP-IDF 5.5.6, from this fork's `libnostr-c-esp32` folder.
- **Status:** fix ready. Commit `283a962` on local branch `fix-esp-idf-component-name` (from `upstream/main` `e07af71`, one commit, not pushed, no PR); cherry-picked onto `esp32-noscrypt-free` as `587915b`. No matching upstream issue or PR.
