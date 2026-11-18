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

**Verify these against the current code at the start of M1.** They were read from an earlier copy of the files, and the README says the ESP32 build uses mbedTLS, which may or may not conflict with the first point.

- **Existing test setup:** `tests/` holds about 35 Unity test files run by `ctest`. `test/esp-idf/` is a small ESP-IDF smoke-test app that prints `SMOKE-PASS`. GitHub Actions (`.github/workflows/build.yml`) already has a `build-mbedtls` job and a `build-esp-idf` job that builds the smoke app for esp32s3 and runs it in QEMU (ESP-IDF v5.4.1). Extend these rather than creating new ones.
- **Style:** follow the actual code and commit history where `CONTRIBUTING.md` disagrees (type names without `_t`, plain-sentence commit messages).
- **Stack:** call `nostr_init()` early, from a task with at least 4 KB of stack; the first random-number draw needs about 500 bytes extra.
- In the ESP32 build recipe (the `if(ESP_PLATFORM)` block of `CMakeLists.txt`), noscrypt is hard-wired: `REQUIRES ... noscrypt` and `HAVE_NOSCRYPT 1`. There is no switch to remove it yet.
- `src/key.c` and `src/event.c` already contain complete `#elif defined(HAVE_SECP256K1)` paths for key generation, key agreement, signing and verifying.
- **Bug:** the secp256k1 path of `nostr_key_ecdh` calls `secp256k1_ecdh(..., NULL, NULL)`, which hashes the shared point with SHA-256. NIP-44 requires the **unhashed x-coordinate**. Fix with a custom hash function, proven by the NIP-44 conversation-key test vectors.
- `src/nip44.c` is entirely inside `#ifdef NOSTR_FEATURE_CRYPTO_NOSCRYPT`; without noscrypt, NIP-44 returns `NOSTR_ERR_NOT_SUPPORTED`. Its padding, base64 and payload framing are reusable; the encryption core (key derivation, ChaCha20, HMAC) must be rebuilt on mbedTLS. ChaCha20 and HKDF may need enabling in the test app's `sdkconfig.defaults`.
- The ESP32 build has no relay **client** and no NIP-59; the relay files it includes are for running a relay.
- **Entropy:** ESP32 random numbers are only truly random while Wi-Fi/Bluetooth runs or after `bootloader_random_enable()`. Generate keys only then (see the comment in `nostr_random_bytes`).

## Milestones (in order)

1. **M1** Baseline: verify the known facts above, build on the Mac as-is (noscrypt off), run the existing tests, record results.
2. **M2** Run the existing `test/esp-idf` smoke app on board A (`SMOKE-PASS`), then extend it with Unity tests.
3. **M3** Noscrypt-free ESP32 build (secp256k1 for keys and signing). BIP-340 test vectors and a known Nostr event pass on Mac and board A.
4. **M4** Key-agreement fix (raw x-coordinate). NIP-44 conversation-key vectors pass.
5. **M5** NIP-44 core on mbedTLS. All NIP-44 vectors pass, including invalid ones that must fail.
6. **M6** Independent review of M3–M5 by a separate session that has not seen the work.
7. **M7** Relay client (secure WebSockets, subscribe, publish, reconnect). Board A → relay → board B.
8. **M8** NIP-59 gift wrap / unwrap. Board A sends, board B opens; tampered wraps rejected.
9. **M9** Interoperability with nostr-tools (Node.js script on the Mac), both directions.
10. **M10** Measure memory, flash and timing; NVS key storage; overnight stability run on both boards.
11. **M11** Example project and ESP32 README section, including the entropy warning and a "not independently audited" note.

Right after M3, extend the existing `build.yml` workflow: make the `build-mbedtls` job run the new test vectors, and add an ESP-IDF v5.5.6 build alongside the existing v5.4.1 one. Never merge with a red check.

## Working with the developer

Go step by step. Explain each change in a sentence or two before making it, show commands before running them, and wait for results that need eyes on the hardware. The developer reports what the boards show.
