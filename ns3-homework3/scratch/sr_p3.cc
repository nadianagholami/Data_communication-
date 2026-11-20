#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

using namespace ns3;

class SrHeader : public Header {
public:
  enum Kind : uint8_t { DATA = 1, ACK = 2 };
  SrHeader() : m_kind(DATA), m_seq(0) {}
  SrHeader(Kind k, uint32_t s) : m_kind(k), m_seq(s) {}

  static TypeId GetTypeId() {
    static TypeId tid = TypeId("SrHeader").SetParent<Header>().AddConstructor<SrHeader>();
    return tid;
  }
  TypeId GetInstanceTypeId() const override { return GetTypeId(); }
  uint32_t GetSerializedSize() const override { return 5; }
  void Print(std::ostream &os) const override { os << "k=" << (int)m_kind << " s=" << m_seq; }

  void Serialize(Buffer::Iterator it) const override {
    it.WriteU8((uint8_t)m_kind);
    it.WriteHtonU32(m_seq);
  }
  uint32_t Deserialize(Buffer::Iterator it) override {
    m_kind = (Kind)it.ReadU8();
    m_seq = it.ReadNtohU32();
    return GetSerializedSize();
  }

  Kind m_kind;
  uint32_t m_seq;
};

class SrSim {
public:
  SrSim(Ptr<NetDevice> tx, Ptr<NetDevice> rx, uint32_t winSize, uint32_t rxBufCap,
        uint32_t payload, Time rto, Time dur, double lossRate, uint32_t streamId)
      : m_tx(tx), m_rx(rx), m_win(winSize), m_rxBufCap(rxBufCap),
        m_payload(payload), m_rto(rto), m_dur(dur), m_lossRate(lossRate) {
    m_rng = CreateObject<UniformRandomVariable>();
    m_rng->SetStream(streamId);
    m_rx->SetReceiveCallback(MakeCallback(&SrSim::OnRx, this));
  }

  void Start() { m_active = true; FillWindow(); }
  void Stop() {
    m_active = false;
    for (auto &kv : m_timers) {
      if (kv.second.IsPending()) Simulator::Cancel(kv.second);
    }
    m_timers.clear();
  }

  double GoodputMbps() const {
    return (double)m_delivered * m_payload * 8.0 / (m_dur.GetSeconds() * 1e6);
  }
  double MeanDelayMs() const {
    return m_delivered ? (m_delaySum.GetSeconds() / m_delivered) * 1000.0 : 0.0;
  }
  double RetransRatio() const {
    return m_delivered ? (double)m_retrans / (double)m_delivered : 0.0;
  }
  uint64_t Delivered() const { return m_delivered; }
  uint64_t Timeouts() const { return m_timeouts; }
  uint32_t PeakRxBuf() const { return m_peakRxBuf; }

private:
  bool ShouldDrop() {
    if (m_lossRate <= 0.0) return false;
    return m_rng->GetValue(0.0, 1.0) < m_lossRate;
  }

  void SendPkt(SrHeader::Kind k, uint32_t seq, uint32_t sz) {
    Ptr<Packet> p = Create<Packet>(sz);
    p->AddHeader(SrHeader(k, seq));
    Address peer = m_tx->GetAddress();
    if (auto p2p = DynamicCast<PointToPointNetDevice>(m_tx)) {
      if (auto ch = p2p->GetChannel(); ch && ch->GetNDevices() == 2) {
        peer = ch->GetDevice(ch->GetDevice(0) == m_tx ? 1 : 0)->GetAddress();
      }
    }
    m_tx->Send(p, peer, 0x0800);
  }

  void ArmTimer(uint32_t seq) {
    auto it = m_timers.find(seq);
    if (it != m_timers.end() && it->second.IsPending()) {
      Simulator::Cancel(it->second);
    }
    if (m_active && Simulator::Now() < m_dur) {
      m_timers[seq] = Simulator::Schedule(m_rto, &SrSim::OnPacketTimeout, this, seq);
    }
  }

  void FillWindow() {
    if (!m_active || Simulator::Now() >= m_dur) return;
    while (m_nextSeq < m_base + m_win && Simulator::Now() < m_dur) {
      if (m_acked.find(m_nextSeq) == m_acked.end()) {
        if (m_firstTx.find(m_nextSeq) == m_firstTx.end()) {
          m_firstTx[m_nextSeq] = Simulator::Now();
        }
        SendPkt(SrHeader::DATA, m_nextSeq, m_payload);
        ArmTimer(m_nextSeq);
      }
      m_nextSeq++;
    }
  }

  void OnPacketTimeout(uint32_t seq) {
    if (!m_active || Simulator::Now() >= m_dur) return;
    if (seq < m_base || seq >= m_nextSeq) return;
    if (m_acked.find(seq) != m_acked.end()) return;

    m_timeouts++;
    m_retrans++;
    SendPkt(SrHeader::DATA, seq, m_payload);
    ArmTimer(seq);
  }

  void SlideSenderWindow() {
    while (m_base < m_nextSeq && m_acked.find(m_base) != m_acked.end()) {
      m_acked.erase(m_base);
      m_timers.erase(m_base);
      m_base++;
    }
  }

  void UpdatePeakBuf() {
    uint32_t occ = static_cast<uint32_t>(m_rxBuf.size());
    if (occ > m_rxBufCap) occ = m_rxBufCap;
    m_peakRxBuf = std::max(m_peakRxBuf, occ);
  }

