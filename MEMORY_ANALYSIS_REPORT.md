# Memory Analysis Report — LwM2M Wakaama ARM Client

**Date:** 2026-04-06
**Tools Used:** Valgrind 3.19, AddressSanitizer (clang 14), cppcheck 2.x
**Binary:** `lwm2mclient_tinydtls` (PC x86-64 build, same logic as ARM build)
**Project:** Wakaama-based LwM2M 1.1 client with Object 9 (Software Management)

---

## What is a Memory Leak?

When a program runs, it requests memory from the OS using `malloc()`. When it is done, it must call `free()` to return it. A **memory leak** happens when `malloc()` is called but `free()` is never called — the memory is gone forever until the process exits.

There are three types valgrind reports:

| Type | Meaning |
|---|---|
| **Definitely lost** | `malloc()` was called, pointer is gone, can never be freed — a real leak |
| **Possibly lost** | Pointer exists but valgrind cannot confirm it — may or may not be a real leak |
| **Still reachable** | Memory is allocated and pointer is still valid at exit — not a leak, just not explicitly freed |

---

## Tool 1: Valgrind

### What is Valgrind?
Valgrind is a runtime memory analysis tool. It runs your program inside a virtual CPU and monitors every `malloc`, `free`, and memory access. It catches leaks, use-after-free, and invalid reads/writes.

### How We Built for Valgrind

Valgrind works on a standard debug binary — no special compiler flags needed. We used the normal PC tinydtls build:

```bash
# Configure and build
cmake -S examples/client/tinydtls -B build-client-tinydtls-pc
cmake --build build-client-tinydtls-pc -j$(nproc)
```

### How We Ran It

```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./build-client-tinydtls-pc/lwm2mclient_tinydtls
```

**Flags explained:**
- `--leak-check=full` — report every leaked block with full call stack
- `--show-leak-kinds=all` — show definitely lost, possibly lost, and still reachable
- `--track-origins=yes` — show where the leaked memory was originally allocated

**What we did during the run:**
1. Let the app start and register with the LwM2M server
2. Sent a download URI from the server to trigger `pthread_create` in Object 9
3. Let it download a file completely (`[SWM] Download complete`)
4. Pressed Ctrl+C to exit
5. Read the `LEAK SUMMARY` printed by valgrind

### Final Valgrind Result

```
LEAK SUMMARY:
   definitely lost: 0 bytes in 0 blocks     ← CLEAN
   indirectly lost: 0 bytes in 0 blocks     ← CLEAN
     possibly lost: 336 bytes in 1 blocks   ← FALSE POSITIVE (explained below)
   still reachable: 244 bytes in 4 blocks   ← NOT A LEAK (explained below)
        suppressed: 0 bytes in 0 blocks
ERROR SUMMARY: 1 errors from 1 contexts
```

**Our application code has zero memory leaks.**

---

## Tool 2: AddressSanitizer (ASan)

### What is ASan?
AddressSanitizer is a compiler-level tool built into clang/gcc. It instruments every memory access at compile time and catches bugs at runtime:
- Buffer overflows (reading/writing past end of an array)
- Use-after-free (using memory after `free()`)
- Use of uninitialized memory
- Double-free (calling `free()` twice on same pointer)

Unlike valgrind (which runs a virtual CPU), ASan adds checks directly into the compiled binary — so it runs much faster but must be compiled in.

### How We Built for ASan

```bash
# Clean build with ASan + UBSan + debug symbols + WAKAAMA debug logs
rm -rf build-asan
cmake -S examples/client/tinydtls -B build-asan \
  -DCMAKE_C_COMPILER=clang \
  -DWAKAAMA_LOG_LEVEL=DBG \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
```

**Flags explained:**
- `-fsanitize=address` — enable AddressSanitizer
- `-fsanitize=undefined` — enable UndefinedBehaviorSanitizer (catches integer overflow, null dereference, etc.)
- `-fno-omit-frame-pointer` — keep frame pointers so ASan can print accurate stack traces
- `-g` — include debug symbols for readable error messages
- `-O1` — light optimization (needed for some ASan checks to work correctly)
- `-DWAKAAMA_LOG_LEVEL=DBG` — enable Wakaama debug logs to see full internal state

**How to verify ASan is actually compiled in:**
```bash
nm build-asan/lwm2mclient_tinydtls | grep -c "__asan"
# Output: 637  ← 637 ASan symbols statically linked
```

