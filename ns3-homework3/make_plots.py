import csv
from pathlib import Path
import matplotlib.pyplot as plt

base = Path("/workspaces/Data_communication-/ns3-homework3")
res_dir = base / "results"
plots_dir = base / "plots"
plots_dir.mkdir(parents=True, exist_ok=True)

def load_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    return rows

# 1. Problem 1a: Goodput vs RTT
p1a = load_csv(res_dir / "saw_p1a.csv")
plt.figure(figsize=(6.5, 4.2))
plt.plot([float(r["rtt_ms"]) for r in p1a],
         [float(r["goodput_mbps"]) for r in p1a], marker="o", linewidth=2)
plt.title("Problem 1a: Stop-and-Wait Goodput vs. RTT (No Loss)")
plt.xlabel("Round-Trip Propagation Delay (ms)")
plt.ylabel("Goodput (Mbps)")
plt.grid(True, linestyle="--", alpha=0.6)
plt.tight_layout()
plt.savefig(plots_dir / "p1a_goodput_vs_rtt.png", dpi=200)
plt.close()

# 2. Problem 1b: Goodput & Retransmission Ratio vs p
p1b = load_csv(res_dir / "saw_p1b.csv")
fig, ax1 = plt.subplots(figsize=(6.5, 4.2))
ax2 = ax1.twinx()
ps = [float(r["loss_rate"]) for r in p1b]
l1 = ax1.plot(ps, [float(r["goodput_mbps"]) for r in p1b], marker="o", color="tab:blue", linewidth=2, label="Goodput (Mbps)")
l2 = ax2.plot(ps, [float(r["retrans_ratio"]) for r in p1b], marker="s", color="tab:red", linewidth=2, label="Retransmission Ratio")
ax1.set_title("Problem 1b: Stop-and-Wait vs. Frame Loss Probability p (Delay = 5 ms)")
ax1.set_xlabel("Frame Loss Probability p")
ax1.set_ylabel("Goodput (Mbps)", color="tab:blue")
ax2.set_ylabel("Retransmissions per Unique DATA", color="tab:red")
ax1.grid(True, linestyle="--", alpha=0.6)
lines = l1 + l2
ax1.legend(lines, [l.get_label() for l in lines], loc="center right")
plt.tight_layout()
plt.savefig(plots_dir / "p1b_goodput_retrans_vs_p.png", dpi=200)
plt.close()

# 3. Problem 2a: GBN Goodput vs Window W for 5ms and 20ms
p2a = load_csv(res_dir / "gbn_p2a.csv")
plt.figure(figsize=(6.5, 4.2))
for d in ["5.0000", "20.0000"]:
    sub = [r for r in p2a if r["delay_ms"] == d]
    plt.plot([int(r["win"]) for r in sub],
             [float(r["goodput_mbps"]) for r in sub],
             marker="o", linewidth=2, label=f"One-way delay = {int(float(d))} ms")
plt.title("Problem 2a: Go-Back-N Goodput vs. Window Size W (No Loss)")
plt.xlabel("Window Size W (packets)")
plt.ylabel("Goodput (Mbps)")
plt.legend()
plt.grid(True, linestyle="--", alpha=0.6)
plt.tight_layout()
plt.savefig(plots_dir / "p2a_gbn_goodput_vs_w.png", dpi=200)
plt.close()

# 4. Problem 3a: GBN vs SR Goodput at p = 0.05
gbn3 = load_csv(res_dir / "gbn_p3a.csv")
sr3 = load_csv(res_dir / "sr_p3a.csv")
plt.figure(figsize=(6.5, 4.2))
plt.plot([int(r["win"]) for r in gbn3], [float(r["goodput_mbps"]) for r in gbn3], marker="s", linewidth=2, label="Go-Back-N (p=0.05, Brx=20)")
plt.plot([int(r["win"]) for r in sr3], [float(r["goodput_mbps"]) for r in sr3], marker="o", linewidth=2, label="Selective Repeat (p=0.05, Brx=20)")
plt.title("Problem 3a: Go-Back-N vs. Selective Repeat Goodput (p = 0.05)")
plt.xlabel("Window Size W (packets)")
plt.ylabel("Goodput (Mbps)")
plt.legend()
plt.grid(True, linestyle="--", alpha=0.6)
plt.tight_layout()
plt.savefig(plots_dir / "p3a_gbn_vs_sr_goodput.png", dpi=200)
plt.close()

# 5. Problem 3b: SR Goodput & Peak Buffer vs Receiver Buffer Capacity Brx
sr3b = load_csv(res_dir / "sr_p3b.csv")
fig, ax1 = plt.subplots(figsize=(6.5, 4.2))
ax2 = ax1.twinx()
brx = [int(r["rx_buf"]) for r in sr3b]
l1 = ax1.plot(brx, [float(r["goodput_mbps"]) for r in sr3b], marker="o", color="tab:green", linewidth=2, label="Goodput (Mbps)")
l2 = ax2.plot(brx, [int(r["peak_rx_buf"]) for r in sr3b], marker="^", color="tab:purple", linewidth=2, label="Peak Buffer Occupancy (packets)")
ax1.set_title("Problem 3b: SR Goodput & Peak Buffer Occupancy vs. Buffer Capacity (W=32, p=0.05)")
ax1.set_xlabel("Receiver Buffer Capacity B_rx (packets)")
ax1.set_ylabel("Goodput (Mbps)", color="tab:green")
ax2.set_ylabel("Peak Buffer Occupancy (packets)", color="tab:purple")
ax1.grid(True, linestyle="--", alpha=0.6)
lines = l1 + l2
ax1.legend(lines, [l.get_label() for l in lines], loc="center right")
plt.tight_layout()
plt.savefig(plots_dir / "p3b_sr_buffer_capacity.png", dpi=200)
plt.close()

print("All 5 plots generated successfully!")
