#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

using namespace ns3;

class SawHeader : public Header {
public:
  enum MsgType : uint8_t { MSG_DATA = 1, MSG_ACK = 2 };

  SawHeader() : m_type(MSG_DATA), m_seq(0) {}
  SawHeader(MsgType type, uint32_t seq) : m_type(type), m_seq(seq) {}

  static TypeId GetTypeId() {
    static TypeId tid = TypeId("SawHeader")
                            .SetParent<Header>()
                            .AddConstructor<SawHeader>();
    return tid;
  }
  TypeId GetInstanceTypeId() const override { return GetTypeId(); }

  void Serialize(Buffer::Iterator start) const override {
    start.WriteU8(static_cast<uint8_t>(m_type));
    start.WriteHtonU32(m_seq);
  }

  uint32_t Deserialize(Buffer::Iterator start) override {
    m_type = static_cast<MsgType>(start.ReadU8());
    m_seq = start.ReadNtohU32();
    return GetSerializedSize();
  }

  uint32_t GetSerializedSize() const override { return 1 + 4; }
  void Print(std::ostream &os) const override {
    os << "type=" << static_cast<int>(m_type) << " seq=" << m_seq;
  }

  MsgType m_type;
  uint32_t m_seq;
};

class StopAndWaitNode {
public:
  StopAndWaitNode(Ptr<NetDevice> txDev, Ptr<NetDevice> rxDev,
                  uint32_t payloadBytes, Time rto, uint32_t seed,
                  double lossRate, Time simDuration)
      : m_txDev(txDev), m_rxDev(rxDev), m_payloadBytes(payloadBytes), m_rto(rto),
        m_lossRate(lossRate), m_simDuration(simDuration), m_nextSeq(0),
        m_expectedSeq(0), m_inFlight(false), m_uniqueDelivered(0),
        m_retransmissions(0), m_timeouts(0), m_delaySum(Seconds(0)),
        m_running(false) {
    m_rng = CreateObject<UniformRandomVariable>();
    m_rng->SetStream(seed);
    m_rxDev->SetReceiveCallback(
        MakeCallback(&StopAndWaitNode::OnReceive, this));
  }

  void Start() {
    m_running = true;
    TrySendData();
  }

  void Stop() {
    m_running = false;
    if (m_ackTimer.IsPending()) {
      Simulator::Cancel(m_ackTimer);
    }
  }

  double GetGoodputMbps() const {
    double sec = m_simDuration.GetSeconds();
    if (sec <= 0.0) return 0.0;
    return (static_cast<double>(m_uniqueDelivered) * m_payloadBytes * 8.0) /
           (sec * 1e6);
  }

  double GetMeanDelayMs() const {
    if (m_uniqueDelivered == 0) return 0.0;
    return (m_delaySum.GetSeconds() / m_uniqueDelivered) * 1000.0;
  }

  double GetRetransRatio() const {
    if (m_uniqueDelivered == 0) return 0.0;
    return static_cast<double>(m_retransmissions) /
           static_cast<double>(m_uniqueDelivered);
  }

  uint64_t GetTimeouts() const { return m_timeouts; }
  uint64_t GetUniqueDelivered() const { return m_uniqueDelivered; }

private:
  bool DropFrame() {
    if (m_lossRate <= 0.0) return false;
    return m_rng->GetValue(0.0, 1.0) < m_lossRate;
  }

  void SendFrame(SawHeader::MsgType type, uint32_t seq, uint32_t bytes) {
    Ptr<Packet> pkt = Create<Packet>(bytes);
    SawHeader hdr(type, seq);
    pkt->AddHeader(hdr);

    Address peerAddr = m_txDev->GetAddress();
    Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(m_txDev);
    if (p2p) {
      Ptr<Channel> ch = p2p->GetChannel();
      if (ch && ch->GetNDevices() == 2) {
        uint32_t idx = (ch->GetDevice(0) == m_txDev) ? 1 : 0;
        peerAddr = ch->GetDevice(idx)->GetAddress();
      }
    }
    m_txDev->Send(pkt, peerAddr, 0x88B5);
  }

  void TrySendData() {
    if (!m_running || m_inFlight) return;
    if (Simulator::Now() >= m_simDuration) return;

    m_inFlight = true;
    if (m_firstTx.find(m_nextSeq) == m_firstTx.end()) {
      m_firstTx[m_nextSeq] = Simulator::Now();
    } else {
      m_retransmissions++;
    }

    SendFrame(SawHeader::MSG_DATA, m_nextSeq, m_payloadBytes);
    m_ackTimer = Simulator::Schedule(m_rto, &StopAndWaitNode::OnTimeout,
                                     this, m_nextSeq);
  }

  void OnTimeout(uint32_t seq) {
    if (!m_running || !m_inFlight || seq != m_nextSeq) return;
    if (Simulator::Now() >= m_simDuration) return;

    m_timeouts++;
    m_inFlight = false;
    TrySendData();
  }