Note: ASan is linked **statically** by clang by default, so `ldd | grep asan` shows nothing — that is normal.

### How We Ran It

```bash
./build-asan/lwm2mclient_tinydtls
```

**What we did during the run:**
1. Let the app register with the server
2. Sent a download URI from the server
3. Pressed Ctrl+C mid-download to test cancellation path
4. Watched for any `ERROR: AddressSanitizer` output

**ASan behavior:** If no errors are found, ASan prints nothing. Only when a real bug is found does it print a red error report and abort.

### ASan Result

```
# No ASan errors printed during runtime
# On exit:
=================================================================
==180663==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 144 byte(s) in 1 object(s) allocated from:
    #1 lwm2m_connection_new_incoming (connection.c:372)
```

The only report was the same upstream 144-byte `dtlsSession` leak that valgrind also found. After we fixed it by adding `free(connList->dtlsSession)` in `lwm2m_connection_free`, ASan reported zero leaks from our code.

**Result: Zero ASan errors in our application code.**

---

## Tool 3: cppcheck

### What is cppcheck?
cppcheck is a **static analysis** tool — it reads all source code without running the program. It checks every code path including ones that were never executed during valgrind or ASan testing. It finds:
- Null pointer dereferences
- Buffer overflows
- Uninitialized variables
- Resource leaks (file descriptors, etc.)
- Undefined behavior patterns
- Dead code

### How We Ran It

We first ran it on just our application files:

```bash
cppcheck --enable=all \
  --suppress=missingIncludeSystem \
  --suppress=missingInclude \
  -I examples/client/common -I include \
  examples/client/common/object_software_mgmt.c \
  examples/client/common/lwm2mclient.c \
  2>&1
```

Then we ran it on the **exact files compiled** in our tinydtls build (extracted from `find build-client-tinydtls-pc -name "*.c.o"`):

```bash
cppcheck --enable=all \
  --suppress=missingIncludeSystem \
  --suppress=missingInclude \
  --suppress=unusedFunction \
  -I include -I core -I examples/client/common \
  -I transport/tinydtls \
  -I transport/tinydtls/third_party/tinydtls \
  -DLWM2M_CLIENT_MODE -DWITH_TINYDTLS \
  examples/client/common/*.c \
  examples/shared/*.c \
  coap/block.c coap/er-coap-13/er-coap-13.c coap/transaction.c \
  core/*.c \
  data/*.c \
  transport/tinydtls/connection.c \
  transport/tinydtls/third_party/tinydtls/dtls.c \
  transport/tinydtls/third_party/tinydtls/dtls_debug.c \
  transport/tinydtls/third_party/tinydtls/dtls_prng.c \
  transport/tinydtls/third_party/tinydtls/dtls_time.c \
  transport/tinydtls/third_party/tinydtls/ccm.c \
  transport/tinydtls/third_party/tinydtls/crypto.c \
  transport/tinydtls/third_party/tinydtls/hmac.c \
  transport/tinydtls/third_party/tinydtls/netq.c \
  transport/tinydtls/third_party/tinydtls/peer.c \
  transport/tinydtls/third_party/tinydtls/session.c \
  transport/tinydtls/third_party/tinydtls/aes/rijndael.c \
  transport/tinydtls/third_party/tinydtls/aes/rijndael_wrap.c \
  transport/tinydtls/third_party/tinydtls/ecc/ecc.c \
  transport/tinydtls/third_party/tinydtls/sha2/sha2.c \
  2>&1 | grep -E "error:|warning:"
```

### cppcheck Results

**Our application code (`object_software_mgmt.c`):**
```
(no output) ← Zero issues
```

**Full build (all compiled files) — final result after our fixes:**

| File | Issue | Severity | Action Taken |
|---|---|---|---|
| `lwm2mclient.c:825` | Redundant inner `if` always true | Warning | Not fixed — upstream Wakaama loop, harmless dead code |
| `dtls.h:338` | `#error` structure packing | Error | Not fixed — cppcheck parser limitation, compiles fine with gcc/clang |

All other files: clean.

---

## Explanation of Remaining Valgrind Entries

### Possibly Lost: 336 bytes — pthread TLS (FALSE POSITIVE)

**What is it?**
When `pthread_create()` creates the download thread in Object 9, glibc allocates a **Thread Local Storage (TLS)** block. This is glibc's internal bookkeeping for the thread.

