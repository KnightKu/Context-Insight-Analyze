#!/usr/bin/env python3
import os
import struct
from pathlib import Path


def le_n(value: int, nbytes: int) -> bytes:
    return int(value).to_bytes(nbytes, "little", signed=False)


def rec_rw(op: int, start_lba: int, length: int, reserved: int, latency: int, time_rel: int) -> bytes:
    return bytes([op]) + le_n(start_lba, 5) + le_n(length, 2) + le_n(reserved, 2) + le_n(latency, 3) + le_n(time_rel, 3)


def rec_trim(start_lba: int, total_ranges_raw: int, range_index: int, reserved: int, length: int, time_rel: int) -> bytes:
    return bytes([0x03]) + le_n(start_lba, 5) + bytes([total_ranges_raw, range_index, reserved]) + le_n(length, 4) + le_n(time_rel, 3)


def rec_stat(qd: int, wa: int, time_rel: int, reserved: int = 0) -> bytes:
    return bytes([0x0F, reserved]) + le_n(qd, 2) + bytes([wa]) + le_n(time_rel, 3)


def rec_marker(abs_time: int) -> bytes:
    return bytes([0xFF]) + le_n(abs_time, 7)


def main() -> None:
    fixture_dir = Path(__file__).resolve().parent / "fixtures"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    # Valid mixed stream: RW + Trim group(3) + Stat + Marker
    valid = b"".join(
        [
            rec_rw(0x01, 0x123456789A, 0x0200, 0x0000, 0x001234, 0x005678),
            rec_trim(0x10, 3, 0, 0, 16, 100),
            rec_trim(0x20, 3, 1, 0, 32, 101),
            rec_trim(0x30, 3, 2, 0, 48, 102),
            rec_stat(8, 2, 1000),
            rec_marker(0x01020304050607),
        ]
    )
    (fixture_dir / "valid_mixed.bin").write_bytes(valid)

    # Valid termination marker: first byte of both 8-byte halves is 0x00
    terminator = rec_stat(4, 1, 50) + bytes([0x00]) + b"\x11" * 7 + bytes([0x00]) + b"\x22" * 7
    (fixture_dir / "valid_termination.bin").write_bytes(terminator)

    # Invalid: unknown opcode
    invalid_op = bytes([0x77]) + b"\x00" * 7
    (fixture_dir / "invalid_op.bin").write_bytes(invalid_op)

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

    # Invalid: not 8-byte aligned
    invalid_alignment = rec_stat(1, 1, 1) + b"\xAA"
    (fixture_dir / "invalid_alignment.bin").write_bytes(invalid_alignment)


if __name__ == "__main__":
    main()
