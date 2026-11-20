# libnostr-c: ESP32 noscrypt-free fork

This repository is a fork of [privkeyio/libnostr-c](https://github.com/privkeyio/libnostr-c) (MIT). The goal: a reusable ESP-IDF component that can receive and open private, gift-wrapped Nostr messages on ESP32 chips, **with no noscrypt (LGPL) code**, so any project can use it under MIT.

Personal working style, board details and the Mac environment live in `~/projects/CLAUDE.md`, which loads automatically. This file holds only project facts, because the repository is public.

## Rules

- **One milestone at a time** (see the list below). A milestone is done only when its test passes and the work is committed.
- **Never write cryptographic primitives.** Connect trusted libraries (secp256k1 for curve operations, mbedTLS for SHA-256, HKDF, ChaCha20, HMAC). Prove every crypto change against official test vectors.
- **Compare MACs in constant time** and wipe secrets from memory after use, following the existing `secure_wipe` / `nostr_constant_time_memcmp` patterns.
- **No noscrypt in the ESP32 build.** Before adding any new dependency, check its license and report it; only permissive licenses (MIT, Apache 2.0, BSD) are allowed.
- **Keep it general.** No code specific to any one application.
- **Follow the existing code style** and structure of libnostr-c (naming, error codes, `#ifdef` backends).
- **No secrets** in the repository. It is public.
- **Upstream etiquette:** keep `main` identical to `upstream/main`. Work on `esp32-noscrypt-free`. Never push to `upstream`. Changes intended for upstream are kept small and separate.
- **When you find a bug in the original libnostr-c code**, add it to `docs/esp32/UPSTREAM.md` the same day, and tell me. Fixes meant for upstream are made on their own branch from `upstream/main`, one fix per branch, with a test that fails before the fix and passes after.

## Contribution policy

- **Fixes to existing upstream code:** on their own branch from `upstream/main`, one fix per branch, then cherry-picked onto `esp32-noscrypt-free`. Logged in `docs/esp32/UPSTREAM.md` and offered upstream.
- **Exception, stacked fixes:** if a fix only makes sense on top of another unmerged fix branch, base it on that branch, note the dependency in `UPSTREAM.md`, and offer them together in order.
- **New features upstream has said it wants** (check issue #75 and any later replies): built the same way, as separate branches, and offered.
- **New features for this fork's goals:** built directly on `esp32-noscrypt-free`. Not prepared for upstream unless the maintainer asks.
- **Fork-only files** (`CLAUDE.md`, `docs/esp32/`, the CI trigger for this branch): never offered upstream.
- **Upstream issues are short:** the problem, the key evidence and the questions, in a few lines each. Details go in the pull request descriptions.
- **Periodically sync `main` with upstream** and merge it into `esp32-noscrypt-free`, so the fork doesn't drift.

## Environment

- macOS, ESP-IDF **v5.5.6**, target **esp32s3**. ESP-IDF must be activated in the terminal before `idf.py` works.
- Host (Mac) builds use CMake with the secp256k1 backend. Use `-DNOSTR_FEATURE_CRYPTO_NOSCRYPT=OFF`, and `-DNOSTR_CRYPTO_MBEDTLS=ON` to exercise the same mbedTLS code paths the ESP32 build uses.
- Homebrew versions on the Mac: **mbedTLS 3.x** (`mbedtls@3`, to match ESP-IDF 5.5; plain `mbedtls` is 4.x, whose API differs, so point CMake at `$(brew --prefix mbedtls@3)`), **OpenSSL 4** (`openssl@4` 4.1.0, the default) plus **OpenSSL 3** (`openssl@3` 3.5.9, keg-only); if the build can't find OpenSSL or is incompatible with 4, pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`, secp256k1 0.8.0, cJSON 1.7.19, libwebsockets 5.1.0, Python 3.13.
- Hardware: Waveshare ESP32-S3-ePaper-1.54 V2 boards (ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB PSRAM).

## The two-board setup

Both boards are plugged into the Mac at once, each on its own serial port: board **A sends**, board **B receives**. Their ports and MAC addresses are in the board table in `~/projects/CLAUDE.md` (kept out of this public repository).

- Find ports with `ls /dev/cu.*`. Confirm which board is which with `esptool.py --port <port> chip_id` (it prints the MAC). Port names can change between plug-ins, so check them at the start of each session.
- Always pass the port explicitly: `idf.py -p <port> flash monitor`. Never rely on automatic port detection with two boards attached.
- Use a separate terminal window per board for flashing and logs.
- Exit the monitor with **Ctrl+]**.
- If a board won't enter download mode: hold **both** the sun (BOOT) and power buttons while plugging it in, then release. After a manual download-mode flash, the board may restart into download mode again (`boot:0x0 (DOWNLOAD...)`); unplug and replug with no buttons held.

## Known facts about the original code

**Checked against upstream `e07af71` during M1** (see `docs/esp32/BASELINE.md`).

- **Existing test setup:** `tests/` holds 35 test files; by default 26 are compiled into 15 test programs that `ctest` runs. Unity is not used: the tests fall back to their own checker, which doesn't count failures, so "passed" only means "didn't crash" (see `docs/esp32/BASELINE.md`). `test/esp-idf/` is a small ESP-IDF smoke-test app that prints `SMOKE-PASS`. GitHub Actions (`.github/workflows/build.yml`) already has a `build-mbedtls` job and a `build-esp-idf` job that builds the smoke app for esp32s3 and runs it in QEMU (ESP-IDF v5.4.1). Extend these rather than creating new ones.
- **Style:** follow the actual code and commit history where `CONTRIBUTING.md` disagrees (type names without `_t`, plain-sentence commit messages).
- **Stack:** call `nostr_init()` early, from a task with at least 4 KB of stack. The first random-number draw seeds the generator, and that needs extra stack: upstream measured it under QEMU on ESP32-S3 (`README.md`, "ESP-IDF stack requirement"), where a 2 KB task that seeds crashes and a 3 KB task passes, so the limit is between 2 and 3 KB. 4 KB is recommended for margin. Not yet measured on our boards.
- In the ESP32 build recipe (the `if(ESP_PLATFORM)` block of `CMakeLists.txt`), noscrypt is hard-wired: `REQUIRES ... noscrypt` and `HAVE_NOSCRYPT 1`. There is no switch to remove it yet.
- `src/key.c` and `src/event.c` already contain complete `#elif defined(HAVE_SECP256K1)` paths for key generation, key agreement, signing and verifying.
- **Bug:** the secp256k1 path of `nostr_key_ecdh` calls `secp256k1_ecdh(..., NULL, NULL)`, which hashes the shared point with SHA-256. NIP-44 requires the **unhashed x-coordinate**. Fix with a custom hash function, proven by the NIP-44 conversation-key test vectors.
- `src/nip44.c` is entirely inside `#ifdef NOSTR_FEATURE_CRYPTO_NOSCRYPT`; without noscrypt, NIP-44 returns `NOSTR_ERR_NOT_SUPPORTED`. Its padding, base64 and payload framing are reusable; the encryption core (key derivation, ChaCha20, HMAC) must be rebuilt on mbedTLS. ChaCha20 and HKDF may need enabling in the test app's `sdkconfig.defaults`.
- The ESP32 build has no relay **client** and no NIP-59; the relay files it includes are for running a relay.
- **Entropy:** ESP32 random numbers are only truly random while Wi-Fi/Bluetooth runs or after `bootloader_random_enable()`. Generate keys only then (see the comment in `nostr_random_bytes`).

## Milestones (in order)

1. **M1** Baseline: verify the known facts above, build on the Mac as-is (noscrypt off), run the existing tests, record results.
2. **M2** Run the existing `test/esp-idf` smoke app on board A (`SMOKE-PASS`), then extend it with Unity tests.
3. **M2b** Real test counts: switch the Mac tests to real Unity (v2.6.0, downloaded by CMake, pinned by commit) so failing checks count; a deliberately broken check must make ctest fail.
4. **M3** Noscrypt-free ESP32 build (secp256k1 for keys and signing). BIP-340 test vectors and a known Nostr event pass on Mac and board A.
5. **M4** Key-agreement fix (raw x-coordinate). NIP-44 conversation-key vectors pass.
6. **M5** NIP-44 core on mbedTLS. All NIP-44 vectors pass, including invalid ones that must fail.
7. **M6** Independent review of M3–M5 by a separate session that has not seen the work.
8. **M7** Relay client (secure WebSockets, subscribe, publish, reconnect). Board A → relay → board B.
9. **M8** NIP-59 gift wrap / unwrap. Board A sends, board B opens; tampered wraps rejected.
10. **M9** Interoperability with nostr-tools (Node.js script on the Mac), both directions.
11. **M10** Measure memory, flash and timing; NVS key storage; overnight stability run on both boards.
12. **M11** Example project and ESP32 README section, including the entropy warning and a "not independently audited" note.

Right after M3, extend the existing `build.yml` workflow: make the `build-mbedtls` job run the new test vectors, and add an ESP-IDF v5.5.6 build alongside the existing v5.4.1 one. Never merge with a red check.

## Working with the developer

Go step by step. Explain each change in a sentence or two before making it, show commands before running them, and wait for results that need eyes on the hardware. The developer reports what the boards show.
