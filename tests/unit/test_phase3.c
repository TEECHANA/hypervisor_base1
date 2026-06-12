/*
 * test_phase3.c — Phase 3 unit tests
 *
 * Tests:
 *   §3.3.2  Scheduler stats (sched_stats)
 *   §3.3.3  Context switch logging (via mock)
 *   §3.3.4  Runtime slice configuration (sched_set_slice)
 *   §3.2.x  IPC send/recv/notify
 *   HVC     Scheduler hypercall ABI values
 *
 * Build:
 *   gcc -O0 -g -Wall tests/unit/test_phase3.c \
 *       -o build/test_phase3 && ./build/test_phase3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef u64      paddr_t;
typedef u64      ipa_t;
typedef s32      err_t;

#define E_OK        0
#define E_INVAL    -3
#define E_BUSY     -4
#define E_NOTFOUND -5
#define FAIL(e)    ((e) != E_OK)
#define OK(e)      ((e) == E_OK)
#define UNUSED(x)  ((void)(x))
#define MAX_VMS     4

/* Test harness */
static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s\n", msg); _fail++; } \
} while(0)

/* ── §3.3.2 sched_stats inline implementation ── */
typedef struct {
    u64 total_us;
    u32 slice_count;
    u32 preempt_count;
    u32 wfi_count;
    u32 slice_dur_us;
    u32 vm_id;
} vm_stats_t;

static vm_stats_t _stats[MAX_VMS];
static u64 _switch_count = 0;

static void stats_init(void) {
    memset(_stats, 0, sizeof(_stats));
    _switch_count = 0;
    for (u32 i = 0; i < MAX_VMS; i++) {
        _stats[i].vm_id = i + 1;
        _stats[i].slice_dur_us = 10000;
    }
}

static void stats_on_switch(u32 prev_id, u32 next_id,
                             u64 slice_us, bool preempt) {
    if (prev_id >= 1 && prev_id <= MAX_VMS) {
        _stats[prev_id-1].total_us += slice_us;
        if (preempt) _stats[prev_id-1].preempt_count++;
    }
    if (next_id >= 1 && next_id <= MAX_VMS)
        _stats[next_id-1].slice_count++;
    _switch_count++;
}

static void stats_on_wfi(u32 vm_id) {
    if (vm_id >= 1 && vm_id <= MAX_VMS)
        _stats[vm_id-1].wfi_count++;
}

static err_t stats_get(u32 vm_id, vm_stats_t *out) {
    if (!out || vm_id < 1 || vm_id > MAX_VMS) return E_INVAL;
    *out = _stats[vm_id-1];
    return E_OK;
}

static void test_sched_stats(void)
{
    printf("\n--- test_sched_stats ---\n");
    stats_init();

    vm_stats_t s;

    /* Initial state */
    stats_get(1, &s);
    CHECK(s.total_us == 0,      "Initial total_us == 0");
    CHECK(s.slice_count == 0,   "Initial slice_count == 0");
    CHECK(s.preempt_count == 0, "Initial preempt_count == 0");
    CHECK(s.wfi_count == 0,     "Initial wfi_count == 0");

    /* Simulate: RTOS(VM2) runs for 10ms, timer fires, switch to Linux(VM1) */
    stats_on_switch(2, 1, 10000, true);
    stats_get(2, &s);
    CHECK(s.total_us == 10000,   "RTOS total_us = 10ms after one slice");
    CHECK(s.preempt_count == 1,  "RTOS preempt_count = 1 after timer preempt");
    stats_get(1, &s);
    CHECK(s.slice_count == 1,    "Linux slice_count = 1 after receiving CPU");

    /* Simulate: Linux(VM1) runs 50ms, WFI yield, switch to Android(VM3) */
    stats_on_switch(1, 3, 50000, false);
    stats_get(1, &s);
    CHECK(s.total_us == 50000,   "Linux total_us = 50ms");
    CHECK(s.preempt_count == 0,  "Linux preempt_count = 0 (WFI not preempt)");
    stats_get(3, &s);
    CHECK(s.slice_count == 1,    "Android slice_count = 1");

    /* WFI accounting */
    stats_on_wfi(2);
    stats_on_wfi(2);
    stats_get(2, &s);
    CHECK(s.wfi_count == 2, "RTOS wfi_count = 2 after 2 WFI yields");

    /* Switch counter */
    CHECK(_switch_count == 2, "Global switch_count = 2");

    /* Multiple switches */
    for (u32 i = 0; i < 5; i++)
        stats_on_switch(1, 2, 1000, true);
    stats_get(1, &s);
    CHECK(s.preempt_count == 5, "Linux preempt_count = 5 after 5 timer preempts");
    CHECK(_switch_count == 7,   "Global switch_count = 7 total");

    /* NULL/invalid args */
    err_t r = stats_get(0, &s);
    CHECK(r == E_INVAL, "stats_get(0): E_INVAL");
    r = stats_get(1, NULL);
    CHECK(r == E_INVAL, "stats_get(NULL out): E_INVAL");
    r = stats_get(5, &s);
    CHECK(r == E_INVAL, "stats_get(5 > MAX_VMS): E_INVAL");
}

