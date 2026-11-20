# ns-3 Homework 3: Flow Control and Reliable Transmission

- **Setup:** 2 nodes on a PointToPoint link (10 Mbps, 5 ms baseline one-way delay, 1000-byte payload, 30 s simulation, 20-packet baseline receiver buffer, fixed seed `42`).
- **Timeouts (`--rtoMs`):** P1a (`30/50/100 ms`), P1b (`50 ms`), P2a (`60/120 ms`), P3a & P3b (`60 ms`).
- **Run from `~/ns3-sim/ns-3`:** `python3.12 ./ns3 run "saw_p1a --delayMs=5 --rtoMs=50"` (and `saw_p1ab`, `gbn_p2`, `sr_p3`).
