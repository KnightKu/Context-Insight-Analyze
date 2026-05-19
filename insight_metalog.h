/*
 * insight_metalog.h — 64-byte fixed layout for insight metalog records.
 *
 * Byte layout:
 *   0-39:  5×u64 — io_id, ts_us, session_id, kvb_id, lba_byte_offset
 *   40-43: u32   — size_bytes
 *   44-49: 3×u16 — layer_start, layer_count, placement_group
 *   50-53: 4×u8  — op, object_type, owner_type, coalesced_count
 *   54-63: 10 bytes reserved (zero-filled; e.g. future block_hash/model_id/dp_rank/lora_id)
 *
 * Records are processed in 64-byte units (little-endian u64 fields where noted).
 * insight_metalog_read() installs a dedicated nvme_read post_action that scans
 * each chunk for insight_metalog_record entries and aggregates per-session
 * min/max of ts_us and lba_byte_offset.
 */

#ifndef INSIGHT_METALOG_H
#define INSIGHT_METALOG_H

#include <stdint.h>
#include <stddef.h>


/* session_id: use when cmd.owner_id == -1 */
#define INSIGHT_METALOG_SESSION_ID_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)

#define INSIGHT_METALOG_RECORD_BYTES 64U

/** Consecutive all-zero 64-byte records that trigger nvme_read soft-stop (ENODATA). */
#define INSIGHT_METALOG_CONSECUTIVE_ZERO_STOP 2U

#define INSIGHT_METALOG_OP_READ  0U
#define INSIGHT_METALOG_OP_WRITE 1U
#define INSIGHT_METALOG_OP_TRIM  2U

/* object_type, owner_type: binary classification (0/1) */
#define INSIGHT_METALOG_OBJECT_TYPE_0 0U
#define INSIGHT_METALOG_OBJECT_TYPE_1 1U
#define INSIGHT_METALOG_OWNER_TYPE_0  0U
#define INSIGHT_METALOG_OWNER_TYPE_1  1U

#pragma pack(push, 1)
struct insight_metalog_record {
	uint64_t io_id;
	uint64_t ts_us;
	/* From cmd.owner_id; INSIGHT_METALOG_SESSION_ID_INVALID if owner_id == -1 */
	uint64_t session_id;
	uint64_t kvb_id; /* cmd.object_id */
	uint64_t lba_byte_offset; /* cmd.lba, or 0 if not applicable */
	uint32_t size_bytes;
	uint16_t layer_start;
	uint16_t layer_count;
	uint16_t placement_group; /* values >65535 are invalid; callers should reject */
	uint8_t op; /* INSIGHT_METALOG_OP_* */
	uint8_t object_type; /* 0 or 1 */
	uint8_t owner_type; /* 0 or 1 */
	uint8_t coalesced_count; /* min(cmd.coalesced_count, 255) */
	uint8_t reserved[10]; /* zero-filled */
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
 * The underlying nvme_read() is run with end-of-run optional stderr output
 * disabled for that call only (latency / LBA / workload summaries, debug
 * bandwidth stats, and post-action perf summary when those features apply).
 *
 * Returns 0 on success, -1 on error (errno set). nvme_read() soft-stop
 * (e.g. INSIGHT_METALOG_CONSECUTIVE_ZERO_STOP consecutive all-zero records)
 * is treated as success; partial log is still summarized.
 */
int insight_metalog_read(const char *device_name,
			 struct insight_metalog_session_summary **out_sessions,
			 size_t *out_count);

/** Frees the array returned by insight_metalog_read(); sessions may be NULL. */
void insight_metalog_sessions_free(struct insight_metalog_session_summary *sessions);

/** Minimum recommended size for buffers passed to insight_get_session_time_window (includes NUL). */
#define INSIGHT_METALOG_TIME_STR_BUFSIZ 32U

/**
 * Look up @p session_id in @p sessions[0 .. session_count-1] and format
 * ts_us_start / ts_us_end as local calendar time strings
 * "YYYY-MM-DD HH:MM:SS" (second precision; sub-microsecond part of ts_us is truncated).
 *
 * @p ts_start_local and @p ts_end_local must each hold at least
 * INSIGHT_METALOG_TIME_STR_BUFSIZ bytes for a typical strftime result.
 *
 * On success, copies the matched row's lba_byte_offset_start / end into
 * @p out_lba_byte_offset_start / @p out_lba_byte_offset_end when the
 * corresponding pointer is non-NULL (byte offsets from the metalog aggregate).
 *
 * Returns 0 on success, -1 on error. errno: EINVAL (bad args), ENOENT (no such
 * session), ENOBUFS (strftime buffer too small), or ERANGE/EOVERFLOW from time
 * conversion.
 */
int insight_get_session_time_window(const struct insight_metalog_session_summary *sessions,
				    size_t session_count,
				    uint64_t session_id,
				    char *ts_start_local,
				    size_t ts_start_local_len,
				    char *ts_end_local,
				    size_t ts_end_local_len,
				    uint64_t *out_lba_byte_offset_start,
				    uint64_t *out_lba_byte_offset_end);

void insight_print_sessions(const struct insight_metalog_session_summary *sessions,
				    size_t session_count);
#endif /* INSIGHT_METALOG_H */
