# Baselines: M1 host builds, CI and M2 hardware

**Summary:** Both Mac builds (secp256k1 + OpenSSL, and secp256k1 + mbedTLS) compile and `ctest` reports 15/15 passed, but the fallback test runner does not count failures, and one real failure (`test_zap_verify`) is hidden in both. On hardware (M2), the unchanged smoke app prints `SMOKE-PASS` on board A, and after its checks became Unity tests, a deliberately broken check is counted as a failure. With real Unity on the Mac (M2b), both builds run 359 checks with 0 failures, and the zap failure, once counted, is fixed.

## M1 setup

| Item | Value |
|---|---|
| Date | 2026-11-19 |
| Baseline commit | `bed0895` on `esp32-noscrypt-free` (upstream `e07af71`, "Release v0.2.2 (#74)", plus the project brief only) |
| macOS | 26.5.2 (Apple silicon, Homebrew in `/opt/homebrew`) |
| Shell | ESP-IDF v5.5.6 activated (`get_idf`) in the terminal that ran the builds |
| CMake | 3.30.2, the copy bundled with ESP-IDF, because `get_idf` puts it first on PATH. A plain terminal without `get_idf` picks Homebrew's 4.5.0 instead; these builds were not run with 4.5.0 |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| OpenSSL | 3.5.9 (`openssl@3`; CMake picked it on its own although `openssl@4` is the default) |
| mbedTLS | 3.6.8 (`mbedtls@3`) |
| secp256k1 | 0.8.0 (Homebrew) |
| cJSON / libwebsockets | 1.7.19 / 5.1.0 |
| Unity | not installed (fallback runner used, see below) |
| utf8proc | not installed (optional; BIP-39 then rejects non-ASCII input) |

## M1 commands

Run from the repository root. Build folders sit under `build/`, which is git-ignored.

### Build 1: secp256k1 + OpenSSL

```sh
cmake --fresh -S . -B build/host -DCMAKE_BUILD_TYPE=Debug \
  -DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/opt/homebrew/lib" \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/lib"
cmake --build build/host -j
ctest --test-dir build/host -V > build/host/ctest-verbose.log 2>&1
```

### Build 2: secp256k1 + mbedTLS (the ESP32 code paths)

```sh
cmake -S . -B build/mbedtls -DCMAKE_BUILD_TYPE=Debug \
  -DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF -DNOSTR_CRYPTO_MBEDTLS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix mbedtls@3)" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/opt/homebrew/lib" \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/lib"
grep '^#define HAVE_MBEDTLS' build/mbedtls/include/nostr_features.h
cmake --build build/mbedtls -j
ctest --test-dir build/mbedtls -V > build/mbedtls/ctest-verbose.log 2>&1
otool -L build/mbedtls/tests/test_random | grep mbedcrypto
```

The `grep` prints `#define HAVE_MBEDTLS`; `otool` shows `libmbedcrypto.16.dylib ... current version 3.6.8`.

### Linker workaround

Without the two `-L/opt/homebrew/lib` flags, linking `libnostr.dylib` fails with `ld: library 'secp256k1' not found`, and configure warns that "secp256k1 lacks schnorrsig support" (false: the library has `secp256k1_schnorrsig_sign32`). Setting `-DSECP256K1_LIB_DIR=...` does not help, because that option is skipped when pkg-config already found secp256k1. Details in [UPSTREAM.md](UPSTREAM.md). Homebrew on Apple silicon installs into `/opt/homebrew/lib`, which the linker does not search by default; upstream's macOS CI builds secp256k1 into `/usr/local/lib`, which it does.

## M1 results

| | Build 1 (OpenSSL) | Build 2 (mbedTLS) |
|---|---|---|
| Test programs run by `ctest` | 15 | 15 |
| Reported passed | 15 | 15 |
| Reported failed | 0 | 0 |
| Reported skipped | 0 | 0 |
| Hidden failures | 1 (`test_zap_verify`) | 1 (`test_zap_verify`) |
| Compiler warnings (own code) | 1, `tests/test_thread_safety.c:156` (`-Wgnu-folding-constant`) | same |

The two verbose logs are identical apart from timings and folder names.

