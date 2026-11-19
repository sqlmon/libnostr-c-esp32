/*
 * On-target smoke test.
 *
 * Built by CI and, under QEMU, actually run. That distinction is the point: the
 * mbedTLS RNG path is compiled only for ESP targets, and until this ran it had
 * never executed anywhere. Three defects reached main through that gap, and
 * each has a case here:
 *
 *   - a self-deadlock, where nostr_init() held its context lock and the RNG
 *     took the same non-recursive lock
 *   - an unsynchronised mbedtls_ctr_drbg_random() draw, whose internal mutex
 *     compiles out because ESP-IDF leaves MBEDTLS_THREADING_C off
 *   - a >1024-byte regression, since mbedtls_ctr_drbg_random() refuses more
 *     than MBEDTLS_CTR_DRBG_MAX_REQUEST and does not split internally
 *
 * The host suite covers the same logic against mbedTLS 2.x. This covers the 3.x
 * ESP-IDF actually ships, on the FreeRTOS scheduler the race needs.
 *
 * Each check is a Unity test (the Unity bundled with ESP-IDF), so failures are
 * counted and summarised. The tests run in order and later ones use the state
 * earlier ones leave behind, as the checks did before.
 *
 * The result is one line. CI greps for it, so a silent hang or a crash is a
 * failure rather than a pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "nostr.h"
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define DRAW_TASKS 4
#define DRAWS_PER_TASK 16
#define DRAW_LEN 32

static uint8_t g_draws[DRAW_TASKS][DRAWS_PER_TASK][DRAW_LEN];
static int g_task_failures[DRAW_TASKS];
static SemaphoreHandle_t g_done;
static int g_started;
static uint8_t g_big[4096];

void setUp(void) {}
void tearDown(void) {}

static int all_zero(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i]) return 0;
    }
    return 1;
}

static void draw_task(void *arg) {
    int t = (int)(intptr_t)arg;
    for (int i = 0; i < DRAWS_PER_TASK; i++) {
        if (nostr_random_bytes(g_draws[t][i], DRAW_LEN) != 1) {
            g_task_failures[t]++;
        }
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* Deadlock case: nostr_init() seeds through the same path the RNG uses. A
 * regression here hangs rather than fails, which is why CI bounds the run. */
static void test_nostr_init(void) {
    TEST_ASSERT_EQUAL_INT(NOSTR_OK, nostr_init());
}

static void test_draw_after_init_does_not_deadlock_and_fills_the_buffer(void) {
    uint8_t buf[DRAW_LEN];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(1, nostr_random_bytes(buf, sizeof(buf)));
    TEST_ASSERT_FALSE_MESSAGE(all_zero(buf, sizeof(buf)), "buffer is all zeros");
}

/* Chunking case: one request past MBEDTLS_CTR_DRBG_MAX_REQUEST. An
 * unchunked implementation fails here while passing every 32-byte draw. */
static void test_draw_of_4096_bytes_succeeds(void) {
    memset(g_big, 0, sizeof(g_big));
    TEST_ASSERT_EQUAL_INT(1, nostr_random_bytes(g_big, sizeof(g_big)));
}

static void test_bytes_past_the_1024_byte_drbg_limit_are_filled(void) {
    TEST_ASSERT_FALSE(all_zero(g_big + 1024, sizeof(g_big) - 1024));
}

/* Race case: concurrent draws on a dual-core part. Without an external lock
 * two tasks can be handed the same AES-CTR block, so duplicates are the
 * observable symptom. */
static void test_semaphore_created(void) {
    memset(g_draws, 0, sizeof(g_draws));
    memset(g_task_failures, 0, sizeof(g_task_failures));
    g_done = xSemaphoreCreateCounting(DRAW_TASKS, 0);
    TEST_ASSERT_NOT_NULL(g_done);
}

