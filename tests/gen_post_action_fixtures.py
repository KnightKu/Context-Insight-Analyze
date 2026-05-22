#!/usr/bin/env python3
from pathlib import Path


def le_n(value: int, nbytes: int) -> bytes:
    return int(value).to_bytes(nbytes, "little", signed=False)


def rec_rw(op: int, start_lba: int, length: int, reserved: int, latency: int, time_rel: int) -> bytes:
    return bytes([op]) + le_n(start_lba, 5) + le_n(length, 2) + le_n(reserved, 2) + le_n(latency, 3) + le_n(time_rel, 3)


def rec_trim(start_lba: int, total_ranges_raw: int, range_index: int, reserved: int, length: int, time_rel: int) -> bytes:
    return bytes([0x03]) + le_n(start_lba, 5) + bytes([total_ranges_raw, range_index, reserved]) + le_n(length, 4) + le_n(time_rel, 3)


def rec_stat(qd: int,
             time_rel: int,
             log_write_4k: int,
             reserved: int,
             hot_write_4k: int,
             folding_write_4k: int) -> bytes:
    return (
        bytes([0x0F]) +
        le_n(qd, 2) +
        le_n(time_rel, 3) +
        le_n(log_write_4k, 2) +
        le_n(reserved, 2) +
        le_n(hot_write_4k, 3) +
        le_n(folding_write_4k, 3)
    )


def rec_marker(abs_time: int, unix_time_ms: int, marker_reserved: int = 0) -> bytes:
    return bytes([0xFF]) + le_n(abs_time, 7) + bytes([marker_reserved & 0xFF]) + le_n(unix_time_ms, 7)


