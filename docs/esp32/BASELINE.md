# M1 baseline: host build without noscrypt

**Summary:** Both Mac builds (secp256k1 + OpenSSL, and secp256k1 + mbedTLS) compile and `ctest` reports 15/15 passed, but the fallback test runner does not count failures, and one real failure (`test_zap_verify`) is hidden in both.

## Setup

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

## Commands

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

## Results

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

## Other findings

- **Smoke app paths:** `test/esp-idf/CMakeLists.txt` expects `../../../noscrypt` and `../../../secp256k1-frost` next to the repository. Neither exists in this checkout's parent folder. To be handled in M2.
- **macOS CI uses a different secp256k1 setup:** upstream's `build-macos` job runs the same 15 test programs (secp256k1 and noscrypt variants), but builds secp256k1 from source into `/usr/local/lib`, which the macOS linker searches by default. That is why CI never hits the linker problem above. (The `-DBUILD_TESTS=OFF` in that job belongs to the step that builds noscrypt, not libnostr-c.) Its `ctest --output-on-failure` prints nothing for programs it counts as passed, so a hidden failure would not show there.
- **Mac mbedTLS is 3.6.8:** `CMakeLists.txt:178` says the host mbedTLS is 2.x. That holds for upstream's Linux CI, not here. ESP-IDF 5.5.6 ships 3.6.7, so the Mac is closer to the ESP32 build than CI is.
- **26 of 35 test files compiled, into 15 test programs:** `test_runner` alone is built from 12 files and `test_relay_protocol` from 5. The 9 files not compiled are the NIP-04, NIP-44 (2), NIP-59, HD-key (`test_hd_key`, `test_hd_integration`, `test_nip06`) and NIP-47 (2) tests, because `NOSTR_FEATURE_NIP04/44/47/59` and `HD_KEYS` default to `OFF` (and NIP-44 needs noscrypt). Of the two NIP-47 files, only `test_nip47_minimal.c` is wired up even when NIP-47 is on; `test_nip47.c` is never built.
- **utf8proc not installed:** optional; without it BIP-39 rejects non-ASCII mnemonics.
- **Schnorr label:** the failed configure check only changes the text returned by `nostr_feature_crypto_backend_info()` ("secp256k1 (limited - no schnorr)"). Signing and verifying do not depend on it.
