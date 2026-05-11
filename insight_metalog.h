/*
 * insight_metalog.h — 64-byte metalog record layout and session aggregation API.
 *
 * Records are processed in 64-byte units (little-endian u64 fields where noted).
 * insight_metalog_read() installs a dedicated nvme_read post_action that scans
 * each chunk for insight_metalog_record entries and aggregates per-session
 * min/max of ts_us and lba_byte_offset.
 */

#ifndef INSIGHT_METALOG_H
#define INSIGHT_METALOG_H

#include <stddef.h>
#include <stdint.h>

#define INSIGHT_METALOG_RECORD_BYTES 64U

#define INSIGHT_METALOG_SESSION_ID_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)

#define INSIGHT_METALOG_OP_READ  0U
#define INSIGHT_METALOG_OP_WRITE 1U
#define INSIGHT_METALOG_OP_TRIM  2U

#define INSIGHT_METALOG_OBJECT_TYPE_0 0U
#define INSIGHT_METALOG_OBJECT_TYPE_1 1U
#define INSIGHT_METALOG_OWNER_TYPE_0  0U
#define INSIGHT_METALOG_OWNER_TYPE_1  1U

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

/** Per-session aggregate over all parsed metalog records. */
struct insight_metalog_session_summary {
	uint64_t session_id;
	uint64_t ts_us_start;
	uint64_t ts_us_end;
	uint64_t lba_byte_offset_start;
	uint64_t lba_byte_offset_end;
};

/**
 * Read NVMe log range via nvme_read(), parse 64-byte metalog records, and
 * build one summary row per distinct session_id (min/max ts_us and
 * lba_byte_offset across records for that session).
 *
 * On success, *out_sessions points to a heap-allocated array of length
 * *out_count; free with insight_metalog_sessions_free().
 *
 * Returns 0 on success, -1 on error (errno set). nvme_read() soft-stop is
 * treated as success; partial log is still summarized.
 */
int insight_metalog_read(const char *device_name,
			 uint64_t slba,
			 uint64_t data_len,
			 struct insight_metalog_session_summary **out_sessions,
			 size_t *out_count);

/** Frees the array returned by insight_metalog_read(); sessions may be NULL. */
void insight_metalog_sessions_free(struct insight_metalog_session_summary *sessions);

#endif /* INSIGHT_METALOG_H */