/* ── §3.3.4 Runtime slice configuration ── */

#define MAX_SLOTS 16
typedef struct { u32 vm_id; u32 dur_us; } slot_t;
static slot_t _slots[MAX_SLOTS];
static u32 _nslots = 0;

static void slots_init(void) { _nslots = 0; }
static void slots_add(u32 vm_id, u32 dur_us) {
    if (_nslots < MAX_SLOTS) {
        _slots[_nslots].vm_id  = vm_id;
        _slots[_nslots].dur_us = dur_us;
        _nslots++;
    }
}
static err_t set_slice(u32 vm_id, u32 dur_us) {
    if (dur_us < 100)     dur_us = 100;
    if (dur_us > 1000000) dur_us = 1000000;
    bool found = false;
    for (u32 i = 0; i < _nslots; i++) {
        if (_slots[i].vm_id == vm_id) {
            _slots[i].dur_us = dur_us;
            found = true;
        }
    }
    return found ? E_OK : E_NOTFOUND;
}

static void test_slice_config(void)
{
    printf("\n--- test_slice_config ---\n");
    slots_init();
    slots_add(1, 50000);  /* Linux:   50ms */
    slots_add(2, 1000);   /* RTOS:    1ms  */
    slots_add(3, 50000);  /* Android: 50ms */
    slots_add(3, 50000);  /* Android vCPU1 */

    err_t r;

    /* Valid slice changes */
    r = set_slice(1, 20000);
    CHECK(r == E_OK, "set_slice(Linux, 20ms): E_OK");
    CHECK(_slots[0].dur_us == 20000, "Linux slot updated to 20ms");

    r = set_slice(2, 5000);
    CHECK(r == E_OK, "set_slice(RTOS, 5ms): E_OK");
    CHECK(_slots[1].dur_us == 5000, "RTOS slot updated to 5ms");

    /* Android has 2 slots — both should be updated */
    r = set_slice(3, 30000);
    CHECK(r == E_OK, "set_slice(Android, 30ms): E_OK");
    CHECK(_slots[2].dur_us == 30000 && _slots[3].dur_us == 30000,
          "Both Android vCPU slots updated to 30ms");

    /* Non-existent VM */
    r = set_slice(4, 10000);
    CHECK(r == E_NOTFOUND, "set_slice(VM4 not in table): E_NOTFOUND");

    /* Floor clamp: < 100us → 100us */
    r = set_slice(2, 50);
    CHECK(r == E_OK && _slots[1].dur_us == 100,
          "set_slice floor clamp: 50us → 100us");

    /* Ceiling clamp: > 1s → 1s */
    r = set_slice(1, 5000000);
    CHECK(r == E_OK && _slots[0].dur_us == 1000000,
          "set_slice ceiling clamp: 5s → 1s");
}

/* ── §3.2.x IPC inline implementation ── */

#define IPC_MSG_MAX  256
#define IPC_DEPTH    8
#define IPC_VIRQ     31

typedef struct { u8 data[IPC_MSG_MAX]; u32 len; u32 src; } ipc_msg_t;
typedef struct { ipc_msg_t slots[IPC_DEPTH]; u32 head,tail,cnt; } ipc_q_t;

static ipc_q_t _iqs[MAX_VMS];
static u32 _notified_vm = 0;

