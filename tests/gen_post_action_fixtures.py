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


def rec_marker(abs_time: int) -> bytes:
    return bytes([0xFF]) + le_n(abs_time, 7)


def main() -> None:
    fixture_dir = Path(__file__).resolve().parent / "fixtures"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    # Valid mixed stream: Marker first, then RW + Trim group(3) + Stat
    # Non-marker record time fields are relative to the latest marker abs_time.
    valid = b"".join(
        [
            rec_marker(0x01020304050000),
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
            rec_marker(2_000_000),
            rec_rw(0x02, 0x1000, 0x80, 0x0000, 300, 15),
            rec_stat(16, 20, 7, 0, 2, 1),
            rec_trim(0x2000, 2, 0, 0, 64, 25),
            rec_trim(0x3000, 2, 1, 0, 64, 30),
        ]
    )
    (fixture_dir / "valid_relative_time_with_marker.bin").write_bytes(valid_relative)

    # Valid stream with op==0 noise in the middle:
    # parser should skip one 8-byte slot and continue.
    valid_with_zero_op_skip = b"".join(
        [
            rec_marker(4_000_000),
            rec_stat(3, 10, 1, 0, 1, 0),
            bytes([0x00]) + b"\xAA" * 7,
            rec_stat(5, 20, 2, 0, 1, 0),
        ]
    )
    (fixture_dir / "valid_zero_op_skip.bin").write_bytes(valid_with_zero_op_skip)

    # Valid all-zero termination: one full-zero 8-byte record means end-of-valid-log.
    terminator = rec_marker(3_000_000) + rec_stat(4, 50, 3, 0, 1, 0) + (b"\x00" * 8) + rec_stat(6, 60, 5, 0, 2, 1)
    (fixture_dir / "valid_termination.bin").write_bytes(terminator)
    (fixture_dir / "valid_all_zero_termination.bin").write_bytes(terminator)

    # Invalid: unknown opcode
    invalid_op = bytes([0x77]) + b"\x00" * 7
    (fixture_dir / "invalid_op.bin").write_bytes(invalid_op)

    # Valid by skip policy: records before first marker are dropped as invalid/noise.
    # After marker appears, following relative-time records are processed normally.
    valid_pre_marker_skip = b"".join(
        [
            rec_stat(2, 7, 1, 0, 1, 0),  # dropped before marker
            rec_rw(0x01, 0x100, 0x10, 0x0000, 11, 3),  # dropped before marker
            rec_marker(5_000_000),
            rec_stat(4, 9, 2, 0, 1, 0),  # valid after marker
        ]
    )
    (fixture_dir / "valid_pre_marker_skip.bin").write_bytes(valid_pre_marker_skip)

    # Invalid: marker timestamp is not strictly increasing, indicating overwrite.
    # Marker is 8 bytes each: first at offset 0, second at offset 8.
    invalid_marker_overwrite = rec_marker(2_000_000) + rec_marker(1_999_999)
    (fixture_dir / "invalid_marker_overwrite.bin").write_bytes(invalid_marker_overwrite)

    # Invalid: equal marker timestamp is also treated as non-increasing overwrite.
    invalid_marker_nonincreasing = rec_marker(3_000_000) + rec_marker(3_000_000)
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
    invalid_alignment = rec_marker(1_000_000) + rec_stat(1, 1, 1, 0, 1, 0) + b"\xAA"
    (fixture_dir / "invalid_alignment.bin").write_bytes(invalid_alignment)


if __name__ == "__main__":
    main()