  bool OnReceive(Ptr<NetDevice> dev, Ptr<const Packet> packet,
                 uint16_t protocol, const Address &sender) {
    if (!m_running) return true;

    Ptr<Packet> copy = packet->Copy();
    SawHeader hdr;
    copy->RemoveHeader(hdr);

    if (hdr.m_type == SawHeader::MSG_DATA) {
      if (DropFrame()) return true;

      if (hdr.m_seq == m_expectedSeq) {
        m_uniqueDelivered++;
        m_expectedSeq++;
      }
      // Send ACK for received seq (including duplicates)
      SendFrame(SawHeader::MSG_ACK, hdr.m_seq, 0);
    } else if (hdr.m_type == SawHeader::MSG_ACK) {
      if (DropFrame()) return true;

      if (m_inFlight && hdr.m_seq == m_nextSeq) {
        if (m_ackTimer.IsPending()) {
          Simulator::Cancel(m_ackTimer);
        }
        auto it = m_firstTx.find(m_nextSeq);
        if (it != m_firstTx.end()) {
          m_delaySum += (Simulator::Now() - it->second);
          m_firstTx.erase(it);
        }
        m_nextSeq++;
        m_inFlight = false;
        TrySendData();
      }
    }
    return true;
  }

  Ptr<NetDevice> m_txDev;
  Ptr<NetDevice> m_rxDev;
  uint32_t m_payloadBytes;
  Time m_rto;
  double m_lossRate;
  Time m_simDuration;
  Ptr<UniformRandomVariable> m_rng;

  uint32_t m_nextSeq;
  uint32_t m_expectedSeq;
  bool m_inFlight;
  EventId m_ackTimer;

  uint64_t m_uniqueDelivered;
  uint64_t m_retransmissions;
  uint64_t m_timeouts;
  Time m_delaySum;
  std::map<uint32_t, Time> m_firstTx;
  bool m_running;
};

int main(int argc, char *argv[]) {
  std::string dataRate = "10Mbps";
  double delayMs = 5.0;
  uint32_t payloadBytes = 1000;
  double simSec = 30.0;
  double rtoMs = 50.0;
  uint32_t seed = 42;
  double lossRate = 0.0;
  std::string csvFile = "/workspaces/Data_communication-/ns3-homework3/results/p1_saw.csv";
  bool writeHeader = false;

  CommandLine cmd;
  cmd.AddValue("dataRate", "Link data rate", dataRate);
  cmd.AddValue("delayMs", "One-way propagation delay (ms)", delayMs);
  cmd.AddValue("payloadBytes", "Payload size (bytes)", payloadBytes);
  cmd.AddValue("simSec", "Simulation time (s)", simSec);
  cmd.AddValue("rtoMs", "ACK timeout (ms)", rtoMs);
  cmd.AddValue("seed", "Fixed random seed", seed);
  cmd.AddValue("lossRate", "Frame loss probability p", lossRate);
  cmd.AddValue("csvFile", "CSV output path", csvFile);
  cmd.AddValue("writeHeader", "Write CSV header", writeHeader);
  cmd.Parse(argc, argv);

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(1);

  NodeContainer nodes;
  nodes.Create(2);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue(dataRate));
  p2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(delayMs)));
  NetDeviceContainer devs = p2p.Install(nodes);

  StopAndWaitNode saw(devs.Get(0), devs.Get(1), payloadBytes,
                      MilliSeconds(rtoMs), seed + 1, lossRate,
                      Seconds(simSec));

  Simulator::Schedule(Seconds(0.0), &StopAndWaitNode::Start, &saw);
  Simulator::Schedule(Seconds(simSec), &StopAndWaitNode::Stop, &saw);
  Simulator::Stop(Seconds(simSec + 1.0));
  Simulator::Run();
  Simulator::Destroy();

  double rttPropMs = 2.0 * delayMs;
  std::ofstream out(csvFile, std::ios::app);
  if (writeHeader) {
    out << "delay_ms,rtt_prop_ms,loss_rate,rto_ms,seed,goodput_mbps,"
           "mean_delay_ms,retrans_ratio,timeouts,unique_packets\n";
  }
  out << std::fixed << std::setprecision(4)
      << delayMs << "," << rttPropMs << "," << lossRate << "," << rtoMs << ","
      << seed << "," << saw.GetGoodputMbps() << "," << saw.GetMeanDelayMs()
      << "," << saw.GetRetransRatio() << "," << saw.GetTimeouts() << ","
      << saw.GetUniqueDelivered() << "\n";
  out.close();

  std::cout << "delay=" << delayMs << "ms (RTT=" << rttPropMs << "ms) "
            << "p=" << lossRate << " rto=" << rtoMs << "ms -> "
            << "Goodput=" << saw.GetGoodputMbps() << " Mbps, "
            << "MeanDelay=" << saw.GetMeanDelayMs() << " ms, "
            << "RetransRatio=" << saw.GetRetransRatio() << ", "
            << "Timeouts=" << saw.GetTimeouts() << std::endl;
  return 0;
}