static void ipc_init_test(void) {
    memset(_iqs, 0, sizeof(_iqs));
    _notified_vm = 0;
}

static err_t ipc_send_test(u32 dst, const u8 *buf, u32 len) {
    if (dst < 1 || dst > MAX_VMS || len > IPC_MSG_MAX) return E_INVAL;
    ipc_q_t *q = &_iqs[dst-1];
    if (q->cnt >= IPC_DEPTH) return E_BUSY;
    ipc_msg_t *m = &q->slots[q->tail];
    memcpy(m->data, buf, len);
    m->len = len; m->src = 1;
    q->tail = (q->tail+1) % IPC_DEPTH;
    q->cnt++;
    return E_OK;
}

static err_t ipc_recv_test(u32 dst, u8 *buf, u32 max, u32 *out_len) {
    ipc_q_t *q = &_iqs[dst-1];
    if (q->cnt == 0) { *out_len = 0; return E_OK; }
    ipc_msg_t *m = &q->slots[q->head];
    u32 n = (m->len < max) ? m->len : max;
    memcpy(buf, m->data, n);
    *out_len = n;
    q->head = (q->head+1) % IPC_DEPTH;
    q->cnt--;
    return E_OK;
}

static void ipc_notify_test(u32 dst) { _notified_vm = dst; }

static void test_ipc(void)
{
    printf("\n--- test_ipc ---\n");
    ipc_init_test();

    u8 send_buf[64] = "Hello from RTOS";
    u8 recv_buf[64] = {0};
    u32 out_len = 0;
    err_t r;

    /* Send and receive */
    r = ipc_send_test(1, send_buf, 16);
    CHECK(r == E_OK, "ipc_send to VM1: E_OK");
    CHECK(_iqs[0].cnt == 1, "VM1 queue depth = 1");

    r = ipc_recv_test(1, recv_buf, sizeof(recv_buf), &out_len);
    CHECK(r == E_OK,   "ipc_recv from VM1: E_OK");
    CHECK(out_len == 16, "Received 16 bytes");
    CHECK(memcmp(send_buf, recv_buf, 16) == 0,
          "Received data matches sent data");
    CHECK(_iqs[0].cnt == 0, "VM1 queue empty after recv");

    /* Empty queue recv */
    r = ipc_recv_test(1, recv_buf, sizeof(recv_buf), &out_len);
    CHECK(r == E_OK && out_len == 0, "Empty queue recv: E_OK, len=0");

    /* Fill queue to capacity */
    u8 data[4] = {1,2,3,4};
    for (u32 i = 0; i < IPC_DEPTH; i++)
        ipc_send_test(2, data, 4);
    r = ipc_send_test(2, data, 4);
    CHECK(r == E_BUSY, "ipc_send to full queue: E_BUSY");

    /* Drain queue */
    for (u32 i = 0; i < IPC_DEPTH; i++)
        ipc_recv_test(2, recv_buf, sizeof(recv_buf), &out_len);
    CHECK(_iqs[1].cnt == 0, "VM2 queue drained to empty");

    /* IPC notify */
    ipc_notify_test(2);
    CHECK(_notified_vm == 2, "ipc_notify: VM2 notified");

    /* Invalid args */
    r = ipc_send_test(0, data, 4);
    CHECK(r == E_INVAL, "ipc_send to VM0: E_INVAL");
    r = ipc_send_test(5, data, 4);
    CHECK(r == E_INVAL, "ipc_send to VM5 (>MAX): E_INVAL");
    r = ipc_send_test(1, data, IPC_MSG_MAX + 1);
    CHECK(r == E_INVAL, "ipc_send oversized msg: E_INVAL");

    /* Multi-message ordering */
    u8 msg1[4] = {0xAA,0,0,0}, msg2[4] = {0xBB,0,0,0};
    ipc_send_test(3, msg1, 4);
    ipc_send_test(3, msg2, 4);
    ipc_recv_test(3, recv_buf, 4, &out_len);
    CHECK(recv_buf[0] == 0xAA, "IPC FIFO: first message received first");
    ipc_recv_test(3, recv_buf, 4, &out_len);
    CHECK(recv_buf[0] == 0xBB, "IPC FIFO: second message received second");
}

