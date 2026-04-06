/*
 * FinalIntegratedTest.cc
 *
 * Unified experiment harness — WiFi-only mode.
 * LR-WPAN and wired domains have been removed because rLEDBAT is a
 * receiver-side algorithm that requires TCP timestamps (TSecr) to measure
 * RTT; 802.15.4's 127-byte frame limit makes reliable TCP operation with
 * timestamps impractical at non-trivial loads.
 *
 * ---------------------------------------------------------------------------
 * Topology (WiFi mode):
 *
 *   Sender WiFi domain (mobile)    p2p backbone         Receiver WiFi domain
 *   ----------------------------   -------------------  --------------------
 *   wifi sender nodes --\                                  relay nodes ...
 *                        +--[GW_wifi_tx]--[CORE]--[CORE_wifi]--\
 *   wifi sender nodes --/                                        +--[node0]
 *                                                                sinks 9000/9001
 *
 *   node0=receiver, node1=GW_wifi_tx, node2=CORE, node3=CORE_wifi
 *   nodes 4..(4+txN-1)  = WiFi TX senders (mobile, sender-side channel)
 *   nodes (4+txN)..N-1  = WiFi RX relays  (mobile, receiver-side channel)
 *
 *   Two independent YansWifiChannel objects isolate the two WiFi domains.
 *   All sender traffic MUST route GW→P2P→CORE→P2P→CORE_wifi→receiver.
 *
 * ---------------------------------------------------------------------------
 * Queue configuration on CORE's outgoing P2P port toward CORE_wifi (d2.Get(0)):
 *
 *   Base pass      : no queue configured (default device DropTail)
 *   Modified pass  : RED + ECN  (cfg.enableEcn=true, MinTh=50p, MaxTh=200p)
 *   CADF-only run  : non-ECN RED (large DropTail-like, MinTh=150p, MaxTh=200p)
 *   ECS-only run   : RED + ECN  (same as modified pass)
 *   ATD-only run   : non-ECN RED (same as CADF-only)
 *
 *   pktTxMs at 200 Mbps, 1448-byte MSS: ~0.116 ms
 *   ECS MinTh=50p → 5.8 ms queuing — CE marking fires before targetDelay=10 ms,
 *   giving ECS an early-warning advantage over the delay-based controller.
 *
 * ---------------------------------------------------------------------------
 * Output:
 *   scratch/final_integrated_results.txt   — sweep metrics (parser-compatible)
 *   scratch/dat/final-wledbat-<tag>.dat    — W_ledbat trace per run
 *   scratch/dat/final-qdelay-<tag>.dat     — queuing delay trace per run
 *   scratch/dat/final-basedelay-<tag>.dat  — base delay trace per run
 * ---------------------------------------------------------------------------
 */

#include "ns3/tcp-rledbat.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/energy-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nix-vector-routing-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <map>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("FinalIntegratedTest");

// =========================================================================
// Trace callbacks
// =========================================================================

static void
TraceWLedbat(std::ofstream* ofs, uint32_t /* old */, uint32_t newVal)
{
    *ofs << std::fixed << std::setprecision(6)
         << Simulator::Now().GetSeconds() << "  " << newVal << "\n";
}

static void
TraceDelay(std::ofstream* ofs, Time /* old */, Time newVal)
{
    *ofs << std::fixed << std::setprecision(6)
         << Simulator::Now().GetSeconds() << "  "
         << newVal.GetMilliSeconds() << "\n";
}

// =========================================================================
// CustomSink
// =========================================================================

class CustomSink : public Application
{
  public:
    static TypeId
    GetTypeId()
    {
        static TypeId tid = TypeId("CustomSink")
                                .SetParent<Application>()
                                .SetGroupName("Applications")
                                .AddConstructor<CustomSink>();
        return tid;
    }

    CustomSink()
        : m_port(0),
          m_socket(nullptr),
          m_totalRx(0),
          m_useEcn(false),
          m_ipv6(false),
          m_wFile(nullptr),
          m_qFile(nullptr),
          m_bFile(nullptr)
    {
    }

    void
    Setup(TypeId socketClassId,
          uint16_t port,
          Time targetDelay = Time(0),
          bool useEcn = false,
          bool ipv6 = false)
    {
        m_socketClassId = socketClassId;
        m_port = port;
        m_targetDelay = targetDelay;
        m_useEcn = useEcn;
        m_ipv6 = ipv6;
    }

    void
    SetTraceFiles(std::ofstream* wFile, std::ofstream* qFile, std::ofstream* bFile)
    {
        m_wFile = wFile;
        m_qFile = qFile;
        m_bFile = bFile;
    }

    uint64_t GetTotalRx()    const { return m_totalRx; }
    uint32_t GetAcceptCount() const { return m_acceptCount; }

  private:
    // Temporarily set TcpL4Protocol::SocketClass to m_socketClassId before
    // creating the listening socket, then immediately restore the default.
    // TcpL4Protocol::Fork() is overridden in TcpRledbat to clone the socket
    // class, so every accepted connection on port 9000 will be a TcpRledbat
    // instance and the WLedbat trace source will be available in HandleAccept.
    void
    StartApplication() override
    {
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        TypeIdValue prev;
        tcp->GetAttribute("SocketClass", prev);
        tcp->SetAttribute("SocketClass", TypeIdValue(m_socketClassId));
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        tcp->SetAttribute("SocketClass", prev);

        if (m_useEcn)
            m_socket->SetAttribute("UseEcn", StringValue("On"));

        if (!m_targetDelay.IsZero() &&
            m_socketClassId == TypeId::LookupByName("ns3::TcpRledbat"))
            m_socket->SetAttribute("TargetDelay", TimeValue(m_targetDelay));

        if (m_ipv6)
        {
            int rc = m_socket->Bind(Inet6SocketAddress(Ipv6Address::GetAny(), m_port));
            NS_ABORT_MSG_IF(rc != 0, "CustomSink IPv6 bind failed");
        }
        else
        {
            int rc = m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
            NS_ABORT_MSG_IF(rc != 0, "CustomSink IPv4 bind failed");
        }

        NS_ABORT_MSG_IF(m_socket->Listen() != 0, "CustomSink listen failed");
        m_socket->ShutdownSend();
        m_socket->SetRecvCallback(MakeCallback(&CustomSink::HandleRead, this));
        m_socket->SetRecvPktInfo(true);
        m_acceptAll = true;
        m_socket->SetAcceptCallback(
            MakeCallback(&CustomSink::HandleAcceptRequest, this),
            MakeCallback(&CustomSink::HandleAccept, this));
        m_socket->SetCloseCallbacks(MakeCallback(&CustomSink::HandlePeerClose, this),
                                    MakeCallback(&CustomSink::HandlePeerError, this));
    }

    bool HandleAcceptRequest(Ptr<Socket>, const Address&) { return m_acceptAll; }
    void HandlePeerClose(Ptr<Socket>) {}
    void HandlePeerError(Ptr<Socket>) {}

    void
    StopApplication() override
    {
        if (m_socket) { m_socket->Close(); m_socket = nullptr; }
        for (auto& s : m_connectedSockets) s->Close();
        m_connectedSockets.clear();
    }