def main() -> None:
    fixture_dir = Path(__file__).resolve().parent / "fixtures"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    # Valid mixed stream: Marker first, then RW + Trim group(3) + Stat
    # Non-marker record time fields are relative to the latest marker abs_time.
    valid = b"".join(
        [
            rec_marker(0x01020304050000, 1_710_000_000_000),
            rec_rw(0x01, 0x123456789A, 0x0200, 0x0000, 0x001234, 0x005678),
            rec_trim(0x10, 3, 0, 0, 16, 100),
            rec_trim(0x20, 3, 1, 0, 32, 101),
            rec_trim(0x30, 3, 2, 0, 48, 102),
            rec_stat(8, 1000, 64, 0, 12, 4),
        ]
    )
    (fixture_dir / "valid_mixed.bin").write_bytes(valid)

    # Valid record stream with explicit marker + relative-time records.
    valid_relative = b"".join(
        [
            rec_marker(2_000_000, 1_710_000_010_000),
            rec_rw(0x02, 0x1000, 0x80, 0x0000, 300, 15),
            rec_stat(16, 20, 7, 0, 2, 1),
            rec_trim(0x2000, 2, 0, 0, 64, 25),
            rec_trim(0x3000, 2, 1, 0, 64, 30),
        ]
    )
    (fixture_dir / "valid_relative_time_with_marker.bin").write_bytes(valid_relative)

    # Valid request-count stream:
    # one 32K write + one 32K read (+ one small read for W2FR visibility),
    # used to verify count histograms are request-based (per operation), not per 4K bucket count.
    valid_request_count = b"".join(
        [
            rec_marker(6_000_000, 1_710_000_020_000),
            rec_rw(0x02, 0x4000, 8, 0x0000, 120, 10),   # write 32K
            rec_rw(0x01, 0x4000, 8, 0x0000, 130, 20),   # read 32K
            rec_rw(0x01, 0x4000, 1, 0x0000, 140, 30),   # read 4K
        ]
    )
    (fixture_dir / "valid_request_count.bin").write_bytes(valid_request_count)

    # Valid stream with op==0 noise in the middle:
    # parser should skip one 16-byte slot and continue.
    valid_with_zero_op_skip = b"".join(
        [
            rec_marker(4_000_000, 1_710_000_030_000),
            rec_stat(3, 10, 1, 0, 1, 0),
            bytes([0x00]) + b"\xAA" * 15,
            rec_stat(5, 20, 2, 0, 1, 0),
        ]
    )
    (fixture_dir / "valid_zero_op_skip.bin").write_bytes(valid_with_zero_op_skip)

    # Valid all-zero termination: one full-zero 16-byte record means end-of-valid-log.
    terminator = rec_marker(3_000_000, 1_710_000_040_000) + rec_stat(4, 50, 3, 0, 1, 0) + (b"\x00" * 16) + rec_stat(6, 60, 5, 0, 2, 1)
    (fixture_dir / "valid_termination.bin").write_bytes(terminator)
    (fixture_dir / "valid_all_zero_termination.bin").write_bytes(terminator)

    # Invalid: unknown opcode
    invalid_op = bytes([0x77]) + b"\x00" * 15
    (fixture_dir / "invalid_op.bin").write_bytes(invalid_op)

    # Valid by skip policy: records before first marker are dropped as invalid/noise.
    # After marker appears, following relative-time records are processed normally.
    valid_pre_marker_skip = b"".join(
        [
            rec_stat(2, 7, 1, 0, 1, 0),  # dropped before marker
            rec_rw(0x01, 0x100, 0x10, 0x0000, 11, 3),  # dropped before marker
            rec_marker(5_000_000, 1_710_000_050_000),
            rec_stat(4, 9, 2, 0, 1, 0),  # valid after marker
        ]
    )
    (fixture_dir / "valid_pre_marker_skip.bin").write_bytes(valid_pre_marker_skip)

    # Request-count fixture: one write + one 32K read on same range.
    # With request-based counting, Read Count histogram should record one request.
    valid_request_count_32k = b"".join(
        [
            rec_marker(6_000_000, 1_710_000_060_000),
            rec_rw(0x02, 0x4000, 8, 0x0000, 120, 10),  # 32K write
            rec_rw(0x01, 0x4000, 8, 0x0000, 150, 20),  # 32K read
        ]
    )
    (fixture_dir / "valid_request_count_32k.bin").write_bytes(valid_request_count_32k)

    # Invalid: marker timestamp is not strictly increasing, indicating overwrite.
    # Marker is 16 bytes each: first at offset 0, second at offset 16.
    invalid_marker_overwrite = rec_marker(2_000_000, 1_710_000_070_000) + rec_marker(1_999_999, 1_710_000_070_001)
    (fixture_dir / "invalid_marker_overwrite.bin").write_bytes(invalid_marker_overwrite)

    # Invalid: equal marker timestamp is also treated as non-increasing overwrite.
    invalid_marker_nonincreasing = rec_marker(3_000_000, 1_710_000_080_000) + rec_marker(3_000_000, 1_710_000_080_001)
    (fixture_dir / "invalid_marker_nonincreasing.bin").write_bytes(invalid_marker_nonincreasing)

    # Invalid: RW reserved non-zero
    invalid_rw_reserved = rec_rw(0x02, 0xABCDE, 0x10, 0x0001, 1, 2)
    (fixture_dir / "invalid_rw_reserved.bin").write_bytes(invalid_rw_reserved)

    # Invalid: Trim inconsistent total_ranges in a group
    invalid_trim_total = b"".join(
        [
            rec_trim(0x10, 2, 0, 0, 16, 1),
            rec_trim(0x20, 3, 1, 0, 32, 2),
        ]
    )
    (fixture_dir / "invalid_trim_total_ranges.bin").write_bytes(invalid_trim_total)

    # Invalid: not 8-byte aligned tail fragment (has marker first).
    invalid_alignment = rec_marker(1_000_000, 1_710_000_090_000) + rec_stat(1, 1, 1, 0, 1, 0) + b"\xAA"
    (fixture_dir / "invalid_alignment.bin").write_bytes(invalid_alignment)

    # Invalid: marker high 8-byte first byte must be zero (reserved field check).
    invalid_marker_reserved = rec_marker(7_000_000, 1_710_000_100_000, marker_reserved=1)
    (fixture_dir / "invalid_marker_reserved.bin").write_bytes(invalid_marker_reserved)

    # Valid window-filter stream:
    # marker1 (outside target window) + one read
    # marker2 (inside target window) + one read
    # marker3 (outside target window) + one read
    valid_time_window = b"".join(
        [
            rec_marker(8_000_000, 1_710_000_000_000),
            rec_rw(0x01, 0x2000, 8, 0x0000, 111, 10),
            rec_marker(8_100_000, 1_710_000_010_000),
            rec_rw(0x01, 0x3000, 8, 0x0000, 222, 10),
            rec_marker(8_200_000, 1_710_000_020_000),
            rec_rw(0x01, 0x4000, 8, 0x0000, 333, 10),
        ]
    )
    (fixture_dir / "valid_time_window.bin").write_bytes(valid_time_window)

    # Valid block-size-filter stream:
    # block-size=16K (4 * 4K-LBA units), keep only lengths that are >=4 and multiple of 4.
    valid_block_size_filter = b"".join(
        [
            rec_marker(9_000_000, 1_710_000_110_000),
            rec_rw(0x01, 0x5000, 2, 0x0000, 101, 10),  # 8K, drop (<16K)
            rec_rw(0x01, 0x6000, 4, 0x0000, 102, 20),  # 16K, keep
            rec_rw(0x02, 0x7000, 6, 0x0000, 103, 30),  # 24K, drop (not multiple of 16K)
            rec_rw(0x02, 0x8000, 8, 0x0000, 104, 40),  # 32K, keep
            rec_trim(0x9000, 1, 0, 0, 3, 50),          # 12K, drop
            rec_trim(0xA000, 1, 0, 0, 4, 60),          # 16K, keep
        ]
    )
    (fixture_dir / "valid_block_size_filter.bin").write_bytes(valid_block_size_filter)

    chunk_bytes = 256 * 1024
    probe_chunks = []
    for idx, unix_ms in enumerate(
        [1_710_000_000_000, 1_710_000_010_000, 1_710_000_020_000, 1_710_000_030_000]
    ):
        head = rec_marker((idx + 1) * 1_000_000, unix_ms)
        probe_chunks.append(head + (b"\x00" * (chunk_bytes - len(head))))
    (fixture_dir / "valid_log_probe_chunks.bin").write_bytes(b"".join(probe_chunks))


if __name__ == "__main__":
    main()
