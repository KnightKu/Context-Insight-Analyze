#define _POSIX_C_SOURCE 200809L

#include "nvme_read.h"
#include "post_action.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int expect_slba_ms(const char *name,
                          const char *path,
                          uint64_t time_start_ms,
                          uint64_t expect_block,
                          int expect_rc) {
    uint64_t slba = 0ULL;
    errno = 0;
    int rc = nvme_read_probe_log_slba_by_time_ms_from_file(path, 0ULL, time_start_ms, &slba);
    const uint64_t expect_slba = LOG_START_LBA + (expect_block * NVME_READ_CHUNK_BYTES);
    if (rc != expect_rc) {
        fprintf(stderr,
                "%s: expected rc=%d got rc=%d errno=%d (%s)\n",
                name,
                expect_rc,
                rc,
                errno,
                strerror(errno));
        return -1;
    }
    if (expect_rc == 0 && slba != expect_slba) {
        fprintf(stderr,
                "%s: expected slba=%" PRIu64 " got %" PRIu64 "\n",
                name,
                expect_slba,
                slba);
        return -1;
    }
    return 0;
}

int main(void) {
    const char *path = "tests/fixtures/valid_log_probe_chunks.bin";

    if (expect_slba_ms("block0", path, 1710000000000ULL, 0ULL, 0) != 0) {
        return 1;
    }
    if (expect_slba_ms("block1", path, 1710000015000ULL, 1ULL, 0) != 0) {
        return 1;
    }
    if (expect_slba_ms("block2", path, 1710000025000ULL, 2ULL, 0) != 0) {
        return 1;
    }
    if (expect_slba_ms("block3_last", path, 2000000000000ULL, 3ULL, 0) != 0) {
        return 1;
    }

    uint64_t slba = 0ULL;
    if (nvme_read_probe_log_slba_by_time_ms_from_file(NULL, 0ULL, 1710000000000ULL, &slba) == 0 ||
        errno != EINVAL) {
        fprintf(stderr, "expected EINVAL for NULL path\n");
        return 1;
    }

    int tw_enabled = 0;
    uint64_t tw_start = 0ULL;
    uint64_t tw_end = 0ULL;
    if (nvme_read_set_time_window(1, 1710000005000ULL, 1, 1710000020000ULL) != 0) {
        fprintf(stderr, "failed to set time window before probe\n");
        return 1;
    }
    if (nvme_read_probe_log_slba_by_time_ms_from_file(path, 0ULL, 1710000015000ULL, &slba) != 0) {
        fprintf(stderr, "probe failed while time window was set\n");
        return 1;
    }
    if (nvme_post_action_get_time_window_ms(&tw_enabled, &tw_start, &tw_end) != 0) {
        fprintf(stderr, "failed to read time window after probe\n");
        return 1;
    }
    if (tw_enabled == 0 || tw_start != 1710000005000ULL || tw_end != 1710000020000ULL) {
        fprintf(stderr,
                "probe must restore time window (enabled=%d start=%" PRIu64 " end=%" PRIu64 ")\n",
                tw_enabled,
                tw_start,
                tw_end);
        return 1;
    }

    return 0;
}