### "Passed" only means "did not crash"

Unity is not installed, so the tests use a fallback:

- `tests/unity.h` is a small imitation of Unity. On top of that, many test files (among them `test_key.c`, `test_event.c` and `test_zap.c`) carry their own copy of the check macros behind `#ifndef HAVE_UNITY`. Nothing ever defines `HAVE_UNITY`, so those files always use their own copy.
- Those checks print a message (`Assertion failed: ...`) and `return` from the test function without counting anything. The caller then prints `Success: ...`.
- Several programs end with `return 0` regardless of results, including `tests/test_runner.c`, which holds the key, signing, event and zap tests.

A failing check therefore never reaches `ctest`. The only way to see one is to search the verbose log for the runner's messages (`Assertion failed`, `Condition failed`, `Memory comparison failed`, `Pointer is NULL`).

### The hidden zap failure

Both builds print `Assertion failed: NOSTR_OK != err` just before `Success: test_zap_verify`. In a debugger (Build 1):

- The failing check is `tests/test_zap.c:230`: `nostr_zap_verify(receipt, request, server_pubkey_hex)` returns `NOSTR_ERR_INVALID_EVENT`.
- It returns from the amount check in `src/zap.c:257`: `request_amount = 21000` (millisats, from the request) but `receipt_amount = 21000000` (from the test's `lnbc210u...` invoice, 210 µBTC).
- So the test data are inconsistent; the library rejects them correctly. Signing worked (`nostr_event_sign` returned `NOSTR_OK` just before).
- Because the test stops there, the receipt's signature check (`src/zap.c:263`) and the wrong-server-key check (`tests/test_zap.c:233`) never run.
- The same test also uses a 62-character `p` tag (`tests/test_zap.c:213`) where a 64-character public key is expected.

No skipped or failing lines mention signing, verifying or Schnorr. The `test_key.c` signing tests (sign, verify, rejecting a corrupted signature or ID) and the NIP-26 delegation verify tests printed no failures, but under this runner that is weaker evidence than it looks.

## M1 other findings

- **Smoke app paths:** `test/esp-idf/CMakeLists.txt` expects `../../../noscrypt` and `../../../secp256k1-frost` next to the repository. Neither exists in this checkout's parent folder. To be handled in M2.
- **macOS CI uses a different secp256k1 setup:** upstream's `build-macos` job runs the same 15 test programs (secp256k1 and noscrypt variants), but builds secp256k1 from source into `/usr/local/lib`, which the macOS linker searches by default. That is why CI never hits the linker problem above. (The `-DBUILD_TESTS=OFF` in that job belongs to the step that builds noscrypt, not libnostr-c.) Its `ctest --output-on-failure` prints nothing for programs it counts as passed, so a hidden failure would not show there.
- **Mac mbedTLS is 3.6.8:** `CMakeLists.txt:178` says the host mbedTLS is 2.x. That holds for upstream's Linux CI, not here. ESP-IDF 5.5.6 ships 3.6.7, so the Mac is closer to the ESP32 build than CI is.
- **26 of 35 test files compiled, into 15 test programs:** `test_runner` alone is built from 12 files and `test_relay_protocol` from 5. The 9 files not compiled are the NIP-04, NIP-44 (2), NIP-59, HD-key (`test_hd_key`, `test_hd_integration`, `test_nip06`) and NIP-47 (2) tests, because `NOSTR_FEATURE_NIP04/44/47/59` and `HD_KEYS` default to `OFF` (and NIP-44 needs noscrypt). Of the two NIP-47 files, only `test_nip47_minimal.c` is wired up even when NIP-47 is on; `test_nip47.c` is never built.
- **utf8proc not installed:** optional; without it BIP-39 rejects non-ASCII mnemonics.
- **Schnorr label:** the failed configure check only changes the text returned by `nostr_feature_crypto_backend_info()` ("secp256k1 (limited - no schnorr)"). Signing and verifying do not depend on it.

## CI baseline

All 12 jobs green on `9e71812` (this fork's Actions runs 44607338835 and 44607338925), with the zap failure still hidden: the "CI" job's valgrind step prints `Assertion failed: NOSTR_OK != err`, as upstream's does.

## M2 hardware baseline

**Summary:** the existing smoke app, unchanged and still built with noscrypt, prints `SMOKE-PASS` on board A (11 of 11 checks).

### Setup

| Item | Value |
|---|---|
| Date | 2026-11-19 |
| Code | `88ba821` (code identical to `9e71812`; only `docs/esp32/` differs), app version `v0.2.2-6-g88ba821` |
| ESP-IDF | v5.5.6, target `esp32s3` (upstream CI uses v5.4.1) |
| esptool | 4.13.0 |
| noscrypt | `privkeyio/noscrypt` at `02e47e81eb9338d8cb9ae84a14a3992f00f4a651` ("Merge tag 'v0.1.15' into esp-idf-support"), **LGPL-2.1-or-later**. Cloned next to the repository for this baseline only; never committed |
| secp256k1-frost | `privkeyio/secp256k1-frost` at `746dbbea5c34dde963c9bd55bc7474b2d50fa597` ("Add ESP-IDF component support"), MIT |
| cJSON | `espressif/cjson` 1.7.19~2, fetched by the ESP-IDF component manager (`idf_component.yml` asks for `"*"`), MIT |
| Board | Board A: ESP32-S3-PICO-1 (LGA56), chip revision v0.2, 8 MB embedded flash (GD), 8 MB PSRAM |

The noscrypt and secp256k1-frost commits are the ones upstream CI pins (`.github/workflows/build.yml:180–190`).

### Commands

The smoke app requires the component name `libnostr-c`, which ESP-IDF takes from the folder name, so it does not build from a folder named `libnostr-c-esp32` ([UPSTREAM.md #6](UPSTREAM.md#6-esp-idf-smoke-app-only-builds-from-a-folder-named-libnostr-c)). As a workaround, the build ran in a temporary detached worktree named like CI's checkout, removed afterwards. Run in a terminal with ESP-IDF activated (`get_idf`):

```sh
cd ~/projects
git clone https://github.com/privkeyio/noscrypt.git
git -C noscrypt checkout 02e47e81eb9338d8cb9ae84a14a3992f00f4a651
git clone https://github.com/privkeyio/secp256k1-frost.git
git -C secp256k1-frost checkout 746dbbea5c34dde963c9bd55bc7474b2d50fa597
git -C libnostr-c-esp32 worktree add --detach ~/projects/libnostr-c 88ba821

cd ~/projects/libnostr-c/test/esp-idf
idf.py set-target esp32s3
idf.py build
idf.py -p <board A port> flash
idf.py -p <board A port> monitor
```

Find board A's port by matching the MAC printed by `esptool.py --port <port> chip_id` against the board table kept outside this repository. Flashing needed no buttons.

### Result

- **Build:** succeeded. `libnostr_test.bin` is 0x3aeb0 bytes (about 236 KB); 77% of the 1 MB app partition is free. 217 compiler warnings, all from secp256k1-frost (mostly `-Wunused-function`); none from libnostr-c or noscrypt.
- **Flash:** bootloader, partition table and app written and hash-verified.
- **Run:** `SMOKE-BEGIN`, 11 `ok:` lines (`nostr_init` through `nostr_keypair_generate`), `SMOKE-PASS`, then `main_task: Returned from app_main()`. About 70 ms from `Calling app_main()` to the return.

### Notes

- **Flash size 2 MB, board has 8 MB:** the app uses ESP-IDF's default flash size setting (2 MB), as in CI. At boot the bootloader reports `SPI Flash Size : 2MB` and the app warns `Detected size(8192k) larger than the size in the binary image header(2048k). Using the size in the binary image header.` Harmless for this test; left unchanged.
- **PSRAM not enabled:** the boot log lists only internal RAM (326 + 21 + 32 + 7 KiB), because the default configuration does not turn PSRAM on.
- **Not random-safe keys:** Wi-Fi and Bluetooth are off, so `nostr_keypair_generate` draws from a pseudo-random source (see the entropy note in `CLAUDE.md`). The test key is destroyed immediately.
- **Still uses noscrypt:** this is the baseline before M3 removes it from the ESP32 build.

## M2 Unity tests

**Summary:** the 11 smoke checks now run as Unity tests, and board A prints Unity's summary before `SMOKE-PASS`. A deliberately broken check was counted as 1 failure and turned the result into `SMOKE-FAIL-TOTAL 1`.

### Setup

| Item | Value |
|---|---|
| Date | 2026-11-19 |
| Code | `415490d`, built from the `libnostr-c-esp32` folder (no worktree, since the folder-name fix `587915b`). App version `v0.2.2-9-g7e49d80-dirty`: the build ran just before the commit, so it was stamped with the parent commit plus uncommitted changes. The built code is identical to `415490d` |
| Unity | v2.6.0, the copy bundled with ESP-IDF v5.5.6 (`components/unity`), MIT. Nothing downloaded |
| Everything else | as in the M2 hardware baseline above (ESP-IDF v5.5.6, esptool 4.13.0, same noscrypt and secp256k1-frost commits, board A) |

The tests are called with Unity's plain `UNITY_BEGIN` / `RUN_TEST` / `UNITY_END`, not ESP-IDF's interactive test menu, which waits for keyboard input and would make the CI run time out. CI's `build-esp-idf` job only checks the log for `SMOKE-PASS`, so it needs no change. The app still prints `SMOKE-BEGIN` first and `SMOKE-PASS` or `SMOKE-FAIL-TOTAL N` last.

### Commands

In a terminal with ESP-IDF activated (`get_idf`):

```sh
cd ~/projects/libnostr-c-esp32/test/esp-idf
idf.py build
esptool.py --port <board A port> chip_id
idf.py -p <board A port> flash
idf.py -p <board A port> monitor
```

### Results

| Run | App size | ELF fingerprint (start) | Unity summary | Last line |
|---|---|---|---|---|
| 1. Unity version | 0x3b640 bytes | `de73f926a` | `11 Tests 0 Failures 0 Ignored`, `OK` | `SMOKE-PASS` |
| 2. One check deliberately broken | 0x3b760 bytes | `b4ce8e309` | `11 Tests 1 Failures 0 Ignored`, `FAIL` | `SMOKE-FAIL-TOTAL 1` |
| 3. After undoing the break | 0x3b640 bytes | `de73f926a` | `11 Tests 0 Failures 0 Ignored`, `OK` | `SMOKE-PASS` |

- **Run 1:** one `PASS` line per test, `test_nostr_init` through `test_nostr_keypair_generate`. About 130 ms from `Calling app_main()` to the return. Unity adds about 2 KB to the app (0x3aeb0 before).
- **Run 2:** in `test_nostr_keypair_generate`, the expected result was flipped (`TEST_ASSERT_NOT_EQUAL_INT(NOSTR_OK, rc)`). Tests 1–10 still printed `PASS`, and the broken test printed `test_main.c:163:test_nostr_keypair_generate:FAIL: Expected 0 to be not equal to 0`. The broken version was never committed.
- **Run 3:** the file was compared byte for byte with a copy saved before the break (`cmp`: identical). Same app size and same ELF fingerprint as run 1, so the board is back on exactly the tested version.

## M2b real test counts

**Summary:** the Mac tests now use real Unity, so a failing check fails `ctest`. Counted for the first time, the hidden zap failure showed up as 1 failure; with its test data fixed, both builds pass 15/15 programs and 359 Unity checks with 0 failures.

### Setup

| Item | Value |
|---|---|
| Date | 2026-11-19 |
| Code | `esp32-noscrypt-free` at `4e19e6c`: the zap data fix and the seven `fix-test-unity` commits cherry-picked onto `086dbc6` (see [UPSTREAM.md](UPSTREAM.md) #3, #4, #7) |
| Unity | v2.6.0 (MIT), downloaded by CMake on first configure, pinned to commit `860062d51b2e8a75d150337b63ca2a472840d13c`, compiled from its single source as `nostr_unity` |
| Everything else | as in M1 (same Homebrew versions, Debug builds, linker workaround) |

### Commands

Same as M1, in new build folders so the M1 logs stay as they were. Run from the repository root:

```sh
cmake --fresh -S . -B build/m2b-host -DCMAKE_BUILD_TYPE=Debug \
  -DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/opt/homebrew/lib" \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/lib"
cmake --build build/m2b-host -j
ctest --test-dir build/m2b-host -V > build/m2b-host/ctest-verbose.log 2>&1

cmake --fresh -S . -B build/m2b-mbedtls -DCMAKE_BUILD_TYPE=Debug \
  -DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF -DNOSTR_CRYPTO_MBEDTLS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix mbedtls@3)" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L/opt/homebrew/lib" \
  -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/lib"
cmake --build build/m2b-mbedtls -j
ctest --test-dir build/m2b-mbedtls -V > build/m2b-mbedtls/ctest-verbose.log 2>&1
```

To build only the library, with no Unity download, add `-DBUILD_TESTING=OFF`.

### Results

Identical in both builds (OpenSSL and mbedTLS 3.6.8), except where noted. The two middle rows ran on branch `fix-test-unity`, before the zap data fix was added:

| Stage | `ctest` programs | Unity checks | Failures |
|---|---|---|---|
| M1, fallback runner | 15/15 "passed" | not counted | 1 hidden (`test_zap_verify`) |
| Real Unity, zap data not yet fixed (`6b4ceb3`) | 14/15 | 359 | 1: `test_zap_verify: Expected 0 Was 2` |
| One NIP-10 check deliberately broken, then undone (`651f747`, OpenSSL build only; 5 programs on Unity at that point) | 13/15 | 186 | 2: `nip10_tests` and `unit_tests` |
| Real Unity and zap data fixed (`4e19e6c`) | **15/15** | **359** | **0** |

- **Unity checks per program:** `unit_tests` 119, `relay_protocol_tests` 117, NIP-05 31, NIP-50 17, NIP-18 15, NIP-26 12, NIP-25 11, NIP-10 10, filter clone 10, NIP-49 7, random 6, thread safety 4. Each count matches the number of test functions its files define.
- **Not on Unity:** NIP-51 (19 checks), NIP-65 (13) and relay integration (9) use their own check macros, which already count failures. All passed.
- **The deliberate break** was in `tests/test_nip10.c`, which is built both on its own and into `test_runner`, so it failed two programs; it was undone and never committed.
- **ESP32 build unaffected:** the smoke app still builds from `4e19e6c` for `esp32s3` at 0x3b640 bytes, the same as the M2 Unity run, with no Unity download (the ESP-IDF path of `CMakeLists.txt` returns before `tests/`).
- **Warnings:** in our own code, still only `tests/test_thread_safety.c:156` from M1. Unity compiles without warnings.
- **Left out:** the 9 test files no default build compiles, among them NIP-44 and NIP-59 (switched on in M5 and M8).

### CI after M2b

On `4487bc8` (this fork's Actions runs 44643270900 and 44643270660), 11 of 12 jobs green; `build-windows` red:

- **Green:** the "CI" job (15/15; `unit_tests` 119 Tests 0 Failures; valgrind 0 errors; the zap `Assertion failed` line is gone), `build-esp-idf` in QEMU (`11 Tests 0 Failures`, `OK`, `SMOKE-PASS`), and all nine Linux and macOS jobs (15/15 each).
- **Red, `build-windows`:** 13/14 programs; `unit_tests` `114 Tests 5 Failures`. The Windows preset builds without cJSON, and five tests were never written for that case: three event tests expect the wrong error code, and two zap tests need JSON. The fallback runner had hidden them. Logged as [UPSTREAM.md](UPSTREAM.md) #9; the step's `|| echo` did not mask the failure, which corrected #5.
- **Fix:** `31e453d` (from `fix-tests-without-cjson`). Reproduced on macOS with `-DNOSTR_FEATURE_JSON_ENHANCED=OFF -DNOSTR_FEATURE_RELAY=OFF` in `build/m2b-nocjson`: `114 Tests 5 Failures` before, `114 Tests 0 Failures 2 Ignored` after. The two normal Mac builds stay at 15/15 and 359 tests, 0 failures. CI on `31e453d` and later: to be recorded after the next push.
