/*
 * insight_metalog.h — 64-byte fixed layout for insight metalog records.
 *
 * Byte layout:
 *   0-39:  5×u64 — io_id, ts_us, session_id, kvb_id, lba_byte_offset
 *   40-43: u32   — size_bytes
 *   44-49: 3×u16 — layer_start, layer_count, placement_group
 *   50-53: 4×u8  — op, object_type, owner_type, coalesced_count
 *   54-63: 10 bytes reserved (zero-filled; e.g. future block_hash/model_id/dp_rank/lora_id)
 */

#ifndef INSIGHT_METALOG_H
#define INSIGHT_METALOG_H

#include <stdint.h>

/* session_id: use when cmd.owner_id == -1 */
#define INSIGHT_METALOG_SESSION_ID_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)

/* op */
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

#endif /* INSIGHT_METALOG_H */