  bool OnRx(Ptr<NetDevice>, Ptr<const Packet> pkt, uint16_t, const Address &) {
    if (!m_active) return true;
    Ptr<Packet> copy = pkt->Copy();
    SrHeader h;
    copy->RemoveHeader(h);

    if (h.m_kind == SrHeader::DATA) {
      if (ShouldDrop()) return true;

      if (h.m_seq == m_expSeq) {
        // In-order packet: deliver immediately and flush buffered consecutive packets
        m_delivered++;
        m_expSeq++;
        while (m_rxBuf.find(m_expSeq) != m_rxBuf.end()) {
          m_rxBuf.erase(m_expSeq);
          m_delivered++;
          m_expSeq++;
        }
        UpdatePeakBuf();
        SendPkt(SrHeader::ACK, h.m_seq, 0);
      } else if (h.m_seq > m_expSeq && h.m_seq < m_expSeq + m_win) {
        // Within receive window: buffer out-of-order if capacity allows
        if (m_rxBuf.find(h.m_seq) != m_rxBuf.end()) {
          // Duplicate buffered packet: re-ACK
          SendPkt(SrHeader::ACK, h.m_seq, 0);
        } else if (m_rxBuf.size() < m_rxBufCap) {
          m_rxBuf.insert(h.m_seq);
          UpdatePeakBuf();
          SendPkt(SrHeader::ACK, h.m_seq, 0);
        } else {
          // Buffer full: drop / do not ACK so sender will retransmit
          UpdatePeakBuf();
        }
      } else if (h.m_seq < m_expSeq) {
        // Already delivered duplicate: re-ACK
        SendPkt(SrHeader::ACK, h.m_seq, 0);
      }
    } else if (h.m_kind == SrHeader::ACK) {
      if (ShouldDrop()) return true;
      if (h.m_seq >= m_base && h.m_seq < m_nextSeq) {
        if (m_acked.find(h.m_seq) == m_acked.end()) {
          m_acked.insert(h.m_seq);
          auto it = m_firstTx.find(h.m_seq);
          if (it != m_firstTx.end()) {
            m_delaySum += (Simulator::Now() - it->second);
            m_firstTx.erase(it);
          }
          auto tIt = m_timers.find(h.m_seq);
          if (tIt != m_timers.end()) {
            if (tIt->second.IsPending()) Simulator::Cancel(tIt->second);
            m_timers.erase(tIt);
          }
          SlideSenderWindow();
          FillWindow();
        }
      }
    }
    return true;
  }

  Ptr<NetDevice> m_tx, m_rx;
  uint32_t m_win, m_rxBufCap, m_payload;
  Time m_rto, m_dur;
  double m_lossRate;
  Ptr<UniformRandomVariable> m_rng;

  bool m_active = false;
  uint32_t m_base = 0, m_nextSeq = 0, m_expSeq = 0;
  uint64_t m_delivered = 0, m_retrans = 0, m_timeouts = 0;
  uint32_t m_peakRxBuf = 0;
  Time m_delaySum = Seconds(0);
  std::map<uint32_t, Time> m_firstTx;
  std::set<uint32_t> m_acked;
  std::set<uint32_t> m_rxBuf;
  std::map<uint32_t, EventId> m_timers;
};

int main(int argc, char *argv[]) {
  uint32_t win = 4, rxBuf = 20, payload = 1000, seed = 42;
  double delayMs = 5.0, rtoMs = 60.0, simSec = 30.0, lossRate = 0.05;
  std::string csv = "/workspaces/Data_communication-/ns3-homework3/results/sr_p3a.csv";
  bool header = false;

  CommandLine cmd;
  cmd.AddValue("win", "", win);
  cmd.AddValue("rxBuf", "", rxBuf);
  cmd.AddValue("payload", "", payload);
  cmd.AddValue("seed", "", seed);
  cmd.AddValue("delayMs", "", delayMs);
  cmd.AddValue("rtoMs", "", rtoMs);
  cmd.AddValue("simSec", "", simSec);
  cmd.AddValue("lossRate", "", lossRate);
  cmd.AddValue("csv", "", csv);
  cmd.AddValue("header", "", header);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(1);

  NodeContainer nodes;
  nodes.Create(2);
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
  p2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(delayMs)));
  auto devs = p2p.Install(nodes);

  SrSim sim(devs.Get(0), devs.Get(1), win, rxBuf, payload,
            MilliSeconds(rtoMs), Seconds(simSec), lossRate, seed + 73);
  Simulator::Schedule(Seconds(0.0), &SrSim::Start, &sim);
  Simulator::Schedule(Seconds(simSec), &SrSim::Stop, &sim);
  Simulator::Stop(Seconds(simSec + 1.0));
  Simulator::Run();
  Simulator::Destroy();

  std::ofstream out(csv, std::ios::app);
  if (header) {
    out << "win,rx_buf,loss_rate,goodput_mbps,mean_delay_ms,retrans_ratio,peak_rx_buf,timeouts,delivered\n";
  }
  out << std::fixed << std::setprecision(4)
      << win << "," << rxBuf << "," << lossRate << ","
      << sim.GoodputMbps() << "," << sim.MeanDelayMs() << ","
      << sim.RetransRatio() << "," << sim.PeakRxBuf() << ","
      << sim.Timeouts() << "," << sim.Delivered() << "\n";

  std::cout << "SR W=" << win << " Brx=" << rxBuf << " p=" << lossRate
            << " -> Goodput=" << sim.GoodputMbps() << " Mbps"
            << " RetransRatio=" << sim.RetransRatio()
            << " MeanDelay=" << sim.MeanDelayMs() << " ms"
            << " PeakBuf=" << sim.PeakRxBuf() << "\n";
  return 0;
}