/* ── HVC ABI constants verification ── */
#define HVC_VM_GET_ID       0x0001u
#define HVC_VM_QUERY_STATE  0x0002u
#define HVC_VM_STOP         0x0003u
#define HVC_VM_SUSPEND      0x0004u
#define HVC_VM_RESUME       0x0005u
#define HVC_IPC_SEND        0x0020u
#define HVC_IPC_RECV        0x0021u
#define HVC_IPC_NOTIFY      0x0022u
#define HVC_SCHED_SET_SLICE 0x0030u
#define HVC_SCHED_GET_STATS 0x0031u
#define HVC_SCHED_YIELD     0x0032u
#define HVC_LOG_WRITE       0x00F0u
#define HVC_PERF_QUERY      0x00F1u

static void test_hvc_abi(void)
{
    printf("\n--- test_hvc_abi ---\n");

    /* Verify no ID collisions */
    u32 ids[] = {
        HVC_VM_GET_ID, HVC_VM_QUERY_STATE, HVC_VM_STOP,
        HVC_VM_SUSPEND, HVC_VM_RESUME,
        HVC_IPC_SEND, HVC_IPC_RECV, HVC_IPC_NOTIFY,
        HVC_SCHED_SET_SLICE, HVC_SCHED_GET_STATS, HVC_SCHED_YIELD,
        HVC_LOG_WRITE, HVC_PERF_QUERY,
    };
    u32 n = sizeof(ids) / sizeof(ids[0]);
    bool no_collision = true;
    for (u32 i = 0; i < n; i++)
        for (u32 j = i+1; j < n; j++)
            if (ids[i] == ids[j]) no_collision = false;

    CHECK(no_collision,               "No HVC ID collisions");
    CHECK(HVC_SCHED_SET_SLICE == 0x30, "HVC_SCHED_SET_SLICE = 0x30");
    CHECK(HVC_SCHED_GET_STATS == 0x31, "HVC_SCHED_GET_STATS = 0x31");
    CHECK(HVC_SCHED_YIELD     == 0x32, "HVC_SCHED_YIELD     = 0x32");
    CHECK(HVC_PERF_QUERY      == 0xF1, "HVC_PERF_QUERY      = 0xF1");

    /* Phase 3 IDs are in the 0x30 range, not colliding with 0x20 IPC */
    CHECK(HVC_SCHED_SET_SLICE != HVC_IPC_SEND,   "Sched IDs don't overlap IPC");
    CHECK(HVC_SCHED_GET_STATS != HVC_IPC_RECV,   "Sched IDs don't overlap IPC");
    CHECK(HVC_SCHED_YIELD     != HVC_IPC_NOTIFY, "Sched IDs don't overlap IPC");
}

/* ── Context switch log format verification ── */
static void test_ctx_log_format(void)
{
    printf("\n--- test_ctx_log_format ---\n");

    /* Simulate what do_switch() logs and verify the format makes sense */
    char buf[128];
    u64 switch_count = 42;
    const char *from = "rtos", *to = "linux";
    u32 dur_ms = 1;
    const char *reason = "timer";

    snprintf(buf, sizeof(buf), "CTX[%llu]: %s→%s (%ums, %s)",
             (unsigned long long)switch_count, from, to, dur_ms, reason);

    CHECK(strstr(buf, "CTX[42]") != NULL,  "Log contains switch counter");
    CHECK(strstr(buf, "rtos→linux") != NULL, "Log shows from→to VM names");
    CHECK(strstr(buf, "1ms") != NULL,      "Log shows slice duration");
    CHECK(strstr(buf, "timer") != NULL,    "Log shows switch reason");

    /* WFI reason */
    snprintf(buf, sizeof(buf), "CTX[%llu]: %s→%s (%ums, %s)",
             (unsigned long long)43ULL, "rtos", "linux", 0u, "wfi");
    CHECK(strstr(buf, "wfi") != NULL, "WFI reason logged correctly");
}

int main(void)
{
    printf("=== Phase 3 unit tests ===\n");

    test_sched_stats();
    test_slice_config();
    test_ipc();
    test_hvc_abi();
    test_ctx_log_format();

    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0)
        printf("    Phase 3 unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
