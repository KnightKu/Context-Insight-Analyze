#include "insight_api.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char json[INSIGHT_JSON_BUFFER_BYTES];
    int rc = get_qd_distribution("/dev/nvme0n1",
                                 "2026-04-26 10:05:05",
                                 "2026-04-26 12:10:05",
                                 INSIGHT_JSON_QUERY_SESSION_ID_NONE,
                                 json);
    if (rc != 0) {
        if (errno == ENODATA) {
            return 0;
        }
        fprintf(stderr, "get_qd_distribution failed: %s\n", strerror(errno));
        return 1;
    }
    puts(json);
    return 0;
}
