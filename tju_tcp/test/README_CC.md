# Stage 3 Reno Test Tools

Run all commands from `/vagrant/tju_tcp` unless noted otherwise. The actual
course-network addresses are client `172.17.0.2` and server `172.17.0.3`.

## Build

```sh
make clean
make
make -C test cc
mkdir -p test/artifacts
```

## Transfer sizes

`cc_client` and `cc_server` accept the byte count as their first argument.

- Development smoke test: `524288` (512 KiB)
- Formal transfer test: `104857600` (100 MiB)

The receiver checks every byte. Both programs report `PASS` only after normal
TJU_TCP close processing completes.

## Run one experiment

Start the server VM first:

```sh
TJU_CC_TRACE_FILE=test/artifacts/server.csv \
  timeout 300 ./test/cc_server 524288 | tee test/artifacts/server.log
```

Then start the client VM:

```sh
TJU_CC_TRACE_FILE=test/artifacts/client.csv \
  timeout 300 ./test/cc_client 524288 | tee test/artifacts/client.log
```

Use a larger timeout for the 100 MiB and impaired-network runs. Always use
different trace paths for client and server because both VMs mount the same
host directory.

## Validate and plot

Run these on the client VM after the transfer:

```sh
python3 test/tools/validate_cc_trace.py test/artifacts/client.csv \
  --require-event SEND --require-event ACK
python3 test/tools/plot_cc_trace.py \
  test/artifacts/client.csv test/artifacts/client.svg
```

For a three-duplicate-ACK experiment add:

```sh
--require-event DUP_ACK --require-event FAST_RETRANSMIT
```

For an RTO experiment add `--require-event RTO`. The validator checks CSV
types, SMSS, `allowed=min(cwnd,rwnd)`, every SEND window invariant, and Reno
loss-window calculations. The SVG contains the two required charts and marks
RTO/FAST_RETRANSMIT events.

## Trace controls

- Default name: `tju_tcp_cc_<hostname>_<pid>.csv`
- Explicit path: `TJU_CC_TRACE_FILE=<path>`
- Disable: `TJU_CC_TRACE=0`

CSV columns:

```text
timestamp_us,event,seq,ack,cwnd,ssthresh,rwnd,flight_size,state,allowed,smss
```

Before each later performance run, record the exact `tc` configuration and use
a fresh trace/log name. This repository intentionally does not automate or
hide the course network configuration.