    void
    HandleAccept(Ptr<Socket> socket, const Address&)
    {
        ++m_acceptCount;
        socket->SetRecvCallback(MakeCallback(&CustomSink::HandleRead, this));
        m_connectedSockets.push_back(socket);

        // Auto-attach rLEDBAT trace sources.  This fires for every new
        // connection, so dat files are written continuously across all flows.
        if (m_wFile != nullptr &&
            m_socketClassId == TypeId::LookupByName("ns3::TcpRledbat"))
        {
            Ptr<TcpSocketBase> tcpSock = DynamicCast<TcpSocketBase>(socket);
            if (tcpSock)
            {
                bool ok = tcpSock->TraceConnectWithoutContext(
                    "WLedbat", MakeBoundCallback(&TraceWLedbat, m_wFile));
                if (ok)
                {
                    tcpSock->TraceConnectWithoutContext(
                        "QueuingDelay", MakeBoundCallback(&TraceDelay, m_qFile));
                    tcpSock->TraceConnectWithoutContext(
                        "BaseDelay",    MakeBoundCallback(&TraceDelay, m_bFile));
                }
                else
                {
                    std::cerr << "[WARN] WLedbat trace attach failed at t="
                              << Simulator::Now().GetSeconds() << "s\n";
                }
            }
        }
    }

    void
    HandleRead(Ptr<Socket> socket)
    {
        Ptr<Packet> pkt;
        Address from;
        while ((pkt = socket->RecvFrom(from)))
            if (pkt->GetSize() > 0) m_totalRx += pkt->GetSize();
    }

    TypeId   m_socketClassId;
    uint16_t m_port;
    Time     m_targetDelay;
    Ptr<Socket> m_socket;
    uint64_t m_totalRx;
    std::vector<Ptr<Socket>> m_connectedSockets;
    bool m_useEcn;
    bool m_ipv6;
    bool m_acceptAll{false};
    std::ofstream* m_wFile;
    std::ofstream* m_qFile;
    std::ofstream* m_bFile;
    uint32_t m_acceptCount{0};
};

NS_OBJECT_ENSURE_REGISTERED(CustomSink);

// =========================================================================
// RetryBulkSender
// =========================================================================

class RetryBulkSender : public Application
{
  public:
    static TypeId
    GetTypeId()
    {
        static TypeId tid = TypeId("RetryBulkSender")
                                .SetParent<Application>()
                                .SetGroupName("Applications")
                                .AddConstructor<RetryBulkSender>();
        return tid;
    }

    RetryBulkSender()
        : m_socket(nullptr), m_connected(false), m_connecting(false),
          m_running(false), m_connectWatchdogSeconds(10.0)
    {}

    void
    Setup(Address peer,
          uint32_t chunkSize          = 1448,
          uint32_t pps                = 300,
          TypeId   socketClassId      = TcpSocketBase::GetTypeId(),
          bool     forceSocketClass   = false,
          bool     connectWatchdog    = false,
          double   connectWatchdogSec = 10.0,
          std::ofstream* wFile        = nullptr,
          std::ofstream* qFile        = nullptr,
          std::ofstream* bFile        = nullptr)
    {
        m_peer                  = peer;
        m_chunkSize             = chunkSize;
        m_pps                   = std::max(1u, pps);
        m_socketClassId         = socketClassId;
        m_forceSocketClass      = forceSocketClass;
        m_connectWatchdog       = connectWatchdog;
        m_connectWatchdogSeconds = connectWatchdogSec;
        m_wFile = wFile; m_qFile = qFile; m_bFile = bFile;
    }

    uint32_t GetConnectAttempts()          const { return m_connectAttempts; }
    uint32_t GetConnectImmediateFail()     const { return m_connectImmediateFail; }
    uint32_t GetConnectSuccess()           const { return m_connectSuccess; }
    uint32_t GetConnectFailCallback()      const { return m_connectFailCallback; }
    uint32_t GetConnectTimeouts()          const { return m_connectTimeouts; }
    uint32_t GetSendSuccess()              const { return m_sendSuccess; }
    uint32_t GetSendBlocked()              const { return m_sendBlocked; }
    uint32_t GetConnectionSucceededTrace() const { return m_connectionSucceededTrace; }
    uint32_t GetStateSynSent()             const { return m_stateSynSent; }
    uint32_t GetStateEstablished()         const { return m_stateEstablished; }

  private:
    double m_connectWatchdogSeconds;

    void
    SetupSocket()
    {
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        NS_ABORT_MSG_IF(!tcp, "RetryBulkSender: node missing TcpL4Protocol");
        TypeIdValue prev(TcpSocketBase::GetTypeId());
        if (m_forceSocketClass)
        {
            tcp->GetAttribute("SocketClass", prev);
            tcp->SetAttribute("SocketClass", TypeIdValue(m_socketClassId));
        }
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        if (m_forceSocketClass) tcp->SetAttribute("SocketClass", prev);

        m_socket->SetConnectCallback(
            MakeCallback(&RetryBulkSender::OnConnected,    this),
            MakeCallback(&RetryBulkSender::OnConnectFailed, this));
        m_socket->SetSendCallback(MakeCallback(&RetryBulkSender::OnSendPossible, this));
        m_socket->TraceConnectWithoutContext(
            "ConnectionSucceeded",
            MakeCallback(&RetryBulkSender::OnConnectionSucceededTrace, this));
        m_socket->TraceConnectWithoutContext(
            "State", MakeCallback(&RetryBulkSender::OnStateTrace, this));
    }

    void OnConnectionSucceededTrace(Ptr<Socket>) { ++m_connectionSucceededTrace; }
    void OnStateTrace(TcpSocket::TcpStates_t, TcpSocket::TcpStates_t ns)
    {
        if (ns == TcpSocket::SYN_SENT)    ++m_stateSynSent;
        if (ns == TcpSocket::ESTABLISHED) ++m_stateEstablished;
    }

    void StartApplication() override { m_running = true; SetupSocket(); TryConnect(); }

    void
    StopApplication() override
    {
        m_running = false;
        if (m_retryEvent.IsRunning())         Simulator::Cancel(m_retryEvent);
        if (m_connectTimeoutEvent.IsRunning()) Simulator::Cancel(m_connectTimeoutEvent);
        if (m_sendEvent.IsRunning())          Simulator::Cancel(m_sendEvent);
        if (m_socket) { m_socket->Close(); m_socket = nullptr; }
    }

    void
    TryConnect()
    {
        if (!m_running || !m_socket || m_connected || m_connecting) return;
        ++m_connectAttempts;
        int rc = m_socket->Connect(m_peer);
        if (rc != 0)
        {
            ++m_connectImmediateFail;
            if (!m_retryEvent.IsRunning())
                m_retryEvent = Simulator::Schedule(Seconds(1.0),
                                                   &RetryBulkSender::TryConnect, this);
        }
        else
        {
            m_connecting = true;
            if (m_connectWatchdog)
            {
                if (m_connectTimeoutEvent.IsRunning())
                    Simulator::Cancel(m_connectTimeoutEvent);
                m_connectTimeoutEvent = Simulator::Schedule(
                    Seconds(m_connectWatchdogSeconds),
                    &RetryBulkSender::OnConnectTimeout, this);
            }
        }
    }

    void
    OnConnected(Ptr<Socket>)
    {
        ++m_connectSuccess;
        m_connected = true; m_connecting = false;
        if (m_retryEvent.IsRunning())         Simulator::Cancel(m_retryEvent);
        if (m_connectTimeoutEvent.IsRunning()) Simulator::Cancel(m_connectTimeoutEvent);
        ScheduleSend();
    }

    void
    OnConnectFailed(Ptr<Socket>)
    {
        ++m_connectFailCallback;
        if (!m_running) return;
        if (m_socket) { m_socket->Close(); m_socket = nullptr; }
        SetupSocket();
        m_connected = false; m_connecting = false;
        if (m_connectTimeoutEvent.IsRunning()) Simulator::Cancel(m_connectTimeoutEvent);
        if (!m_retryEvent.IsRunning())
            m_retryEvent = Simulator::Schedule(Seconds(1.0),
                                               &RetryBulkSender::TryConnect, this);
    }

    void
    OnConnectTimeout()
    {
        if (!m_running || !m_connecting || m_connected) return;
        ++m_connectTimeouts;
        if (m_socket) { m_socket->Close(); m_socket = nullptr; }
        SetupSocket();
        m_connected = false; m_connecting = false;
        if (!m_retryEvent.IsRunning())
            m_retryEvent = Simulator::Schedule(MilliSeconds(100),
                                               &RetryBulkSender::TryConnect, this);
    }

