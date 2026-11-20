#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>

using namespace ns3;

class GbnHeader : public Header {
public:
  enum Kind : uint8_t { DATA = 1, ACK = 2 };
  GbnHeader() : m_kind(DATA), m_seq(0) {}
  GbnHeader(Kind k, uint32_t s) : m_kind(k), m_seq(s) {}

  static TypeId GetTypeId() {
    static TypeId tid = TypeId("GbnHeader").SetParent<Header>().AddConstructor<GbnHeader>();
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

class GbnSim {
public:
  GbnSim(Ptr<NetDevice> tx, Ptr<NetDevice> rx, uint32_t winSize, uint32_t rxBufCap,
         uint32_t payload, Time rto, Time dur, double lossRate, uint32_t streamId)
      : m_tx(tx), m_rx(rx), m_win(winSize), m_rxBufCap(rxBufCap),
        m_payload(payload), m_rto(rto), m_dur(dur), m_lossRate(lossRate) {
    m_rng = CreateObject<UniformRandomVariable>();
    m_rng->SetStream(streamId);
    m_rx->SetReceiveCallback(MakeCallback(&GbnSim::OnRx, this));
  }

  void Start() { m_active = true; FillWindow(); }
  void Stop() {
    m_active = false;
    if (m_timer.IsPending()) Simulator::Cancel(m_timer);
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

  void SendPkt(GbnHeader::Kind k, uint32_t seq, uint32_t sz) {
    Ptr<Packet> p = Create<Packet>(sz);
    p->AddHeader(GbnHeader(k, seq));
    Address peer = m_tx->GetAddress();
    if (auto p2p = DynamicCast<PointToPointNetDevice>(m_tx)) {
      if (auto ch = p2p->GetChannel(); ch && ch->GetNDevices() == 2) {
        peer = ch->GetDevice(ch->GetDevice(0) == m_tx ? 1 : 0)->GetAddress();
      }
    }
    m_tx->Send(p, peer, 0x0800);
  }

  void StartBaseTimer() {
    if (m_timer.IsPending()) Simulator::Cancel(m_timer);
    if (m_base < m_nextSeq && m_active && Simulator::Now() < m_dur) {
      m_timer = Simulator::Schedule(m_rto, &GbnSim::OnTimeout, this);
    }
  }

  void FillWindow() {
    if (!m_active || Simulator::Now() >= m_dur) return;
    while (m_nextSeq < m_base + m_win && Simulator::Now() < m_dur) {
      if (m_firstTx.find(m_nextSeq) == m_firstTx.end()) {
        m_firstTx[m_nextSeq] = Simulator::Now();
      }
      SendPkt(GbnHeader::DATA, m_nextSeq, m_payload);
      if (m_base == m_nextSeq) {
        StartBaseTimer();
      }
      m_nextSeq++;
    }
  }

  void OnTimeout() {
    if (!m_active || m_base >= m_nextSeq || Simulator::Now() >= m_dur) return;
    m_timeouts++;
    for (uint32_t s = m_base; s < m_nextSeq; ++s) {
      m_retrans++;
      SendPkt(GbnHeader::DATA, s, m_payload);
    }
    StartBaseTimer();
  }

  bool OnRx(Ptr<NetDevice>, Ptr<const Packet> pkt, uint16_t, const Address &) {
    if (!m_active) return true;
    Ptr<Packet> copy = pkt->Copy();
    GbnHeader h;
    copy->RemoveHeader(h);

    if (h.m_kind == GbnHeader::DATA) {
      if (ShouldDrop()) return true;
      // GBN accepts in-order DATA; tracks buffer occupancy if needed
      m_peakRxBuf = std::max<uint32_t>(m_peakRxBuf, std::min<uint32_t>(1, m_rxBufCap));
      if (h.m_seq == m_expSeq) {
        m_delivered++;
        m_expSeq++;
        // Cumulative ACK for latest in-order delivered sequence (m_expSeq - 1)
        SendPkt(GbnHeader::ACK, m_expSeq - 1, 0);
      } else if (m_expSeq > 0) {
        // Re-ACK last in-order packet
        SendPkt(GbnHeader::ACK, m_expSeq - 1, 0);
      }
    } else if (h.m_kind == GbnHeader::ACK) {
      if (ShouldDrop()) return true;
      // Cumulative ACK acknowledges everything up to and including h.m_seq
      if (h.m_seq >= m_base && h.m_seq < m_nextSeq) {
        for (uint32_t s = m_base; s <= h.m_seq; ++s) {
          auto it = m_firstTx.find(s);
          if (it != m_firstTx.end()) {
            m_delaySum += (Simulator::Now() - it->second);
            m_firstTx.erase(it);
          }
        }
        m_base = h.m_seq + 1;
        if (m_base == m_nextSeq) {
          if (m_timer.IsPending()) Simulator::Cancel(m_timer);
        } else {
          StartBaseTimer();
        }
        FillWindow();
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
  EventId m_timer;
};

int main(int argc, char *argv[]) {
  uint32_t win = 4, rxBuf = 20, payload = 1000, seed = 42;
  double delayMs = 5.0, rtoMs = 60.0, simSec = 30.0, lossRate = 0.0;
  std::string csv = "/workspaces/Data_communication-/ns3-homework3/results/gbn_p2a.csv";
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

  GbnSim sim(devs.Get(0), devs.Get(1), win, rxBuf, payload,
             MilliSeconds(rtoMs), Seconds(simSec), lossRate, seed + 31);
  Simulator::Schedule(Seconds(0.0), &GbnSim::Start, &sim);
  Simulator::Schedule(Seconds(simSec), &GbnSim::Stop, &sim);
  Simulator::Stop(Seconds(simSec + 1.0));
  Simulator::Run();
  Simulator::Destroy();

  std::ofstream out(csv, std::ios::app);
  if (header) {
    out << "delay_ms,rtt_ms,win,loss_rate,goodput_mbps,mean_delay_ms,retrans_ratio,timeouts,delivered\n";
  }
  out << std::fixed << std::setprecision(4)
      << delayMs << "," << (2.0 * delayMs) << "," << win << "," << lossRate << ","
      << sim.GoodputMbps() << "," << sim.MeanDelayMs() << ","
      << sim.RetransRatio() << "," << sim.Timeouts() << "," << sim.Delivered() << "\n";

  std::cout << "delay=" << delayMs << "ms W=" << win << " p=" << lossRate
            << " -> Goodput=" << sim.GoodputMbps() << " Mbps"
            << " MeanDelay=" << sim.MeanDelayMs() << " ms"
            << " Timeouts=" << sim.Timeouts() << "\n";
  return 0;
}
