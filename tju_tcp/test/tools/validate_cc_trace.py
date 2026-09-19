#!/usr/bin/env python3
"""Validate a TJU TCP Reno CSV trace without third-party dependencies."""

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


FIELDS = [
    "timestamp_us",
    "event",
    "seq",
    "ack",
    "cwnd",
    "ssthresh",
    "rwnd",
    "flight_size",
    "state",
    "allowed",
    "smss",
]
NUMERIC_FIELDS = [
    "timestamp_us",
    "seq",
    "ack",
    "cwnd",
    "ssthresh",
    "rwnd",
    "flight_size",
    "allowed",
    "smss",
]
KNOWN_EVENTS = {"INIT", "SEND", "ACK", "DUP_ACK", "FAST_RETRANSMIT", "RTO"}
KNOWN_STATES = {"slow_start", "congestion_avoidance"}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--require-event",
        action="append",
        default=[],
        choices=sorted(KNOWN_EVENTS),
        help="Require an event; repeat for multiple events.",
    )
    return parser.parse_args()


def fail(errors, line, message):
    errors.append(f"line {line}: {message}")


def main():
    args = parse_args()
    errors = []
    counts = Counter()
    rows = 0
    first_timestamp = None
    last_timestamp = None
    previous_timestamp = None
    min_cwnd = None
    max_cwnd = None

    try:
        handle = args.trace.open(newline="", encoding="ascii")
    except OSError as exc:
        print(f"FAIL: cannot open {args.trace}: {exc}", file=sys.stderr)
        return 1

    with handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != FIELDS:
            print(
                f"FAIL: unexpected header {reader.fieldnames!r}; expected {FIELDS!r}",
                file=sys.stderr,
            )
            return 1

        for line_number, raw in enumerate(reader, start=2):
            rows += 1
            values = {}
            for field in NUMERIC_FIELDS:
                try:
                    values[field] = int(raw[field])
                except (TypeError, ValueError):
                    fail(errors, line_number, f"{field} is not an integer")
            if len(values) != len(NUMERIC_FIELDS):
                continue

            event = raw["event"]
            state = raw["state"]
            counts[event] += 1
            if event not in KNOWN_EVENTS:
                fail(errors, line_number, f"unknown event {event!r}")
            if state not in KNOWN_STATES:
                fail(errors, line_number, f"unknown state {state!r}")
            if values["smss"] != 1380:
                fail(errors, line_number, f"SMSS is {values['smss']}, expected 1380")
            expected_allowed = min(values["cwnd"], values["rwnd"])
            if values["allowed"] != expected_allowed:
                fail(
                    errors,
                    line_number,
                    f"allowed={values['allowed']} but min(cwnd,rwnd)={expected_allowed}",
                )
            if event == "SEND" and values["flight_size"] > values["allowed"]:
                fail(
                    errors,
                    line_number,
                    f"SEND flight_size={values['flight_size']} exceeds allowed={values['allowed']}",
                )
            if event == "RTO" and values["cwnd"] != values["smss"]:
                fail(errors, line_number, "RTO did not reduce cwnd to one SMSS")
            if event in {"RTO", "FAST_RETRANSMIT"}:
                minimum = 2 * values["smss"]
                expected_ssthresh = max(values["flight_size"] // 2, minimum)
                if values["ssthresh"] != expected_ssthresh:
                    fail(
                        errors,
                        line_number,
                        f"ssthresh={values['ssthresh']} but expected {expected_ssthresh}",
                    )

            timestamp = values["timestamp_us"]
            if previous_timestamp is not None and timestamp < previous_timestamp:
                fail(errors, line_number, "timestamp moved backwards")
            previous_timestamp = timestamp
            first_timestamp = timestamp if first_timestamp is None else first_timestamp
            last_timestamp = timestamp
            cwnd = values["cwnd"]
            min_cwnd = cwnd if min_cwnd is None else min(min_cwnd, cwnd)
            max_cwnd = cwnd if max_cwnd is None else max(max_cwnd, cwnd)

    if rows == 0:
        errors.append("trace has no data rows")
    for event in args.require_event:
        if counts[event] == 0:
            errors.append(f"required event {event} is missing")

    if errors:
        print(f"FAIL: {len(errors)} trace validation error(s)", file=sys.stderr)
        for error in errors[:20]:
            print(f"  {error}", file=sys.stderr)
        if len(errors) > 20:
            print(f"  ... {len(errors) - 20} more", file=sys.stderr)
        return 1

    duration_ms = 0.0
    if first_timestamp is not None and last_timestamp is not None:
        duration_ms = (last_timestamp - first_timestamp) / 1000.0
    event_summary = " ".join(f"{name}={counts[name]}" for name in sorted(counts))
    print(
        f"PASS: rows={rows} duration_ms={duration_ms:.3f} "
        f"cwnd_min={min_cwnd} cwnd_max={max_cwnd} {event_summary}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