    void OnSendPossible(Ptr<Socket>, uint32_t)
    {
        if (m_connected && !m_sendEvent.IsRunning()) ScheduleSend();
    }

    void
    SendOnce()
    {
        if (!m_running || !m_connected || !m_socket) return;
        int sent = m_socket->Send(Create<Packet>(m_chunkSize));
        if (sent > 0) { ++m_sendSuccess; ScheduleSend(); }
        else
        {
            ++m_sendBlocked;
            m_sendEvent = Simulator::Schedule(MilliSeconds(5),
                                              &RetryBulkSender::SendOnce, this);
        }
    }

    void
    ScheduleSend()
    {
        if (!m_running || !m_connected || !m_socket || m_sendEvent.IsRunning()) return;
        m_sendEvent = Simulator::Schedule(Seconds(1.0 / m_pps),
                                          &RetryBulkSender::SendOnce, this);
    }

    Address m_peer;
    Ptr<Socket> m_socket;
    EventId m_retryEvent, m_sendEvent, m_connectTimeoutEvent;
    uint32_t m_chunkSize{1448};
    uint32_t m_pps{300};
    TypeId   m_socketClassId{TcpSocketBase::GetTypeId()};
    bool m_forceSocketClass{false}, m_connectWatchdog{false};
    std::ofstream* m_wFile{nullptr};
    std::ofstream* m_qFile{nullptr};
    std::ofstream* m_bFile{nullptr};
    bool m_connected, m_connecting, m_running;
    uint32_t m_connectAttempts{0}, m_connectImmediateFail{0};
    uint32_t m_connectSuccess{0},  m_connectFailCallback{0};
    uint32_t m_connectTimeouts{0}, m_sendSuccess{0}, m_sendBlocked{0};
    uint32_t m_connectionSucceededTrace{0}, m_stateSynSent{0}, m_stateEstablished{0};
};

NS_OBJECT_ENSURE_REGISTERED(RetryBulkSender);

// =========================================================================
// ScenarioConfig
// =========================================================================

struct ScenarioConfig
{
    // WiFi-only build. activeLrwpan/activeWired kept for parser format compat.
    bool activeWifi{true};
    bool activeLrwpan{false};
    bool activeWired{false};

    uint32_t numNodes{60};
    uint32_t numFlows{30};
    uint32_t pps{300};
    double speedMps{15.0};
    double coverageMultiplier{3.0}; // unused; kept for format compat

    double simTimeSec{60.0};
    double warmupSec{10.0};
    double txRangeM{50.0};

    // rLEDBAT parameters
    double targetDelayMs{10.0};
    uint32_t baseDelayHistSize{2};
    double baseDelayRefreshSec{5.0};

    // Modification flags
    bool enableMods{false};
    bool enableCadf{false};
    bool enableEcs{false};
    bool enableAtd{false};
    // enableEcn: true for ECS runs (enables TCP-level ECN and RED UseEcn).
    bool enableEcn{false};

    // CADF parameters (defaults per spec)
    uint32_t cadfStreakThreshold{5};
    double   cadfSpikeRatio{2.5};
    double   cadfMinAbsoluteSpike{500};
    uint32_t cadfWindowSize{10};

    // ECS / ATD parameters (defaults per spec)
    double ecsBeta{0.5};
    double atdAlpha{0.5};
    double atdMinTargetMs{5.0};

    // Queue configuration:
    //   false → no queue disc (default device DropTail)
    //   true  → RED queue disc on CORE→CORE_wifi P2P link (d2.Get(0)):
    //             enableEcn=true  → RED+ECN (MinTh=50p,  MaxTh=100p)
    //             enableEcn=false → DropTail-like RED (MinTh=150p, MaxTh=200p)
    bool configureQueue{false};

    bool enableTraceDat{true};
    std::string tag{"default"};
};

// =========================================================================
// RunMetrics
// =========================================================================

struct RunMetrics
{
    struct FlowMetrics
    {
        uint32_t flowId{0};
        std::string src;
        uint16_t dport{0};
        std::string klass;
        double activeDurSec{0.0};
        double tputMbps{0.0};
        double delayMs{0.0};
        double jitterMs{0.0};
        double pdr{0.0};
        double dropRatio{0.0};
    };

    double totalTputMbps{0.0};
    double rledbatTputMbps{0.0};
    double interactiveTputMbps{0.0};
    double yieldingRatio{0.0};

    double avgDelayMs{0.0};
    double rledbatDelayMs{0.0};
    double interactiveDelayMs{0.0};

    double avgJitterMs{0.0};
    double rledbatJitterMs{0.0};
    double interactiveJitterMs{0.0};

    double totalPDR{0.0};
    double rledbatPDR{0.0};
    double interactivePDR{0.0};

    double totalDropRatio{0.0};
    double rledbatDropRatio{0.0};
    double interactiveDropRatio{0.0};

    double totalEnergyJ{0.0};
    double avgEnergyPerNodeJ{0.0};

    double jainFairness{1.0};
    std::vector<std::pair<std::string, double>> perSenderTput;
    std::vector<FlowMetrics> perFlow;

    // LR-WPAN diagnostic fields kept for format compat (always zero).
    uint32_t lrwpanSenderApps{0};
    uint64_t lrConnectAttempts{0};
    uint64_t lrConnectImmediateFail{0};
    uint64_t lrConnectSuccess{0};
    uint64_t lrConnectFailCallback{0};
    uint64_t lrConnectTimeouts{0};
    uint64_t lrSendSuccess{0};
    uint64_t lrSendBlocked{0};
    uint64_t lrConnectionSucceededTrace{0};
    uint64_t lrUpdateAccepts{0};
    uint64_t lrInteractiveAccepts{0};
    uint64_t lrUpdateRxBytes{0};
    uint64_t lrInteractiveRxBytes{0};
    uint64_t lrStateSynSent{0};
    uint64_t lrStateEstablished{0};
};

// =========================================================================
// WriteMetrics  — format is fixed; do not modify (parser depends on it)
// =========================================================================

