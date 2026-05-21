#define _POSIX_C_SOURCE 200809L

#include "insight_metalog.h"

#include "nvme_read.h"
#include "post_action.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVME_LBA_SIZE_BYTES 4096ULL
#define INSIGHT_METALOG_LBA_START (937684608ULL * NVME_LBA_SIZE_BYTES)
#define INSIGHT_METALOG_LEN (107374182400ULL)

struct insight_lba_bitmap {
	uint64_t capacity_bytes;
	uint64_t unit_bytes;
	uint64_t bit_count;
	uint64_t *words;
	uint64_t word_count;
};

typedef struct {
	unsigned int filter_session_id;
	struct insight_metalog_session_summary *out_session;
	int seen_record;
} insight_metalog_scan_ctx_t;

static inline uint64_t metalog_load_le64(const unsigned char *p) {
	return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
	       ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
	       ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline uint32_t metalog_load_le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int metalog_record_is_all_zero(const unsigned char *rec) {
	for (unsigned i = 0; i < INSIGHT_METALOG_RECORD_BYTES; ++i) {
		if (rec[i] != 0U) {
			return 0;
		}
	}
	return 1;
}

static insight_lba_bitmap *insight_lba_bitmap_create(uint64_t capacity_bytes,
						     uint64_t unit_bytes) {
	if (unit_bytes == 0ULL) {
		errno = EINVAL;
		return NULL;
	}
	if (capacity_bytes < unit_bytes) {
		errno = EINVAL;
		return NULL;
	}
	uint64_t bit_count = capacity_bytes / unit_bytes;
	if (bit_count == 0ULL) {
		errno = EINVAL;
		return NULL;
	}
	uint64_t word_count = (bit_count + 63ULL) / 64ULL;
	size_t alloc_words = 0U;
	if (word_count > (uint64_t)(SIZE_MAX / sizeof(uint64_t))) {
		errno = ENOMEM;
		return NULL;
	}
	alloc_words = (size_t)word_count;
	insight_lba_bitmap *bm = (insight_lba_bitmap *)calloc(1, sizeof(*bm));
	if (bm == NULL) {
		return NULL;
	}
	bm->words = (uint64_t *)calloc(alloc_words, sizeof(uint64_t));
	if (bm->words == NULL) {
		free(bm);
		return NULL;
	}
	bm->capacity_bytes = capacity_bytes;
	bm->unit_bytes = unit_bytes;
	bm->bit_count = bit_count;
	bm->word_count = word_count;
	return bm;
}

static void insight_lba_bitmap_destroy(insight_lba_bitmap *bm) {
	if (bm == NULL) {
		return;
	}
	free(bm->words);
	free(bm);
}

static int insight_lba_bitmap_set_range(insight_lba_bitmap *bm,
					uint64_t byte_offset,
					uint64_t size_bytes) {
	if (bm == NULL) {
		errno = EINVAL;
		return -1;
	}
	uint64_t range_end = byte_offset + size_bytes;
	if (size_bytes == 0ULL) {
		range_end = byte_offset + 1ULL;
	}
	if (range_end <= byte_offset) {
		return 0;
	}
	uint64_t cap_end = bm->capacity_bytes;
	if (byte_offset >= cap_end) {
		return 0;
	}
	if (range_end > cap_end) {
		range_end = cap_end;
	}
	uint64_t unit_start = byte_offset / bm->unit_bytes;
	uint64_t unit_end = (range_end - 1ULL) / bm->unit_bytes;
	if (unit_end >= bm->bit_count) {
		unit_end = bm->bit_count - 1ULL;
	}
	for (uint64_t unit = unit_start; unit <= unit_end; ++unit) {
		uint64_t word_idx = unit / 64ULL;
		uint64_t bit_idx = unit % 64ULL;
		bm->words[word_idx] |= (1ULL << bit_idx);
	}
	return 0;
}

static int insight_lba_bitmap_unit_is_set(const insight_lba_bitmap *bm, uint64_t unit) {
	if (bm == NULL || unit >= bm->bit_count) {
		return 0;
	}
	uint64_t word_idx = unit / 64ULL;
	uint64_t bit_idx = unit % 64ULL;
	return (bm->words[word_idx] & (1ULL << bit_idx)) != 0ULL ? 1 : 0;
}

void insight_lba_bitmap_print_ranges(const insight_lba_bitmap *bitmap, FILE *out) {
	if (out == NULL) {
		out = stdout;
	}
	if (bitmap == NULL) {
		fprintf(out, "lba_bitmap: (null)\n");
		return;
	}

	uint64_t range_count = 0ULL;
	int in_range = 0;
	uint64_t range_start = 0ULL;
	uint64_t range_end = 0ULL;

	fprintf(out,
		"lba_bitmap ranges (unit=%llu bytes, capacity=%llu bytes):\n",
		(unsigned long long)bitmap->unit_bytes,
		(unsigned long long)bitmap->capacity_bytes);

	for (uint64_t unit = 0ULL; unit < bitmap->bit_count; ++unit) {
		if (insight_lba_bitmap_unit_is_set(bitmap, unit) == 0) {
			if (in_range != 0) {
				fprintf(out,
					"  lba [%" PRIu64 ", %" PRIu64 "]"
					"  bytes [%" PRIu64 ", %" PRIu64 "]\n",
					(uint64_t)range_start,
					(uint64_t)range_end,
					(uint64_t)(range_start * bitmap->unit_bytes),
					(uint64_t)((range_end + 1ULL) * bitmap->unit_bytes - 1ULL));
				++range_count;
				in_range = 0;
			}
			continue;
		}
		if (in_range == 0) {
			range_start = unit;
			range_end = unit;
			in_range = 1;
		} else {
			range_end = unit;
		}
	}
	if (in_range != 0) {
		fprintf(out,
			"  lba [%" PRIu64 ", %" PRIu64 "]"
			"  bytes [%" PRIu64 ", %" PRIu64 "]\n",
			(uint64_t)range_start,
			(uint64_t)range_end,
			(uint64_t)(range_start * bitmap->unit_bytes),
			(uint64_t)((range_end + 1ULL) * bitmap->unit_bytes - 1ULL));
		++range_count;
	}
	if (range_count == 0ULL) {
		fprintf(out, "  (no set bits)\n");
	} else {
		fprintf(out, "  total ranges: %" PRIu64 "\n", range_count);
	}
}

int insight_lba_bitmap_overlaps_lba_range(const insight_lba_bitmap *bm,
					  uint64_t start_lba,
					  uint64_t len_lba) {
	if (bm == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (len_lba == 0ULL) {
		return 0;
	}
	uint64_t last_lba = start_lba + len_lba - 1ULL;
	if (last_lba < start_lba) {
		return 0;
	}
	for (uint64_t lba = start_lba; lba <= last_lba; ++lba) {
		if (insight_lba_bitmap_unit_is_set(bm, lba) != 0) {
			return 1;
		}
	}
	return 0;
}

static int insight_lba_bitmap_byte_range(const insight_lba_bitmap *bm,
					 uint64_t *out_byte_start,
					 uint64_t *out_byte_end) {
	if (bm == NULL || out_byte_start == NULL || out_byte_end == NULL) {
		errno = EINVAL;
		return -1;
	}
	uint64_t first_unit = UINT64_MAX;
	uint64_t last_unit = 0ULL;
	int found = 0;
	for (uint64_t w = 0ULL; w < bm->word_count; ++w) {
		uint64_t word = bm->words[w];
		if (word == 0ULL) {
			continue;
		}
		for (unsigned bit = 0U; bit < 64U; ++bit) {
			if ((word & (1ULL << bit)) == 0ULL) {
				continue;
			}
			uint64_t unit = w * 64ULL + (uint64_t)bit;
			if (unit >= bm->bit_count) {
				break;
			}
			if (!found || unit < first_unit) {
				first_unit = unit;
			}
			if (!found || unit > last_unit) {
				last_unit = unit;
			}
			found = 1;
		}
	}
	if (found == 0) {
		errno = ENOENT;
		return -1;
	}
	*out_byte_start = first_unit * bm->unit_bytes;
	*out_byte_end = (last_unit + 1ULL) * bm->unit_bytes - 1ULL;
	return 0;
}

static int insight_metalog_apply_record(insight_metalog_scan_ctx_t *ctx,
					uint64_t ts_us,
					uint64_t lba_byte_offset,
					uint32_t size_bytes) {
	if (ctx == NULL || ctx->out_session == NULL) {
		errno = EINVAL;
		return -1;
	}
	struct insight_metalog_session_summary *s = ctx->out_session;
	if (ctx->seen_record == 0) {
		s->ts_us_start = ts_us;
		s->ts_us_end = ts_us;
		ctx->seen_record = 1;
	} else {
		if (ts_us < s->ts_us_start) {
			s->ts_us_start = ts_us;
		}
		if (ts_us > s->ts_us_end) {
			s->ts_us_end = ts_us;
		}
	}
	return insight_lba_bitmap_set_range(s->lba_map, lba_byte_offset, (uint64_t)size_bytes);
}

static int insight_metalog_post_action(void *ctx, void *data, uint32_t data_len,
				       uint64_t offset_bytes) {
	(void)offset_bytes;
	insight_metalog_scan_ctx_t *scan = (insight_metalog_scan_ctx_t *)ctx;
	if (scan == NULL || data == NULL || scan->out_session == NULL) {
		return 0;
	}
	const uint64_t filter_id = (uint64_t)scan->filter_session_id;
	const unsigned char *base = (const unsigned char *)data;
	for (uint32_t pos = 0; pos + INSIGHT_METALOG_RECORD_BYTES <= data_len;
	     pos += INSIGHT_METALOG_RECORD_BYTES) {
		const unsigned char *rec = base + (size_t)pos;
		if (metalog_record_is_all_zero(rec) != 0) {
			continue;
		}
		uint64_t session_id = metalog_load_le64(rec + 16);
		if (session_id != filter_id) {
			continue;
		}
		uint64_t ts_us = metalog_load_le64(rec + 8);
		uint64_t lba_off = metalog_load_le64(rec + 32);
		uint32_t size_bytes = metalog_load_le32(rec + 40);
		if (insight_metalog_apply_record(scan, ts_us, lba_off, size_bytes) != 0) {
			return -1;
		}
	}
	return 0;
}

void insight_metalog_session_free(struct insight_metalog_session_summary *session) {
	if (session == NULL) {
		return;
	}
	insight_lba_bitmap_destroy(session->lba_map);
	session->lba_map = NULL;
}

int insight_metalog_read(const char *device_name,
			 struct insight_metalog_session_summary *out_session,
			 const unsigned int session_id) {
	if (device_name == NULL || out_session == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (out_session->lba_map != NULL) {
		errno = EINVAL;
		return -1;
	}

	insight_lba_bitmap *bitmap =
	    insight_lba_bitmap_create(INSIGHT_METALOG_DEFAULT_DEVICE_CAPACITY_BYTES,
				      INSIGHT_METALOG_LBA_UNIT_BYTES);
	if (bitmap == NULL) {
		return -1;
	}

	out_session->session_id = (uint64_t)session_id;
	out_session->ts_us_start = 0ULL;
	out_session->ts_us_end = 0ULL;
	out_session->lba_map = bitmap;

	insight_metalog_scan_ctx_t scan;
	memset(&scan, 0, sizeof(scan));
	scan.filter_session_id = session_id;
	scan.out_session = out_session;
	scan.seen_record = 0;

	nvme_read_post_action_t prev_action = NULL;
	void *prev_ctx = NULL;
	nvme_post_action_get_handler(&prev_action, &prev_ctx);

	if (nvme_read_set_post_action(insight_metalog_post_action, &scan) != 0) {
		insight_metalog_session_free(out_session);
		return -1;
	}

	const int prev_print_reports = nvme_read_get_print_end_reports();
	(void)nvme_read_set_print_end_reports(0);

	int rc = nvme_read(device_name, 0, INSIGHT_METALOG_LEN);

	(void)nvme_read_set_post_action(prev_action, prev_ctx);
	(void)nvme_read_set_print_end_reports(prev_print_reports);

	if (rc != 0) {
		insight_metalog_session_free(out_session);
		return -1;
	}
	if (scan.seen_record == 0) {
		insight_metalog_session_free(out_session);
		errno = ENODATA;
		return -1;
	}
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

int insight_metalog_session_time_window(const struct insight_metalog_session_summary *session,
					char *ts_start_local,
					size_t ts_start_local_len,
					char *ts_end_local,
					size_t ts_end_local_len,
					uint64_t *out_lba_byte_offset_start,
					uint64_t *out_lba_byte_offset_end) {
	if (session == NULL || ts_start_local == NULL || ts_end_local == NULL ||
	    ts_start_local_len == 0U || ts_end_local_len == 0U) {
		errno = EINVAL;
		return -1;
	}
	if (format_ts_us_local(session->ts_us_start, ts_start_local, ts_start_local_len) != 0) {
		return -1;
	}
	if (format_ts_us_local(session->ts_us_end, ts_end_local, ts_end_local_len) != 0) {
		return -1;
	}
	if (out_lba_byte_offset_start == NULL && out_lba_byte_offset_end == NULL) {
		return 0;
	}
	uint64_t byte_lo = 0ULL;
	uint64_t byte_hi = 0ULL;
	if (insight_lba_bitmap_byte_range(session->lba_map, &byte_lo, &byte_hi) != 0) {
		return -1;
	}
	if (out_lba_byte_offset_start != NULL) {
		*out_lba_byte_offset_start = byte_lo;
	}
	if (out_lba_byte_offset_end != NULL) {
		*out_lba_byte_offset_end = byte_hi;
	}
	return 0;
}
