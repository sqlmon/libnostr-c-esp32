# Bugs found in the original libnostr-c code

One entry per bug in upstream [privkeyio/libnostr-c](https://github.com/privkeyio/libnostr-c) code, found while working on this fork. Line numbers refer to upstream `e07af71` (v0.2.2) unless stated otherwise.

Each fix meant for upstream gets its own branch from `upstream/main`, one fix per branch, with a test that fails before the fix and passes after.

Status values: **found** / **fix ready** (committed on a local branch from `upstream/main`, not yet pushed or proposed) / **issue opened** / **PR opened** / **merged**.

| # | Title | Status |
|---|---|---|
| 1 | [secp256k1 ECDH returns a hashed shared secret](#1-secp256k1-ecdh-returns-a-hashed-shared-secret) | issue opened |
| 2 | [SECP256K1_LIB_DIR ignored when pkg-config finds secp256k1](#2-secp256k1_lib_dir-ignored-when-pkg-config-finds-secp256k1) | found |
| 3 | [test_zap_verify uses inconsistent test data](#3-test_zap_verify-uses-inconsistent-test-data) | fix ready |
| 4 | [Fallback test runner does not count failures](#4-fallback-test-runner-does-not-count-failures) | fix ready |
| 5 | [Windows CI test step prints a misleading message on failure](#5-windows-ci-test-step-prints-a-misleading-message-on-failure) | found (cosmetic) |
| 6 | [ESP-IDF smoke app only builds from a folder named libnostr-c](#6-esp-idf-smoke-app-only-builds-from-a-folder-named-libnostr-c) | fix ready |
| 7 | [BUILD_TESTS and BUILD_EXAMPLES are documented but do not exist](#7-build_tests-and-build_examples-are-documented-but-do-not-exist) | fix ready (tests switch only) |
| 8 | [cmake --install ships the header template, not the generated header](#8-cmake---install-ships-the-header-template-not-the-generated-header) | found |
| 9 | [Event and zap tests fail in builds without cJSON](#9-event-and-zap-tests-fail-in-builds-without-cjson) | fix ready (stacked on #4) |

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
- **Fix:** `test_zap_verify` now requests 21,000,000 millisats, and both zap verify tests (`:213`, `:290`) use the 64-character hex of the recipient key. `test_zap_verify_invalid_amount` keeps its deliberate 42,000-millisat mismatch. Test data only.
- **Proof (2026-11-19, macOS, OpenSSL build of upstream code with the fallback runner):** before the fix, the `ctest -V` log prints `Assertion failed: NOSTR_OK != err`; after it, the line is gone, all six zap tests print `Success`, and `ctest` reports 15/15. With real Unity (#4) the same failure is counted: `unit_tests` fails with `test_zap_verify:FAIL: Expected 0 Was 2` before this fix and passes after.
- **Status:** fix ready. Commit `e8762ba` on local branch `fix-zap-test-data` (from `upstream/main` `e07af71`, one commit, not pushed, no PR); cherry-picked onto `esp32-noscrypt-free` as `0d3d2ce`. No matching upstream issue or PR. Must be merged before or together with #4, because #4 makes this failure count.

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
- **Fix:** branch `fix-test-unity`, six commits after the `BUILD_TESTING` switch (#7):
  - `f5aa42a` downloads Unity v2.6.0 with `FetchContent`, pinned to commit `860062d51b2e8a75d150337b63ca2a472840d13c`, only when `BUILD_TESTING` is on. It compiles Unity's single source as `nostr_unity` instead of adding Unity's own project, so `cmake --install` does not ship Unity. It renames `tests/unity.h` to `unity_fallback.h` so it no longer hides the real header, and adds a note on Unity and the first-configure download to `tests/README.md`, `CONTRIBUTING.md` and `docs/BUILDING.md`.
  - `651f747` moves `test_runner` (12 files) and the standalone NIP-05, NIP-10, NIP-18 and NIP-25 programs to Unity: one tally per program, every test through `RUN_TEST`, `return UNITY_END();`.
  - `bd8b8ec` does the same for NIP-26, NIP-49, NIP-50, filter clone and the 5-file relay protocol program. Most of these already had Unity branches behind `HAVE_UNITY`; those are now used.
  - `8ce08ca` moves `test_random` and `test_thread_safety` to the real header; `c02b5fb` deletes the imitation header; `6b4ceb3` drops completion messages that printed even when tests failed.
- **Proof (2026-11-19, macOS, both baseline builds):** with the zap failure (#3) still in place, `ctest` reports 14/15 and Unity `119 Tests 1 Failures` in `unit_tests` (`test_zap_verify`). A deliberately broken check in `tests/test_nip10.c` raised it to 2/15 failed (`nip10_tests` `10 Tests 1 Failures`, `unit_tests` `119 Tests 2 Failures`); undone, never committed. With #3 applied: 15/15 and 359 Unity tests, 0 failures, in both builds.
- **Left out:**
  - The 9 test files that no default build compiles (NIP-04, NIP-44 ×2, NIP-47 ×2, NIP-59, HD keys ×2, NIP-06) are unchanged, since they could not be built or run here. NIP-44 and NIP-59 are among them; this fork switches them on in M5 and M8.
  - NIP-51, NIP-65 and relay integration use their own check macros and already make `ctest` fail. Left as they are; converting them is a possible follow-up.
  - On CMake 3.16–3.17, Unity's own install rules still apply, because `FetchContent`'s `SOURCE_SUBDIR` (used to skip them) needs CMake 3.18.
- **Merge order:** this makes `test_zap_verify` fail upstream's CI, and five tests fail in builds without cJSON (Windows), so #3 and #9 must be merged first or together.
- **Status:** fix ready. Local branch `fix-test-unity` (from `upstream/main` `e07af71`, not pushed, no PR); cherry-picked onto `esp32-noscrypt-free` as `2b998e7`, `0425ac7`, `2693d82`, `b4461b5`, `86262ab`, `4e19e6c`. Flagged once before: in upstream PR [#52](https://github.com/privkeyio/libnostr-c/pull/52) an automated reviewer noted that non-Unity failures don't propagate and can give false-green CI (for `test_nip18.c`, marked minor). There was no reply and no issue.

## 5. Windows CI test step prints a misleading message on failure

- **What is wrong:** the Windows job runs `ctest --preset win-release || echo "Tests completed with warnings"`. The intent looks like hiding test failures, but it does not: the step runs in PowerShell (`pwsh`), and GitHub ends a PowerShell step with the last native command's exit code, which is still ctest's. So a failing test does turn the job red; it only prints "Tests completed with warnings" first, which reads as if nothing failed.
- **Correction (2026-11-19):** this entry first said the step hides failures. This fork's run 44643270660 showed otherwise: the log prints `Tests completed with warnings` and then `Process completed with exit code 1`, and the job failed (see #9). Lowered to cosmetic.
- **File and line:** `.github/workflows/build.yml:309` (job `build-windows`, step "Test using CMake Preset").
- **How to reproduce:** make a test fail on Windows and push to a fork; the job fails, and the log shows the message just before the exit code.
- **Proposed fix:** remove `|| echo "Tests completed with warnings"`; the result is the same and the log no longer suggests the tests passed with warnings.
- **Platform:** Windows CI only.
- **Status:** found (cosmetic). No matching upstream issue or PR.

## 6. ESP-IDF smoke app only builds from a folder named libnostr-c

- **What is wrong:** the smoke app finds the library with `EXTRA_COMPONENT_DIRS ../../`, i.e. the repository folder. ESP-IDF names a component after its folder, but `main` requires the fixed name `libnostr-c`. If the repository is cloned under any other folder name, configuration fails with `Failed to resolve component 'libnostr-c' required by component 'main': unknown name.` Upstream CI passes only because it checks the repository out into a folder named exactly `libnostr-c` (`.github/workflows/build.yml:175`).
- **File and line:** `test/esp-idf/CMakeLists.txt:3` (`EXTRA_COMPONENT_DIRS`), `test/esp-idf/main/CMakeLists.txt:4` (`REQUIRES libnostr-c`). Both date from PR [#25](https://github.com/privkeyio/libnostr-c/pull/25).
- **How to reproduce:** clone the repository into a folder with a different name (for example `git clone https://github.com/privkeyio/libnostr-c.git libnostr-c-test`), place `noscrypt` and `secp256k1-frost` next to it at the commits CI pins, then run `idf.py set-target esp32s3` in `test/esp-idf`. It fails with the error above.
- **Fix:** `test/esp-idf/main/CMakeLists.txt` now derives the component name the way ESP-IDF does (`get_filename_component(... ABSOLUTE)` on `${COMPONENT_DIR}/../../..`, then `NAME`; compare `tools/cmake/component.cmake:152–158` in ESP-IDF 5.5.6) and passes it to `REQUIRES`. Three lines of CMake plus a comment, test app only; the library, CI and the folder layout are unchanged. An `idf_component.yml` local dependency was considered and not used, because it adds a file and brings in the component manager's naming.
- **Proof (2026-11-19, ESP-IDF 5.5.6, target `esp32s3`):** before the fix, `idf.py set-target esp32s3` from a folder named `libnostr-c-esp32` failed with the error above. After it, the smoke app builds from that folder (component registered as `libnostr-c-esp32`) and from a folder named `libnostr-c` (CI's layout); both produce the same 0x3aeb0-byte app. The `libnostr-c-esp32` build prints `SMOKE-PASS` (11/11) on board A.
- **Platform:** all (any checkout whose folder is not named `libnostr-c`). Seen on macOS with ESP-IDF 5.5.6, from this fork's `libnostr-c-esp32` folder.
- **Status:** fix ready. Commit `283a962` on local branch `fix-esp-idf-component-name` (from `upstream/main` `e07af71`, one commit, not pushed, no PR); cherry-picked onto `esp32-noscrypt-free` as `587915b`. No matching upstream issue or PR.

## 7. BUILD_TESTS and BUILD_EXAMPLES are documented but do not exist

- **What is wrong:** the docs tell readers to pass `-DBUILD_TESTS=ON` (`CONTRIBUTING.md:33`, `docs/GETTING_STARTED.md:56`) and `-DBUILD_EXAMPLES=ON` (`CONTRIBUTING.md:33`, `docs/GETTING_STARTED.md:59`, `examples/README.md:7` and `:11`), and `docs/BUILDING.md:236` lists `BUILD_TESTING` (default ON). `CMakeLists.txt` defines none of these: `enable_testing()`, `add_subdirectory(tests)` and `add_subdirectory(examples)` run unconditionally (`CMakeLists.txt:963–965`). Turning tests or examples off has no effect.
- **File and line:** as above. (The `BUILD_TESTS=OFF` in `.github/workflows/build.yml:71` and `:377` is passed to noscrypt's own build, not to libnostr-c.)
- **How to reproduce:** configure with `-DBUILD_TESTS=OFF`; CMake warns that the variable was not used, and the test programs are still built.
- **Fix (tests only):** `6be2a14` adds `option(BUILD_TESTING "Build the test suite" ON)` around `enable_testing()` and `add_subdirectory(tests)`, and makes `CONTRIBUTING.md` and `docs/GETTING_STARTED.md` say `BUILD_TESTING` like `docs/BUILDING.md`. `BUILD_TESTING` is CMake's standard name, which package managers already turn off. `BUILD_EXAMPLES` is unchanged.
- **Proof (2026-11-19, macOS):** with `-DBUILD_TESTING=OFF`, configure succeeds and no Unity download happens (no `_deps` folder); with the default, all 15 test programs are built.
- **Platform:** all.
- **Status:** fix ready for the tests switch. Commit `6be2a14`, the first commit on local branch `fix-test-unity` (from `upstream/main` `e07af71`, not pushed, no PR); cherry-picked onto `esp32-noscrypt-free` as `b3182e3`. `BUILD_EXAMPLES`: found. No matching upstream issue or PR.

## 8. cmake --install ships the header template, not the generated header

- **What is wrong:** `install(DIRECTORY include/ DESTINATION include)` copies the source `include/` folder, which holds the template `nostr_features.h.in`. The generated `nostr_features.h` is written to the build folder (`CMakeLists.txt:942–943`) and is never installed. The installed `nostr.h` includes it (`include/nostr.h:43`), so a program compiled against an installed libnostr-c fails with `nostr_features.h` not found.
- **File and line:** `CMakeLists.txt:975`; `include/nostr.h:43`.
- **How to reproduce:** `cmake --install build --prefix /tmp/nostr-install`, then list `/tmp/nostr-install/include`: it contains `nostr_features.h.in` but no `nostr_features.h`.
- **Proposed fix:** install the generated header as well, for example `install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/nostr_features.h DESTINATION include)`, and exclude the template: `install(DIRECTORY include/ DESTINATION include PATTERN "*.in" EXCLUDE)`.
- **Platform:** all host builds (the ESP-IDF build does not use `cmake --install`). Seen on macOS while checking that Unity is not installed (#4).
- **Status:** found. No matching upstream issue or PR.

## 9. Event and zap tests fail in builds without cJSON

- **What is wrong:** two separate test problems that only show when cJSON is off, as in the Windows preset (`NOSTR_FEATURE_JSON_ENHANCED` off, so `HAVE_CJSON` is undefined). The fallback runner hid both; with real Unity (#4) they fail the Windows job.
  - In their no-cJSON branches, `test_event_to_json`, `test_event_from_json` and `test_event_from_json_invalid` expect `NOSTR_ERR_ENCODING`, but `src/event.c` returns `NOSTR_ERR_NOT_SUPPORTED` there (for example `src/event.c:438`). The library is right: `docs/FEATURE_FLAGS.md:101` says "Disabled functions return `NOSTR_ERR_NOT_SUPPORTED`", the enum documents it as "Feature not supported" and `NOSTR_ERR_ENCODING` as an encoding/decoding error, and all 11 compiled-out (`#else`) branches in `src/` that return either code return `NOT_SUPPORTED`. Both the expectation and the return date from the first commit (`71265bf`).
  - `test_zap_verify` and `test_zap_verify_invalid_amount` serialize the zap request with `nostr_event_to_json`, which cannot work without cJSON, and do not check for it.
- **File and line:** `tests/test_event.c:140`, `:171`, `:185`; `tests/test_zap.c:147`, `:224` (as on branch `fix-test-unity`).
- **How to reproduce:** with real Unity (#4), configure with `-DNOSTR_FEATURE_JSON_ENHANCED=OFF -DNOSTR_FEATURE_RELAY=OFF` (relay support switches JSON back on) and run `ctest`: `unit_tests` reports `114 Tests 5 Failures`, three `Expected 6 Was 12` and two `Expected 0 Was 12`. Upstream-style CI shows the same in `build-windows`.
- **Fix:** the three event tests expect `NOSTR_ERR_NOT_SUPPORTED`; the two zap verify tests call `TEST_IGNORE_MESSAGE("needs cJSON")` when `HAVE_CJSON` is undefined. Test code only.
- **Proof (2026-11-19, macOS, no-cJSON build):** before, `114 Tests 5 Failures 0 Ignored`; after, `114 Tests 0 Failures 2 Ignored` and 15/15. The normal builds stay at 359 tests, 0 failures.
- **Platform:** all builds without cJSON; seen in Windows CI (fork run 44643270660) and reproduced on macOS.
- **Status:** fix ready. Commit `83cb603` on local branch `fix-tests-without-cjson`, **stacked on `fix-test-unity`** rather than `upstream/main`: on `upstream/main` the failures cannot be counted and `TEST_IGNORE` does not exist, so it goes upstream as part of the Unity series (#4). Cherry-picked onto `esp32-noscrypt-free` as `31e453d`. Not pushed, no PR. No matching upstream issue or PR.