static void
WriteMetrics(std::ofstream& ofs, const ScenarioConfig& cfg, const RunMetrics& m)
{
    ofs << "\nTag: " << cfg.tag << "\n";
    ofs << "  Domains: wifi=" << (cfg.activeWifi ? "on" : "off")
        << " lrwpan=" << (cfg.activeLrwpan ? "on" : "off")
        << " wired=" << (cfg.activeWired ? "on" : "off") << "\n";
    ofs << "  Config: N=" << cfg.numNodes
        << " flows=" << cfg.numFlows
        << " pps=" << cfg.pps
        << " speed=" << cfg.speedMps;
    if (cfg.activeLrwpan)
        ofs << " coverage=" << cfg.coverageMultiplier << "xTxRange";
    ofs << " mods=" << (cfg.enableMods ? "on" : "off") << "\n";

    ofs << std::fixed << std::setprecision(4);
    ofs << "  [Throughput]\n";
    ofs << "    Total Network    : " << m.totalTputMbps << " Mbps\n";
    ofs << "    rLEDBAT Flows    : " << m.rledbatTputMbps << " Mbps\n";
    ofs << "    Interactive Flows: " << m.interactiveTputMbps << " Mbps\n";
    ofs << "    Yielding Ratio   : " << m.yieldingRatio << "\n";
    ofs << "    Jain Fairness    : " << m.jainFairness << "\n";

    ofs << std::setprecision(2);
    ofs << "  [E2E Delay]\n";
    ofs << "    Average          : " << m.avgDelayMs << " ms\n";
    ofs << "    rLEDBAT avg      : " << m.rledbatDelayMs << " ms\n";
    ofs << "    Interactive avg  : " << m.interactiveDelayMs << " ms\n";

    ofs << std::setprecision(4);
    ofs << "  [PDR]\n";
    ofs << "    Total            : " << 100.0 * m.totalPDR << " %\n";
    ofs << "    rLEDBAT          : " << 100.0 * m.rledbatPDR << " %\n";
    ofs << "    Interactive      : " << 100.0 * m.interactivePDR << " %\n";

    ofs << "  [Drop Ratio]\n";
    ofs << "    Total            : " << 100.0 * m.totalDropRatio << " %\n";
    ofs << "    rLEDBAT          : " << 100.0 * m.rledbatDropRatio << " %\n";
    ofs << "    Interactive      : " << 100.0 * m.interactiveDropRatio << " %\n";

    ofs << std::setprecision(2);
    ofs << "  [Jitter]\n";
    ofs << "    Average          : " << m.avgJitterMs << " ms\n";
    ofs << "    rLEDBAT avg      : " << m.rledbatJitterMs << " ms\n";
    ofs << "    Interactive avg  : " << m.interactiveJitterMs << " ms\n";

    ofs << std::setprecision(4);
    ofs << "  [Energy Consumption]\n";
    ofs << "    Total (all nodes): " << m.totalEnergyJ << " J\n";
    ofs << "    Per-Node Average : " << m.avgEnergyPerNodeJ << " J\n";

    ofs << "  [Per-Sender Throughput]\n";
    for (const auto& [lbl, tp] : m.perSenderTput)
        ofs << "    " << std::left << std::setw(42) << lbl
            << " : " << std::right << std::setprecision(4) << tp << " Mbps\n";

    ofs << "  [Per-Flow Metrics]\n";
    for (const auto& f : m.perFlow)
        ofs << "    flowId=" << f.flowId
            << " src=" << f.src
            << " class=" << f.klass
            << " port=" << f.dport
            << " dur=" << std::setprecision(3) << f.activeDurSec << "s"
            << " tput=" << std::setprecision(4) << f.tputMbps << "Mbps"
            << " delay=" << std::setprecision(2) << f.delayMs << "ms"
            << " jitter=" << std::setprecision(2) << f.jitterMs << "ms"
            << " pdr=" << std::setprecision(4) << (100.0 * f.pdr) << "%"
            << " drop=" << std::setprecision(4) << (100.0 * f.dropRatio) << "%\n";

    // LR-WPAN section never prints (activeLrwpan is always false).
    if (cfg.activeLrwpan && !cfg.activeWifi)
    {
        ofs << "  [LR-WPAN TCP Diagnostics]\n";
        ofs << "    Sender apps       : " << m.lrwpanSenderApps << "\n";
        ofs << "    Connect attempts  : " << m.lrConnectAttempts << "\n";
        ofs << "    Connect immediate fail: " << m.lrConnectImmediateFail << "\n";
        ofs << "    Connect success   : " << m.lrConnectSuccess << "\n";
        ofs << "    Connect-fail callback: " << m.lrConnectFailCallback << "\n";
        ofs << "    Connect timeouts  : " << m.lrConnectTimeouts << "\n";
        ofs << "    Send success calls: " << m.lrSendSuccess << "\n";
        ofs << "    Send blocked calls: " << m.lrSendBlocked << "\n";
        ofs << "    ConnectionSucceeded trace: " << m.lrConnectionSucceededTrace << "\n";
        ofs << "    State SYN_SENT    : " << m.lrStateSynSent << "\n";
        ofs << "    State ESTABLISHED : " << m.lrStateEstablished << "\n";
        ofs << "    Update accepts    : " << m.lrUpdateAccepts << "\n";
        ofs << "    Interactive accepts: " << m.lrInteractiveAccepts << "\n";
        ofs << "    Update sink bytes : " << m.lrUpdateRxBytes << "\n";
        ofs << "    Interactive sink bytes: " << m.lrInteractiveRxBytes << "\n";
    }
}

// =========================================================================
// XML / flow helpers
// =========================================================================

struct SenderTarget
{
    Ptr<Node> node;
    Address   updatePeer;
    Address   interactivePeer;
    std::string domain;
};

static std::string
ExtractXmlAttr(const std::string& line, const std::string& key)
{
    std::string needle = key + "=\"";
    auto p = line.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    auto q = line.find('"', p);
    return (q == std::string::npos) ? "" : line.substr(p, q - p);
}

static void
BuildFlowPortMap(Ptr<FlowClassifier> clf,
                 std::map<uint32_t, std::pair<uint16_t, std::string>>& out)
{
    if (!clf) return;
    std::ostringstream oss;
    clf->SerializeToXmlStream(oss, 0);
    std::istringstream iss(oss.str());
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("<Flow ") == std::string::npos) continue;
        auto sid   = ExtractXmlAttr(line, "flowId");
        auto src   = ExtractXmlAttr(line, "sourceAddress");
        auto dport = ExtractXmlAttr(line, "destinationPort");
        if (sid.empty() || dport.empty()) continue;
        uint32_t fid  = static_cast<uint32_t>(std::stoul(sid));
        uint16_t port = static_cast<uint16_t>(std::stoul(dport));
        out[fid] = {port, src};
    }
}

// =========================================================================
// RunScenario
// =========================================================================