static void test_all_draw_tasks_were_created(void) {
    g_started = 0;
    /* The tasks give this semaphore when they finish. */
    TEST_ASSERT_NOT_NULL_MESSAGE(g_done, "no semaphore to report on");
    for (int t = 0; t < DRAW_TASKS; t++) {
        /* 4096 is generous on purpose: ctr_drbg_reseed_internal puts a
         * 384-byte seed buffer on the caller's stack, plus the entropy gather. */
        if (xTaskCreate(draw_task, "draw", 4096, (void *)(intptr_t)t, 5, NULL) == pdPASS) {
            g_started++;
        }
    }
    /* Unchecked, a failed create meant one fewer xSemaphoreGive and a
     * portMAX_DELAY wait that never returns: a CI timeout with no message,
     * which is the failure mode this whole file exists to avoid. */
    TEST_ASSERT_EQUAL_INT(DRAW_TASKS, g_started);
}

static void test_every_draw_task_finished_within_the_timeout(void) {
    int joined = 0;
    for (int t = 0; t < g_started; t++) {
        /* Bounded rather than portMAX_DELAY, so a task that hangs reports a
         * missing result instead of stalling the run to the job timeout. */
        if (xSemaphoreTake(g_done, pdMS_TO_TICKS(10000)) == pdTRUE) {
            joined++;
        }
    }
    TEST_ASSERT_EQUAL_INT(g_started, joined);
}

static void test_every_concurrent_draw_reported_success(void) {
    int draw_errors = 0;
    for (int t = 0; t < DRAW_TASKS; t++) draw_errors += g_task_failures[t];
    TEST_ASSERT_EQUAL_INT(0, draw_errors);
}

static void test_no_concurrent_draw_returned_all_zeros(void) {
    const int total = DRAW_TASKS * DRAWS_PER_TASK;
    const uint8_t *flat = (const uint8_t *)g_draws;
    int zero = 0;
    for (int i = 0; i < total; i++) {
        if (all_zero(flat + (size_t)i * DRAW_LEN, DRAW_LEN)) zero++;
    }
    TEST_ASSERT_EQUAL_INT(0, zero);
}

static void test_no_two_concurrent_draws_returned_identical_bytes(void) {
    const int total = DRAW_TASKS * DRAWS_PER_TASK;
    const uint8_t *flat = (const uint8_t *)g_draws;
    int dup = 0;
    for (int i = 0; i < total; i++) {
        for (int j = i + 1; j < total; j++) {
            if (memcmp(flat + (size_t)i * DRAW_LEN,
                       flat + (size_t)j * DRAW_LEN, DRAW_LEN) == 0) dup++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, dup);
}

/* The public API the RNG feeds. */
static void test_nostr_keypair_generate(void) {
    nostr_keypair kp;
    int rc = nostr_keypair_generate(&kp);
    nostr_keypair_destroy(&kp);
    TEST_ASSERT_EQUAL_INT(NOSTR_OK, rc);
}

void app_main(void) {
    printf("SMOKE-BEGIN\n");

    UNITY_BEGIN();
    RUN_TEST(test_nostr_init);
    RUN_TEST(test_draw_after_init_does_not_deadlock_and_fills_the_buffer);
    RUN_TEST(test_draw_of_4096_bytes_succeeds);
    RUN_TEST(test_bytes_past_the_1024_byte_drbg_limit_are_filled);
    RUN_TEST(test_semaphore_created);
    RUN_TEST(test_all_draw_tasks_were_created);
    RUN_TEST(test_every_draw_task_finished_within_the_timeout);
    RUN_TEST(test_every_concurrent_draw_reported_success);
    RUN_TEST(test_no_concurrent_draw_returned_all_zeros);
    RUN_TEST(test_no_two_concurrent_draws_returned_identical_bytes);
    RUN_TEST(test_nostr_keypair_generate);
    int failures = UNITY_END();

    if (failures == 0) {
        printf("SMOKE-PASS\n");
    } else {
        printf("SMOKE-FAIL-TOTAL %d\n", failures);
    }
}
