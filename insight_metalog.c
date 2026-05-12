#define _POSIX_C_SOURCE 200809L

#include "insight_metalog.h"

#include "nvme_read.h"
#include "post_action.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INSIGHT_METALOG_SESSIONS_INITIAL_CAP 32U

#define NVME_LBA_SIZE_BYTES 4096ULL
#define INSIGHT_METALOG_LBA_START	(937684608ULL * NVME_LBA_SIZE_BYTES)
#define INSIGHT_METALOG_LEN		(107374182400ULL)

typedef struct {
	struct insight_metalog_session_summary *items;
	size_t count;
	size_t cap;
} insight_metalog_agg_t;

static inline uint64_t metalog_load_le64(const unsigned char *p) {
	return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
	       ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
	       ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static int metalog_record_is_all_zero(const unsigned char *rec) {
	for (unsigned i = 0; i < INSIGHT_METALOG_RECORD_BYTES; ++i) {
		if (rec[i] != 0U) {
			return 0;
		}
	}
	return 1;
}

static int metalog_agg_ensure(insight_metalog_agg_t *agg, size_t need_cap) {
	if (need_cap <= agg->cap) {
		return 0;
	}
	size_t ncap = agg->cap == 0U ? INSIGHT_METALOG_SESSIONS_INITIAL_CAP : agg->cap;
	while (ncap < need_cap) {
		if (ncap > SIZE_MAX / 2U) {
			errno = ENOMEM;
			return -1;
		}
		ncap *= 2U;
	}
	void *p = realloc(agg->items, ncap * sizeof(agg->items[0]));
	if (p == NULL) {
		return -1;
	}
	agg->items = (struct insight_metalog_session_summary *)p;
	agg->cap = ncap;
	return 0;
}

static int metalog_agg_add_record(insight_metalog_agg_t *agg,
				  uint64_t session_id,
				  uint64_t ts_us,
				  uint64_t lba_byte_offset) {
	for (size_t i = 0; i < agg->count; ++i) {
		if (agg->items[i].session_id == session_id) {
			if (ts_us < agg->items[i].ts_us_start) {
				agg->items[i].ts_us_start = ts_us;
			}
			if (ts_us > agg->items[i].ts_us_end) {
				agg->items[i].ts_us_end = ts_us;
			}
			if (lba_byte_offset < agg->items[i].lba_byte_offset_start) {
				agg->items[i].lba_byte_offset_start = lba_byte_offset;
			}
			if (lba_byte_offset > agg->items[i].lba_byte_offset_end) {
				agg->items[i].lba_byte_offset_end = lba_byte_offset;
			}
			return 0;
		}
	}
	if (metalog_agg_ensure(agg, agg->count + 1U) != 0) {
		return -1;
	}
	struct insight_metalog_session_summary *s = &agg->items[agg->count];
	s->session_id = session_id;
	s->ts_us_start = ts_us;
	s->ts_us_end = ts_us;
	s->lba_byte_offset_start = lba_byte_offset;
	s->lba_byte_offset_end = lba_byte_offset;
	++agg->count;
	return 0;
}

static int insight_metalog_post_action(void *ctx, void *data, uint32_t data_len,
				       uint64_t offset_bytes) {
	(void)offset_bytes;
	insight_metalog_agg_t *agg = (insight_metalog_agg_t *)ctx;
	if (agg == NULL || data == NULL) {
		return 0;
	}
	const unsigned char *base = (const unsigned char *)data;
	for (uint32_t pos = 0; pos + INSIGHT_METALOG_RECORD_BYTES <= data_len;
	     pos += INSIGHT_METALOG_RECORD_BYTES) {
		const unsigned char *rec = base + (size_t)pos;
		if (metalog_record_is_all_zero(rec) != 0) {
			continue;
		}
		uint64_t ts_us = metalog_load_le64(rec + 8);
		uint64_t session_id = metalog_load_le64(rec + 16);
		uint64_t lba_off = metalog_load_le64(rec + 32);
		if (metalog_agg_add_record(agg, session_id, ts_us, lba_off) != 0) {
			return -1;
		}
	}
	return 0;
}

static int cmp_session_summary(const void *a, const void *b) {
	const struct insight_metalog_session_summary *x =
	    (const struct insight_metalog_session_summary *)a;
	const struct insight_metalog_session_summary *y =
	    (const struct insight_metalog_session_summary *)b;
	if (x->session_id < y->session_id) {
		return -1;
	}
	if (x->session_id > y->session_id) {
		return 1;
	}
	return 0;
}

void insight_metalog_sessions_free(struct insight_metalog_session_summary *sessions) {
	free(sessions);
}

int insight_metalog_read(const char *device_name,
			 struct insight_metalog_session_summary **out_sessions,
			 size_t *out_count) {
	if (out_sessions == NULL || out_count == NULL) {
		errno = EINVAL;
		return -1;
	}
	*out_sessions = NULL;
	*out_count = 0U;

	nvme_read_post_action_t prev_action = NULL;
	void *prev_ctx = NULL;
	nvme_post_action_get_handler(&prev_action, &prev_ctx);

	insight_metalog_agg_t agg;
	memset(&agg, 0, sizeof(agg));

	if (nvme_read_set_post_action(insight_metalog_post_action, &agg) != 0) {
		return -1;
	}

	const int prev_print_reports = nvme_read_get_print_end_reports();
	(void)nvme_read_set_print_end_reports(0);

	int rc = nvme_read(device_name, INSIGHT_METALOG_LBA_START, INSIGHT_METALOG_LEN, NULL);

	(void)nvme_read_set_post_action(prev_action, prev_ctx);
	(void)nvme_read_set_print_end_reports(prev_print_reports);

	if (rc != 0) {
		free(agg.items);
		return -1;
	}

	if (agg.count > 1U) {
		qsort(agg.items, agg.count, sizeof(agg.items[0]), cmp_session_summary);
	}

	*out_sessions = agg.items;
	*out_count = agg.count;
	agg.items = NULL;
	return 0;
}

static int format_ts_us_local(uint64_t ts_us, char *buf, size_t buf_len) {
	if (buf == NULL || buf_len == 0U) {
		errno = EINVAL;
		return -1;
	}
	uint64_t sec_u64 = ts_us / 1000000ULL;
	const uint64_t max_sec =
	    (sizeof(time_t) > 4U) ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
	if (sec_u64 > max_sec) {
		errno = ERANGE;
		return -1;
	}
	time_t sec = (time_t)sec_u64;
	struct tm tm_buf;
	if (localtime_r(&sec, &tm_buf) == NULL) {
		errno = EOVERFLOW;
		return -1;
	}
	if (strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_buf) == 0U) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

int insight_get_session_time_window(const struct insight_metalog_session_summary *sessions,
				    size_t session_count,
				    uint64_t session_id,
				    char *ts_start_local,
				    size_t ts_start_local_len,
				    char *ts_end_local,
				    size_t ts_end_local_len) {
	if (sessions == NULL || ts_start_local == NULL || ts_end_local == NULL ||
	    ts_start_local_len == 0U || ts_end_local_len == 0U) {
		errno = EINVAL;
		return -1;
	}
	for (size_t i = 0; i < session_count; ++i) {
		if (sessions[i].session_id != session_id) {
			continue;
		}
		if (format_ts_us_local(sessions[i].ts_us_start, ts_start_local, ts_start_local_len) != 0) {
			return -1;
		}
		if (format_ts_us_local(sessions[i].ts_us_end, ts_end_local, ts_end_local_len) != 0) {
			return -1;
		}
		return 0;
	}
	errno = ENOENT;
	return -1;
}