static RunMetrics
RunScenario(const ScenarioConfig& cfg)
{
    RunMetrics metrics;

    const uint16_t UPDATE_PORT      = 9000;
    const uint16_t INTERACTIVE_PORT = 9001;

    // ── Global TCP / rLEDBAT defaults ─────────────────────────────────────
    Config::SetDefault("ns3::TcpSocketBase::Timestamp",  BooleanValue(true));
    Config::SetDefault("ns3::TcpSocketBase::Sack",       BooleanValue(false));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpNewReno"));

    Config::SetDefault("ns3::TcpRledbat::TargetDelay",
                       TimeValue(MilliSeconds(cfg.targetDelayMs)));
    Config::SetDefault("ns3::TcpRledbat::BaseDelayHistorySize",
                       UintegerValue(cfg.baseDelayHistSize));
    Config::SetDefault("ns3::TcpRledbat::BaseDelayRefreshInterval",
                       TimeValue(Seconds(cfg.baseDelayRefreshSec)));
    Config::SetDefault("ns3::TcpRledbat::EnableCadf", BooleanValue(cfg.enableCadf));
    Config::SetDefault("ns3::TcpRledbat::EnableEcs",  BooleanValue(cfg.enableEcs));
    Config::SetDefault("ns3::TcpRledbat::EnableAtd",  BooleanValue(cfg.enableAtd));
    Config::SetDefault("ns3::TcpRledbat::CadfStreakThreshold",
                       UintegerValue(cfg.cadfStreakThreshold));
    Config::SetDefault("ns3::TcpRledbat::CadfSpikeRatio",
                       DoubleValue(cfg.cadfSpikeRatio));
    Config::SetDefault("ns3::TcpRledbat::CadfMinAbsoluteSpike",
                       DoubleValue(cfg.cadfMinAbsoluteSpike));
    Config::SetDefault("ns3::TcpRledbat::CadfWindowSize",
                       UintegerValue(cfg.cadfWindowSize));
    Config::SetDefault("ns3::TcpRledbat::EcsBeta",    DoubleValue(cfg.ecsBeta));
    Config::SetDefault("ns3::TcpRledbat::AtdAlpha",   DoubleValue(cfg.atdAlpha));
    Config::SetDefault("ns3::TcpRledbat::AtdMinTarget",
                       TimeValue(MilliSeconds(cfg.atdMinTargetMs)));
    // TCP-layer ECN: must be On for ECS so the sender participates in ECE/CWR.
    Config::SetDefault("ns3::TcpSocketBase::UseEcn",
                       StringValue(cfg.enableEcn ? "On" : "Off"));

    NS_ABORT_MSG_IF(!cfg.activeWifi,
                    "Only WiFi mode is supported in this build.");

    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    uint32_t bufSize = cfg.pps * 1448u;
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(bufSize));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(bufSize));

    // ── Nodes ─────────────────────────────────────────────────────────────
    NodeContainer nodes;
    nodes.Create(cfg.numNodes);
    Ptr<Node> receiver = nodes.Get(0);

    // node0=receiver  node1=GW_wifi_tx  node2=CORE  node3=CORE_wifi
    const uint32_t INFRA = 4;
    NS_ABORT_MSG_IF(cfg.numNodes <= INFRA,
                    "numNodes must be > 4 (min 5 for WiFi infra + 1 sender)");
    const int32_t gwWifiTxId   = 1;
    const int32_t coreRouterId = 2;
    const int32_t coreWifiId   = 3;
    uint32_t rem = cfg.numNodes - INFRA;
    uint32_t txN = rem / 2;
    uint32_t rxN = rem - txN;
    std::vector<uint32_t> wifiTxIds, wifiRxRelayIds;
    uint32_t cur = INFRA;
    for (uint32_t i = 0; i < txN; ++i) wifiTxIds.push_back(cur++);
    for (uint32_t i = 0; i < rxN; ++i) wifiRxRelayIds.push_back(cur++);

    // ── Internet stack (IPv4 + Nix-vector routing) ────────────────────────
    Ipv4NixVectorHelper nixRouting;
    InternetStackHelper internet;
    internet.SetRoutingHelper(nixRouting);
    internet.Install(nodes);

    // ── Energy sources ────────────────────────────────────────────────────
    BasicEnergySourceHelper esh;
    esh.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(100.0));
    EnergySourceContainer energySources = esh.Install(nodes);

    // ── Static mobility base — overridden for movers below ────────────────
    MobilityHelper smob;
    smob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    smob.Install(nodes);

    // Place infra nodes at predictable positions.
    receiver->GetObject<ConstantPositionMobilityModel>()->SetPosition({0, 0, 0});
    nodes.Get(gwWifiTxId)  ->GetObject<ConstantPositionMobilityModel>()->SetPosition({0,  0, 0});
    nodes.Get(coreRouterId)->GetObject<ConstantPositionMobilityModel>()->SetPosition({50, 0, 0});
    nodes.Get(coreWifiId)  ->GetObject<ConstantPositionMobilityModel>()->SetPosition({0,  0, 0});

    // ── WiFi — two isolated channels ─────────────────────────────────────
    NodeContainer wifiTxNodes, wifiRxNodes;
    wifiTxNodes.Add(nodes.Get(gwWifiTxId));
    for (uint32_t id : wifiTxIds)      wifiTxNodes.Add(nodes.Get(id));
    wifiRxNodes.Add(receiver);
    wifiRxNodes.Add(nodes.Get(coreWifiId));
    for (uint32_t id : wifiRxRelayIds) wifiRxNodes.Add(nodes.Get(id));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",    StringValue("DsssRate11Mbps"),
                                 "ControlMode", StringValue("DsssRate1Mbps"));

    YansWifiChannelHelper chHelper = YansWifiChannelHelper::Default();
    chHelper.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    chHelper.AddPropagationLoss("ns3::RangePropagationLossModel",
                                "MaxRange", DoubleValue(4.0 * cfg.txRangeM));

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    // Sender-side channel — GW_wifi_tx + senders
    YansWifiPhyHelper phyTx;
    phyTx.SetChannel(chHelper.Create()); // new independent channel
    phyTx.Set("TxPowerStart", DoubleValue(20.0));
    phyTx.Set("TxPowerEnd",   DoubleValue(20.0));
    NetDeviceContainer wifiTxDevs = wifi.Install(phyTx, mac, wifiTxNodes);

    // Receiver-side channel — CORE_wifi + relays + receiver
    YansWifiPhyHelper phyRx;
    phyRx.SetChannel(chHelper.Create()); // different independent channel
    phyRx.Set("TxPowerStart", DoubleValue(20.0));
    phyRx.Set("TxPowerEnd",   DoubleValue(20.0));
    NetDeviceContainer wifiRxDevs = wifi.Install(phyRx, mac, wifiRxNodes);

    // Energy models — separate containers so devs[i] and sources[i] match.
    WifiRadioEnergyModelHelper reh;
    EnergySourceContainer txES, rxES;
    for (uint32_t i = 0; i < wifiTxNodes.GetN(); ++i)
        txES.Add(energySources.Get(wifiTxNodes.Get(i)->GetId()));
    reh.Install(wifiTxDevs, txES);
    for (uint32_t i = 0; i < wifiRxNodes.GetN(); ++i)
        rxES.Add(energySources.Get(wifiRxNodes.Get(i)->GetId()));
    reh.Install(wifiRxDevs, rxES);

    // P2P backbone: GW_wifi_tx ↔ CORE ↔ CORE_wifi
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate",  StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay",    StringValue("2ms"));
    NetDeviceContainer d1 = p2p.Install(nodes.Get(gwWifiTxId),   nodes.Get(coreRouterId));
    NetDeviceContainer d2 = p2p.Install(nodes.Get(coreRouterId), nodes.Get(coreWifiId));

    // ── RED queue on CORE's outgoing P2P port toward CORE_wifi ────────────
    //
    // d2.Get(0) is the device on coreRouterId (CORE).  This mirrors the
    // r0→n0 bottleneck port in RledbatTest.cc.  All sender traffic exits
    // CORE via this port before entering the receiver-side WiFi domain.
    //
    // ECS path (enableEcn=true):
    //   MinTh=50p  → ~5.8 ms queuing → CE marking fires before targetDelay=10 ms,
    //   giving ECS an early-warning advantage over the delay-based controller.
    //   MaxTh=100p gives a wide probabilistic marking range.
    //
    // Non-ECS path (enableEcn=false):
    //   MinTh=150p, MaxTh=200p, MaxSize=200p → RED behaves like a large DropTail.
    //   rLEDBAT can observe queuing delay build freely without premature drops
    //   interfering with its delay signal.  RED fires only as a hard backstop.
    if (cfg.configureQueue)
    {
        TrafficControlHelper tch;
        double minTh = cfg.enableEcn ?  50.0 : 150.0;
        double maxTh = cfg.enableEcn ? 100.0 : 200.0;

        tch.SetRootQueueDisc("ns3::RedQueueDisc",
                             "MinTh",         DoubleValue(minTh),
                             "MaxTh",         DoubleValue(maxTh),
                             "MaxSize",       QueueSizeValue(QueueSize("200p")),
                             "LinkBandwidth", StringValue("100Mbps"),
                             "LinkDelay",     StringValue("2ms"),
                             "UseEcn",        BooleanValue(cfg.enableEcn));
        tch.Install(d2.Get(0));
    }

    // ── IP addressing ─────────────────────────────────────────────────────
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.0.0", "255.255.255.0");
    ipv4.Assign(wifiTxDevs);
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer rxIf = ipv4.Assign(wifiRxDevs);
    ipv4.SetBase("10.10.0.0", "255.255.255.252");
    ipv4.Assign(d1);
    ipv4.SetBase("10.10.0.4", "255.255.255.252");
    ipv4.Assign(d2);

    Ipv4Address recvAddr = rxIf.GetAddress(0); // receiver's address on ch2

    // ── Mobility for sender/relay movers ──────────────────────────────────
    // Constrained to ±0.3×txRangeM around domain anchor so nodes always
    // remain reachable to GW_wifi_tx or CORE_wifi respectively.
    double hs = 0.3 * cfg.txRangeM;
    std::string mn = std::to_string(-hs), mx = std::to_string(hs);
    std::string sp = std::to_string(cfg.speedMps);

    auto makeMob = [&]() -> MobilityHelper {
        ObjectFactory pf;
        pf.SetTypeId("ns3::RandomRectanglePositionAllocator");
        pf.Set("X", StringValue("ns3::UniformRandomVariable[Min=" + mn + "|Max=" + mx + "]"));
        pf.Set("Y", StringValue("ns3::UniformRandomVariable[Min=" + mn + "|Max=" + mx + "]"));
        Ptr<PositionAllocator> wp = pf.Create()->GetObject<PositionAllocator>();
        MobilityHelper mob;
        mob.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                 "X", StringValue("ns3::UniformRandomVariable[Min=" + mn + "|Max=" + mx + "]"),
                                 "Y", StringValue("ns3::UniformRandomVariable[Min=" + mn + "|Max=" + mx + "]"));
        mob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                             "Speed", StringValue("ns3::ConstantRandomVariable[Constant=" + sp + "]"),
                             "Pause", StringValue("ns3::ConstantRandomVariable[Constant=0.5]"),
                             "PositionAllocator", PointerValue(wp));
        return mob;
    };

    NodeContainer txMovers, rxMovers;
    for (uint32_t id : wifiTxIds)      txMovers.Add(nodes.Get(id));
    for (uint32_t id : wifiRxRelayIds) rxMovers.Add(nodes.Get(id));
    if (txMovers.GetN() > 0) makeMob().Install(txMovers);
    if (rxMovers.GetN() > 0) makeMob().Install(rxMovers);

    // ── Sender pool ───────────────────────────────────────────────────────
    std::vector<SenderTarget> senderPool;
    for (uint32_t id : wifiTxIds)
    {
        SenderTarget st;
        st.node            = nodes.Get(id);
        st.updatePeer      = InetSocketAddress(recvAddr, UPDATE_PORT);
        st.interactivePeer = InetSocketAddress(recvAddr, INTERACTIVE_PORT);
        st.domain          = "wifi";
        senderPool.push_back(st);
    }
    NS_ABORT_MSG_IF(senderPool.empty(), "No sender nodes in pool (numNodes too small)");

    // ── Dat files ─────────────────────────────────────────────────────────
    const std::string datDir = "scratch/dat/";
    std::ofstream wFile, qFile, bFile;
    if (cfg.enableTraceDat)
    {
        wFile.open(datDir + "final-wledbat-"   + cfg.tag + ".dat");
        qFile.open(datDir + "final-qdelay-"    + cfg.tag + ".dat");
        bFile.open(datDir + "final-basedelay-" + cfg.tag + ".dat");
        wFile << "# t(s) W_ledbat(bytes)\n";
        qFile << "# t(s) QueuingDelay(ms)\n";
        bFile << "# t(s) BaseDelay(ms)\n";
    }

    // ── Sinks on receiver ─────────────────────────────────────────────────
    // Port 9000: TcpRledbat — Fork() clones class; WLedbat trace attaches
    //            in HandleAccept() for every new connection.
    // Port 9001: TcpSocketBase (interactive, normal TCP).
    std::vector<Ptr<CustomSink>> sinks;

    Ptr<CustomSink> updateSink = CreateObject<CustomSink>();
    updateSink->Setup(TypeId::LookupByName("ns3::TcpRledbat"),
                      UPDATE_PORT,
                      MilliSeconds(cfg.targetDelayMs),
                      cfg.enableEcn,
                      false /*ipv4*/);
    if (cfg.enableTraceDat) updateSink->SetTraceFiles(&wFile, &qFile, &bFile);
    receiver->AddApplication(updateSink);
    updateSink->SetStartTime(Seconds(0.0));
    updateSink->SetStopTime(Seconds(cfg.simTimeSec));
    sinks.push_back(updateSink);

    Ptr<CustomSink> interactiveSink = CreateObject<CustomSink>();
    interactiveSink->Setup(TcpSocketBase::GetTypeId(),
                           INTERACTIVE_PORT, Time(0), false, false /*ipv4*/);
    receiver->AddApplication(interactiveSink);
    interactiveSink->SetStartTime(Seconds(0.05));
    interactiveSink->SetStopTime(Seconds(cfg.simTimeSec));
    sinks.push_back(interactiveSink);

    // ── Flow creation ─────────────────────────────────────────────────────
    // Senders use plain TcpSocketBase — the receiver-side SocketClass
    // (set in CustomSink::StartApplication) controls whether port 9000
    // connections are governed by rLEDBAT.  The sender is unmodified.
    uint32_t numUpdate = cfg.numFlows / 2;
    for (uint32_t f = 0; f < cfg.numFlows; ++f)
    {
        const SenderTarget& st = senderPool[f % senderPool.size()];
        bool isUpdate = (f < numUpdate);
        double start = cfg.warmupSec + 0.1 * (f % senderPool.size()) + 0.02 * f;

        Ptr<RetryBulkSender> app = CreateObject<RetryBulkSender>();
        app->Setup(isUpdate ? st.updatePeer : st.interactivePeer,
                   1448u,
                   cfg.pps,
                   TcpSocketBase::GetTypeId(),
                   false,  // forceClass: irrelevant for sender
                   false,  // connectWatchdog: WiFi handshakes complete reliably
                   10.0,
                   nullptr, nullptr, nullptr);
        st.node->AddApplication(app);
        app->SetStartTime(Seconds(start));
        app->SetStopTime(Seconds(cfg.simTimeSec));
    }

    // ── Run ───────────────────────────────────────────────────────────────
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMon = fmHelper.InstallAll();
    Simulator::Stop(Seconds(cfg.simTimeSec));
    Simulator::Run();

    // ── Post-run metrics ──────────────────────────────────────────────────
    flowMon->CheckForLostPackets();
    std::map<uint32_t, std::pair<uint16_t, std::string>> flowInfo;
    BuildFlowPortMap(fmHelper.GetClassifier(),  flowInfo);
    BuildFlowPortMap(fmHelper.GetClassifier6(), flowInfo);

    double measDuration = std::max(1e-6, cfg.simTimeSec - cfg.warmupSec);
    double rRxBytes = 0, rTxPkts = 0, rRxPkts = 0, rDelaySum = 0, rDurSum = 0, rJitterSum = 0;
    double iRxBytes = 0, iTxPkts = 0, iRxPkts = 0, iDelaySum = 0, iDurSum = 0, iJitterSum = 0;
    std::map<std::string, std::pair<double, double>> sUpdateBD, sInterBD;

    for (const auto& [fid, fs] : flowMon->GetFlowStats())
    {
        // Skip failed handshakes (SYN/SYN-ACK packets; no payload delivered).
        if (fs.rxBytes == 0 && fs.rxPackets == 0) continue;

        auto it = flowInfo.find(fid);
        if (it == flowInfo.end()) continue;
        uint16_t dport  = it->second.first;
        std::string src = it->second.second.empty() ? "unknown" : it->second.second;
        if (dport != UPDATE_PORT && dport != INTERACTIVE_PORT) continue;

        double txStart   = std::max(cfg.warmupSec, fs.timeFirstTxPacket.GetSeconds());
        double rxEnd     = fs.timeLastRxPacket.GetSeconds();
        double activeDur = std::max(0.0, rxEnd - txStart);
        if (fs.rxBytes > 0 && activeDur <= 0.0) activeDur = measDuration;

        double tp  = (activeDur > 0.0)
                         ? static_cast<double>(fs.rxBytes) * 8.0 / activeDur / 1e6 : 0.0;
        double pdr = (fs.txPackets > 0)
                         ? static_cast<double>(fs.rxPackets) / fs.txPackets : 0.0;
        double lost  = (fs.txPackets >= fs.rxPackets)
                           ? static_cast<double>(fs.txPackets - fs.rxPackets) : 0.0;
        double drop  = (fs.txPackets > 0) ? lost / fs.txPackets : 0.0;
        double delay = (fs.rxPackets > 0)
                           ? fs.delaySum.GetMilliSeconds() / fs.rxPackets : 0.0;
        double jitter = (fs.rxPackets > 1)
                            ? fs.jitterSum.GetMilliSeconds() / (fs.rxPackets - 1) : 0.0;

        RunMetrics::FlowMetrics rec;
        rec.flowId      = fid;  rec.src       = src;
        rec.dport       = dport; rec.klass    = (dport == UPDATE_PORT) ? "rLEDBAT" : "Interactive";
        rec.activeDurSec = activeDur; rec.tputMbps = tp;
        rec.delayMs      = delay;     rec.jitterMs = jitter;
        rec.pdr          = pdr;       rec.dropRatio = drop;
        metrics.perFlow.push_back(rec);

        if (dport == UPDATE_PORT)
        {
            rRxBytes += static_cast<double>(fs.rxBytes);
            rTxPkts  += static_cast<double>(fs.txPackets);
            rRxPkts  += static_cast<double>(fs.rxPackets);
            rDelaySum  += fs.delaySum.GetMilliSeconds();
            rJitterSum += fs.jitterSum.GetMilliSeconds();
            rDurSum    += activeDur;
            sUpdateBD[src].first += static_cast<double>(fs.rxBytes);
            sUpdateBD[src].second += activeDur;
        }
        else
        {
            iRxBytes += static_cast<double>(fs.rxBytes);
            iTxPkts  += static_cast<double>(fs.txPackets);
            iRxPkts  += static_cast<double>(fs.rxPackets);
            iDelaySum  += fs.delaySum.GetMilliSeconds();
            iJitterSum += fs.jitterSum.GetMilliSeconds();
            iDurSum    += activeDur;
            sInterBD[src].first += static_cast<double>(fs.rxBytes);
            sInterBD[src].second += activeDur;
        }
    }

    metrics.rledbatTputMbps    = (rDurSum > 0) ? rRxBytes * 8.0 / measDuration / 1e6 : 0.0;
    metrics.interactiveTputMbps = (iDurSum > 0) ? iRxBytes * 8.0 / measDuration / 1e6 : 0.0;
    metrics.totalTputMbps      = metrics.rledbatTputMbps + metrics.interactiveTputMbps;
    metrics.yieldingRatio      = (metrics.interactiveTputMbps > 0)
                                     ? metrics.rledbatTputMbps / metrics.interactiveTputMbps : 0.0;

    metrics.rledbatDelayMs     = (rRxPkts > 0) ? rDelaySum / rRxPkts : 0.0;
    metrics.interactiveDelayMs = (iRxPkts > 0) ? iDelaySum / iRxPkts : 0.0;
    double totRxPkts           = rRxPkts + iRxPkts;
    metrics.avgDelayMs         = (totRxPkts > 0) ? (rDelaySum + iDelaySum) / totRxPkts : 0.0;

    double rJS = (rRxPkts > 0) ? rRxPkts - 1 : 0;
    double iJS = (iRxPkts > 0) ? iRxPkts - 1 : 0;
    metrics.rledbatJitterMs     = (rJS > 0) ? rJitterSum / rJS : 0.0;
    metrics.interactiveJitterMs = (iJS > 0) ? iJitterSum / iJS : 0.0;
    metrics.avgJitterMs         = (rJS + iJS > 0) ? (rJitterSum + iJitterSum) / (rJS + iJS) : 0.0;

    metrics.rledbatPDR     = (rTxPkts > 0) ? rRxPkts / rTxPkts : 0.0;
    metrics.interactivePDR = (iTxPkts > 0) ? iRxPkts / iTxPkts : 0.0;
    double totTx           = rTxPkts + iTxPkts;
    metrics.totalPDR       = (totTx > 0) ? (rRxPkts + iRxPkts) / totTx : 0.0;

    double rL = (rTxPkts >= rRxPkts) ? rTxPkts - rRxPkts : 0;
    double iL = (iTxPkts >= iRxPkts) ? iTxPkts - iRxPkts : 0;
    metrics.rledbatDropRatio     = (rTxPkts > 0) ? rL / rTxPkts : 0.0;
    metrics.interactiveDropRatio = (iTxPkts > 0) ? iL / iTxPkts : 0.0;
    metrics.totalDropRatio       = (totTx > 0) ? (rL + iL) / totTx : 0.0;

    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<BasicEnergySource> es = DynamicCast<BasicEnergySource>(energySources.Get(i));
        if (es) metrics.totalEnergyJ += 100.0 - es->GetRemainingEnergy();
    }
    metrics.avgEnergyPerNodeJ = metrics.totalEnergyJ / cfg.numNodes;

    for (const auto& [s, bd] : sUpdateBD)
        metrics.perSenderTput.push_back({"rLEDBAT src=" + s,
                                         (bd.second > 0) ? bd.first * 8.0 / bd.second / 1e6 : 0.0});
    for (const auto& [s, bd] : sInterBD)
        metrics.perSenderTput.push_back({"Interactive src=" + s,
                                         (bd.second > 0) ? bd.first * 8.0 / bd.second / 1e6 : 0.0});

    double sx = 0, sx2 = 0; int nn = 0;
    for (const auto& [l, t] : metrics.perSenderTput)
    {
        (void)l; sx += t; sx2 += t * t; ++nn;
    }
    metrics.jainFairness = (nn > 0 && sx2 > 0)
                               ? (sx * sx) / (static_cast<double>(nn) * sx2) : 1.0;

    Simulator::Destroy();
    return metrics;
}