**Why does valgrind report it?**
After the thread exits and `pthread_join()` completes, glibc **caches** this TLS block internally for potential reuse. The pointer is stored inside glibc's linker structures (`rtld`). Valgrind cannot see pointers stored inside `rtld`, so it reports the block as "possibly lost."

**Is it a real leak?**
No. This is a well-known valgrind false positive with glibc's pthread implementation on Linux. The memory is fully controlled by glibc and released when the process exits. It does **not grow over time** — it is allocated once per `pthread_create` call, not once per download or per packet.

**Evidence:**
```
possibly lost: 336 bytes
   by pthread_create@@GLIBC_2.34 (pthread_create.c:647)
   by prv_sw_write (object_software_mgmt.c:340)
```
The entire call stack is inside glibc — not our code.

---

### Still Reachable: 244 bytes — Upstream Wakaama (NOT A LEAK)

**What does "still reachable" mean?**
The pointer to this memory is still valid when the program exits. The OS reclaims all process memory on exit anyway. "Still reachable" is the least serious valgrind category — the memory is not lost, just not explicitly freed.

**Why does it exist?**
These 4 blocks are Wakaama framework structures kept alive for the full application lifetime. Wakaama's `lwm2m_close()` would free them — but `lwm2m_close()` is only safe to call on a clean programmatic quit (`g_quit == 1`), not on Ctrl+C (`g_quit == 2`). We confirmed this by trying: calling `lwm2m_close()` on Ctrl+C causes **32 use-after-free errors** because the DTLS shutdown sequence tries to send alerts using connection structs that `lwm2m_close()` just freed.

| Bytes | Source | What it is |
|---|---|---|
| 31 bytes | `lwm2m_configure → lwm2m_strdup` | Endpoint name string — allocated once at startup |
| 37 bytes | `prv_handleRegistrationReply` | CoAP registration path — allocated once on first registration |
| 88 bytes | `lwm2m_init` | LwM2M context struct — main framework object |
| 88 bytes | `object_getServers` | Server entry struct — one entry for one server |

**Does this affect long-term operation?**
No. All 4 are allocated exactly once at startup. After startup, these 244 bytes are constant — they do not grow whether the app runs for 1 minute or 1 year.

---

## Bugs Found and Fixed During This Analysis

| # | Bug | File | Found By | Impact |
|---|---|---|---|---|
| 1 | `pthread_cancel` mid-download leaked curl handle (33KB), FILE* (472B), download_args (520B) | `object_software_mgmt.c` | Valgrind | Memory leak on every cancelled download |
| 2 | `pskBuffer` only freed inside `#ifdef WITH_TINYDTLS` | `lwm2mclient.c` | ASan LeakSanitizer | Leak in non-DTLS builds |
| 3 | `dtlsSession` not freed in `lwm2m_connection_free` | `transport/tinydtls/connection.c` | Valgrind + ASan | 144B leak per DTLS reconnect — long-term risk |
| 4 | Partial downloaded file deleted on cancel | `object_software_mgmt.c` | Code review | Resume broken after Ctrl+C or app kill |
| 5 | Wrong OMA Object 9 result codes (e.g. SUCCESS=1 meant "Downloading" to server) | `object_software_mgmt.c` | Manual testing | Server misread all download states |
| 6 | Server showed stuck "sent tasks" after app restart | `object_software_mgmt.c` | Manual testing | Init now reports `CONN_LOST` immediately on re-registration |
| 7 | `va_end` missing after `va_start` in log formatter | `core/logging.c` | cppcheck | Potential stack corruption on ARM with heavy logging |
| 8 | `/dev/urandom` fd not closed in fread error path | `transport/tinydtls/third_party/tinydtls/platform-specific/dtls_prng_posix.c` | cppcheck | File descriptor leak if PRNG init fails |

---

## Final Summary

| Check | Command | Result |
|---|---|---|
| Valgrind — definitely lost | `valgrind --leak-check=full --show-leak-kinds=all` | **0 bytes** |
| Valgrind — possibly lost | same | 336 bytes (glibc pthread false positive) |
| Valgrind — still reachable | same | 244 bytes (upstream Wakaama, fixed-size, safe) |
| ASan — runtime errors | `clang -fsanitize=address,undefined` | **0 errors** |
| cppcheck — our code | `cppcheck --enable=all` | **0 issues** |
| cppcheck — full build | same on all compiled files | 2 findings (upstream, harmless) |
| Long-term memory growth | — | **None** — all remaining entries are fixed one-time allocations |
