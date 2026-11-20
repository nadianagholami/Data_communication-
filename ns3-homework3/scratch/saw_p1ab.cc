#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>

using namespace ns3;

class SawHeader : public Header {
public:
  enum Kind : uint8_t { DATA = 1, ACK = 2 };
  SawHeader() : m_kind(DATA), m_seq(0) {}
  SawHeader(Kind k, uint32_t s) : m_kind(k), m_seq(s) {}

  static TypeId GetTypeId() {
    static TypeId tid = TypeId("SawHeader").SetParent<Header>().AddConstructor<SawHeader>();
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

class SawSim {
public:
  SawSim(Ptr<NetDevice> tx, Ptr<NetDevice> rx, uint32_t payload, Time rto,
         Time dur, double lossRate, uint32_t streamId)
      : m_tx(tx), m_rx(rx), m_payload(payload), m_rto(rto), m_dur(dur),
        m_lossRate(lossRate) {
    m_rng = CreateObject<UniformRandomVariable>();
    m_rng->SetStream(streamId);
    m_rx->SetReceiveCallback(MakeCallback(&SawSim::OnRx, this));
  }

  void Start() { m_active = true; SendIfIdle(); }
  void Stop() { m_active = false; if (m_timer.IsPending()) Simulator::Cancel(m_timer); }

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

private:
  bool ShouldDrop() {
    if (m_lossRate <= 0.0) return false;
    return m_rng->GetValue(0.0, 1.0) < m_lossRate;
  }

  void SendPkt(SawHeader::Kind k, uint32_t seq, uint32_t sz) {
    Ptr<Packet> p = Create<Packet>(sz);
    p->AddHeader(SawHeader(k, seq));
    Address peer = m_tx->GetAddress();
    if (auto p2p = DynamicCast<PointToPointNetDevice>(m_tx)) {
      if (auto ch = p2p->GetChannel(); ch && ch->GetNDevices() == 2) {
        peer = ch->GetDevice(ch->GetDevice(0) == m_tx ? 1 : 0)->GetAddress();
      }
    }
    m_tx->Send(p, peer, 0x0800);
  }

  void SendIfIdle() {
    if (!m_active || m_inFlight || Simulator::Now() >= m_dur) return;
    m_inFlight = true;
    if (m_txTime.find(m_nextSeq) == m_txTime.end()) {
      m_txTime[m_nextSeq] = Simulator::Now();
    } else {
      m_retrans++;
    }
    SendPkt(SawHeader::DATA, m_nextSeq, m_payload);
    m_timer = Simulator::Schedule(m_rto, &SawSim::OnTimeout, this, m_nextSeq);
  }

  void OnTimeout(uint32_t seq) {
    if (!m_active || !m_inFlight || seq != m_nextSeq || Simulator::Now() >= m_dur) return;
    m_timeouts++;
    m_inFlight = false;
    SendIfIdle();
  }

  bool OnRx(Ptr<NetDevice>, Ptr<const Packet> pkt, uint16_t, const Address &) {
    if (!m_active) return true;
    Ptr<Packet> copy = pkt->Copy();
    SawHeader h;
    copy->RemoveHeader(h);

    if (h.m_kind == SawHeader::DATA) {
      if (ShouldDrop()) return true;
      if (h.m_seq == m_expSeq) {
        m_delivered++;
        m_expSeq++;
      }
      SendPkt(SawHeader::ACK, h.m_seq, 0);
    } else if (h.m_kind == SawHeader::ACK) {
      if (ShouldDrop()) return true;
      if (m_inFlight && h.m_seq == m_nextSeq) {
        if (m_timer.IsPending()) Simulator::Cancel(m_timer);
        auto it = m_txTime.find(m_nextSeq);
        if (it != m_txTime.end()) {
          m_delaySum += (Simulator::Now() - it->second);
          m_txTime.erase(it);
        }
        m_nextSeq++;
        m_inFlight = false;
        SendIfIdle();
      }
    }
    return true;
  }

  Ptr<NetDevice> m_tx, m_rx;
  uint32_t m_payload;
  Time m_rto, m_dur;
  double m_lossRate;
  Ptr<UniformRandomVariable> m_rng;
  bool m_active = false, m_inFlight = false;
  uint32_t m_nextSeq = 0, m_expSeq = 0;
  uint64_t m_delivered = 0, m_retrans = 0, m_timeouts = 0;
  Time m_delaySum = Seconds(0);
  std::map<uint32_t, Time> m_txTime;
  EventId m_timer;
};

int main(int argc, char *argv[]) {
  double delayMs = 5.0, rtoMs = 50.0, simSec = 30.0, lossRate = 0.0;
  uint32_t payload = 1000, seed = 42;
  std::string csv = "/workspaces/Data_communication-/ns3-homework3/results/saw_p1b.csv";
  bool header = false;

  CommandLine cmd;
  cmd.AddValue("delayMs", "", delayMs);
  cmd.AddValue("rtoMs", "", rtoMs);
  cmd.AddValue("simSec", "", simSec);
  cmd.AddValue("lossRate", "", lossRate);
  cmd.AddValue("payload", "", payload);
  cmd.AddValue("seed", "", seed);
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

  SawSim sim(devs.Get(0), devs.Get(1), payload, MilliSeconds(rtoMs),
             Seconds(simSec), lossRate, seed + 17);
  Simulator::Schedule(Seconds(0.0), &SawSim::Start, &sim);
  Simulator::Schedule(Seconds(simSec), &SawSim::Stop, &sim);
  Simulator::Stop(Seconds(simSec + 1.0));
  Simulator::Run();
  Simulator::Destroy();

  std::ofstream out(csv, std::ios::app);
  if (header) {
    out << "delay_ms,loss_rate,rto_ms,seed,goodput_mbps,mean_delay_ms,retrans_ratio,timeouts,delivered\n";
  }
  out << std::fixed << std::setprecision(4)
      << delayMs << "," << lossRate << "," << rtoMs << "," << seed << ","
      << sim.GoodputMbps() << "," << sim.MeanDelayMs() << ","
      << sim.RetransRatio() << "," << sim.Timeouts() << ","
      << sim.Delivered() << "\n";

  std::cout << "delay=" << delayMs << "ms p=" << lossRate
            << " Goodput=" << sim.GoodputMbps() << " Mbps"
            << " RetransRatio=" << sim.RetransRatio()
            << " Timeouts=" << sim.Timeouts()
            << " MeanDelay=" << sim.MeanDelayMs() << " ms\n";
  return 0;
}