// =========================================================================
// Helpers
// =========================================================================

static void
PrintSectionHeader(std::ofstream& ofs, const std::string& title)
{
    std::string bar(68, '=');
    std::cout << "\n" << bar << "\n  " << title << "\n" << bar << "\n";
    ofs  << "\n" << bar << "\n  " << title << "\n" << bar << "\n";
}

// =========================================================================
// main
// =========================================================================

int
main(int argc, char* argv[])
{
    // Ensure scratch/dat/ exists before any run creates trace files.
    {
        namespace fs = std::filesystem;
        try { fs::create_directories("scratch/dat"); }
        catch (const std::exception& e)
        {
            std::cerr << "[WARN] Could not create scratch/dat/: " << e.what() << "\n";
        }
    }

    Time::SetResolution(Time::NS);

    bool activeWifi = true; // WiFi-only build; keep for backward compat with CLI

    uint32_t fixedNodes = 60;
    uint32_t fixedFlows = 30;
    uint32_t fixedPps   = 300;
    double fixedSpeed   = 15.0;

    double simTimeSec  = 60.0;
    double warmupSec   = 10.0;
    double txRangeM    = 50.0;

    double   targetDelayMs  = 100.0;
    uint32_t baseHistSize   = 5;
    double   baseRefreshSec = 5.0;

    std::vector<uint32_t> sweepNodes{20, 40, 60, 80, 100};
    std::vector<uint32_t> sweepFlows{10, 20, 30, 40, 50};
    std::vector<uint32_t> sweepPps{100, 200, 300, 400, 500};
    std::vector<double>   sweepSpeed{5, 10, 15, 20, 25};

    bool enableTraceDat = true;
    bool singleRun      = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("activeWifi",  "Must remain true (WiFi-only build)", activeWifi);
    cmd.AddValue("fixedNodes",  "Fixed node count",        fixedNodes);
    cmd.AddValue("fixedFlows",  "Fixed flow count",        fixedFlows);
    cmd.AddValue("fixedPps",    "Fixed pps",               fixedPps);
    cmd.AddValue("fixedSpeed",  "Fixed WiFi speed m/s",    fixedSpeed);
    cmd.AddValue("simTime",     "Simulation time s",       simTimeSec);
    cmd.AddValue("warmup",      "Warmup period s",         warmupSec);
    cmd.AddValue("txRange",     "Reference Tx range m",    txRangeM);
    cmd.AddValue("targetDelay", "rLEDBAT target delay ms", targetDelayMs);
    cmd.AddValue("baseHistSize","rLEDBAT BaseDelayHistorySize", baseHistSize);
    cmd.AddValue("baseRefresh", "rLEDBAT BaseDelayRefreshInterval s", baseRefreshSec);
    cmd.AddValue("traceDat",    "Write .dat trace files",  enableTraceDat);
    cmd.AddValue("singleRun",   "One run (all mods, fixed params, skip sweeps)", singleRun);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(!activeWifi, "activeWifi must be true in this build.");

    std::ofstream ofs("scratch/final_integrated_results.txt",
                      std::ios::out | std::ios::trunc);
    ofs << "====================================================================\n";
    ofs << " Final Integrated Test Results\n";
    ofs << " Domains: wifi=on lrwpan=off wired=off\n";
    ofs << " One-factor-at-a-time sweeps (base + modified) plus individual-mod runs\n";
    ofs << "====================================================================\n";

    auto runOne = [&](const std::string& sn, const std::string& vl,
                      const ScenarioConfig& c) {
        std::cout << "  Running [" << c.tag << "] ... " << std::flush;
        auto m = RunScenario(c);
        std::cout << "done\n";
        WriteMetrics(ofs, c, m);
        (void)sn; (void)vl;
    };

    auto baseTemplate = [&]() -> ScenarioConfig {
        ScenarioConfig cfg;
        cfg.activeWifi          = true;
        cfg.activeLrwpan        = false;
        cfg.activeWired         = false;
        cfg.numNodes            = fixedNodes;
        cfg.numFlows            = fixedFlows;
        cfg.pps                 = fixedPps;
        cfg.speedMps            = fixedSpeed;
        cfg.coverageMultiplier  = 3.0; // format compat only
        cfg.simTimeSec          = simTimeSec;
        cfg.warmupSec           = warmupSec;
        cfg.txRangeM            = txRangeM;
        cfg.targetDelayMs       = targetDelayMs;
        cfg.baseDelayHistSize   = baseHistSize;
        cfg.baseDelayRefreshSec = baseRefreshSec;
        cfg.enableTraceDat      = enableTraceDat;
        // All mod flags and configureQueue default to false via struct init.
        return cfg;
    };

    // ── Single-run shortcut (--singleRun) ─────────────────────────────────
    if (singleRun)
    {
        PrintSectionHeader(ofs, "Single run: all modifications, fixed params");
        ScenarioConfig cfg = baseTemplate();
        cfg.enableMods     = true;
        cfg.enableCadf     = true;
        cfg.enableEcs      = true;
        cfg.enableAtd      = true;
        cfg.enableEcn      = true;  // required for ECS
        cfg.configureQueue = true;  // RED+ECN queue
        cfg.tag = "single_run";
        runOne("single", "single", cfg);
        std::cout << "\nSingle-run complete. Results: scratch/final_integrated_results.txt\n";
        std::cout << "Dat files: scratch/dat/\n";
        return 0;
    }

    // =========================================================================
    // PASS 1 — base (no mods, no queue)
    // PASS 2 — modified (all mods, RED+ECN queue)
    // =========================================================================
    for (int pass = 0; pass < 2; ++pass)
    {
        bool mods = (pass == 1);
        std::string lbl = mods ? "modified" : "base";

        PrintSectionHeader(ofs, std::string("PASS: ") + lbl);

        // Apply pass-level flags uniformly to any config.
        auto applyPass = [&](ScenarioConfig& cfg) {
            cfg.enableMods     = mods;
            cfg.enableCadf     = mods;
            cfg.enableEcs      = mods;
            cfg.enableAtd      = mods;
            cfg.enableEcn      = mods; // TCP ECN + RED UseEcn for modified pass
            cfg.configureQueue = mods; // RED+ECN queue for modified pass only
        };

        // Sweep: nodes
        PrintSectionHeader(ofs, "Sweep NODES");
        for (uint32_t v : sweepNodes)
        {
            ScenarioConfig cfg = baseTemplate();
            applyPass(cfg);
            cfg.numNodes = v;
            std::ostringstream ss;
            ss << "pass_" << lbl << "_nodes_" << v;
            cfg.tag = ss.str();
            runOne("nodes", std::to_string(v), cfg);
        }

        // Sweep: flows
        PrintSectionHeader(ofs, "Sweep FLOWS");
        for (uint32_t v : sweepFlows)
        {
            ScenarioConfig cfg = baseTemplate();
            applyPass(cfg);
            cfg.numFlows = v;
            std::ostringstream ss;
            ss << "pass_" << lbl << "_flows_" << v;
            cfg.tag = ss.str();
            runOne("flows", std::to_string(v), cfg);
        }

        // Sweep: pps
        PrintSectionHeader(ofs, "Sweep PPS");
        for (uint32_t v : sweepPps)
        {
            ScenarioConfig cfg = baseTemplate();
            applyPass(cfg);
            cfg.pps = v;
            std::ostringstream ss;
            ss << "pass_" << lbl << "_pps_" << v;
            cfg.tag = ss.str();
            runOne("pps", std::to_string(v), cfg);
        }

        // Sweep: speed (WiFi mobile)
        PrintSectionHeader(ofs, "Sweep SPEED (WiFi mobile)");
        for (double v : sweepSpeed)
        {
            ScenarioConfig cfg = baseTemplate();
            applyPass(cfg);
            cfg.speedMps = v;
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(0)
               << "pass_" << lbl << "_speed_" << v;
            cfg.tag = ss.str();
            runOne("speed", std::to_string(v), cfg);
        }
    }

    // =========================================================================
    // Extra runs — individual modifications at fixed parameters
    //
    //   CADF-only : enableCadf=true, non-ECN queue (DropTail-like RED)
    //   ECS-only  : enableEcs=true, enableEcn=true, RED+ECN queue
    //   ATD-only  : enableAtd=true, non-ECN queue (DropTail-like RED)
    //
    // These isolate each modification's contribution for the report.
    // =========================================================================

    PrintSectionHeader(ofs, "Run: CADF only (fixed params, non-ECN DropTail-like queue)");
    {
        ScenarioConfig cfg = baseTemplate();
        cfg.enableMods     = true;
        cfg.enableCadf     = true;
        // enableEcn=false → non-ECN RED queue (MinTh=150p, MaxTh=200p)
        cfg.configureQueue = true;
        cfg.tag = "run_cadf_only";
        runOne("cadf", "cadf_only", cfg);
    }

    PrintSectionHeader(ofs, "Run: ECS only (fixed params, RED+ECN queue)");
    {
        ScenarioConfig cfg = baseTemplate();
        cfg.enableMods     = true;
        cfg.enableEcs      = true;
        cfg.enableEcn      = true;  // required: TCP ECN + RED CE marking
        cfg.configureQueue = true;  // RED+ECN queue (MinTh=50p, MaxTh=200p)
        cfg.tag = "run_ecs_only";
        runOne("ecs", "ecs_only", cfg);
    }

    PrintSectionHeader(ofs, "Run: ATD only (fixed params, non-ECN DropTail-like queue)");
    {
        ScenarioConfig cfg = baseTemplate();
        cfg.enableMods     = true;
        cfg.enableAtd      = true;
        // enableEcn=false → non-ECN RED queue
        cfg.configureQueue = true;
        cfg.tag = "run_atd_only";
        runOne("atd", "atd_only", cfg);
    }

    std::cout << "\nFinal integrated test complete.\n";
    std::cout << "Results : scratch/final_integrated_results.txt\n";
    std::cout << "Dat files: scratch/dat/\n";

    return 0;
}
