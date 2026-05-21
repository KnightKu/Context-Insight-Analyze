/*
 * insight_metalog.h — 64-byte fixed layout for insight metalog records.
 *
 * insight_metalog_read() scans NVMe metalog via nvme_read post_action, keeps
 * only records matching @p session_id, and fills @p out_session (time range +
 * 4KiB-unit LBA bitmap).
 */

#ifndef INSIGHT_METALOG_H
#define INSIGHT_METALOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define INSIGHT_METALOG_SESSION_ID_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)

#define INSIGHT_METALOG_RECORD_BYTES 64U

#define INSIGHT_METALOG_OP_READ  0U
#define INSIGHT_METALOG_OP_WRITE 1U
#define INSIGHT_METALOG_OP_TRIM  2U

#define INSIGHT_METALOG_OBJECT_TYPE_0 0U
#define INSIGHT_METALOG_OBJECT_TYPE_1 1U
#define INSIGHT_METALOG_OWNER_TYPE_0  0U
#define INSIGHT_METALOG_OWNER_TYPE_1  1U

/** LBA coverage bitmap uses 4KiB address units. */
#define INSIGHT_METALOG_LBA_UNIT_BYTES 4096ULL

/** Default device capacity when sizing lba_map (4 TiB). */
#define INSIGHT_METALOG_DEFAULT_DEVICE_CAPACITY_BYTES \
    (4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)

#pragma pack(push, 1)
struct insight_metalog_record {
	uint64_t io_id;
	uint64_t ts_us;
	uint64_t session_id;
	uint64_t kvb_id;
	uint64_t lba_byte_offset;
	uint32_t size_bytes;
	uint16_t layer_start;
	uint16_t layer_count;
	uint16_t placement_group;
	uint8_t op;
	uint8_t object_type;
	uint8_t owner_type;
	uint8_t coalesced_count;
	uint8_t reserved[10];
};
#pragma pack(pop)

_Static_assert(sizeof(struct insight_metalog_record) == 64,
	       "insight_metalog_record must be exactly 64 bytes");

typedef struct insight_lba_bitmap insight_lba_bitmap;

struct insight_metalog_session_summary {
	uint64_t session_id;
	uint64_t ts_us_start;
	uint64_t ts_us_end;
	insight_lba_bitmap *lba_map;
};

/**
 * Read NVMe metalog and aggregate a single @p session_id.
 *
 * @p out_session must be zero-initialized by the caller on entry. On success,
 * fields and an allocated @p out_session->lba_map are filled. Free with
 * insight_metalog_session_free().
 *
 * Returns 0 on success, -1 on error (errno set). errno ENODATA when no record
 * matched @p session_id. nvme_read soft-stop is treated as success.
 */
int insight_metalog_read(const char *device_name,
			 struct insight_metalog_session_summary *out_session,
			 const unsigned int session_id);

/** Releases @p session->lba_map; does not free @p session itself. */
void insight_metalog_session_free(struct insight_metalog_session_summary *session);

#define INSIGHT_METALOG_TIME_STR_BUFSIZ 32U

/**
 * Format ts_us_start / ts_us_end and derive byte offsets from lba_map
 * (first/last set 4KiB unit). ENOENT if lba_map has no bits set.
 */
int insight_metalog_session_time_window(const struct insight_metalog_session_summary *session,
					char *ts_start_local,
					size_t ts_start_local_len,
					char *ts_end_local,
					size_t ts_end_local_len,
					uint64_t *out_lba_byte_offset_start,
					uint64_t *out_lba_byte_offset_end);

/**
 * Returns 1 if any 4KiB bitmap unit overlapping LBA range
 * [start_lba, start_lba + len_lba) is set; 0 if none; -1 on error.
 */
int insight_lba_bitmap_overlaps_lba_range(const insight_lba_bitmap *bitmap,
					    uint64_t start_lba,
					    uint64_t len_lba);

/**
 * Print all maximal contiguous LBA ranges covered by set bits in @p bitmap.
 * Each range is [lba_start, lba_end] inclusive, in 4KiB LBA unit indices
 * (same coordinate system as insight_lba_bitmap_overlaps_lba_range()).
 *
 * @p out may be NULL (defaults to stdout). No-op if @p bitmap is NULL.
 */
void insight_lba_bitmap_print_ranges(const insight_lba_bitmap *bitmap, FILE *out);

#endif /* INSIGHT_METALOG_H */
