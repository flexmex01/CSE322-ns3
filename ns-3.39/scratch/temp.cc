/*
 * FinalIntegratedTest.cc
 *
 * Unified experiment harness for active-domain experiments:
 *   - 802.11 mobile WiFi (two-segment WiFi + p2p backbone)
 *   - 802.15.4 static LR-WPAN + 6LoWPAN (star)
 *
 * High-level goals:
 *   - One receiver node hosts both sinks:
 *       port 9000 -> update/background (TcpRledbat)
 *       port 9001 -> interactive/high-priority (TcpSocketBase)
 *   - One-factor-at-a-time parameter sweeps
 *   - Two passes for each test point:
 *       (1) base rLEDBAT
 *       (2) modified rLEDBAT (CADF+ECS+ATD enabled)
 *
 * ---------------------------------------------------------------------------
 * Topology diagram (as implemented):
 *
 * WiFi-only mode (--activeWifi=1 --activeLrwpan=0)
 *
 *   Sender WiFi domain (mobile)          p2p backbone         Receiver WiFi domain
 *   --------------------------------     ------------         ---------------------
 *   wifi sender nodes ----\                                      relay nodes...
 *                          +-- [GW_wifi_tx] -- [CORE] -- [CORE_wifi] --\
 *   wifi sender nodes ----/                                              +-- [R node0]
 *                                                                             sinks 9000/9001
 *
 *   - Two independent WiFi channels are used (sender-side and receiver-side).
 *   - GW/core/coreWifi are mandatory middle hops by construction.
 *   - Sender-side WiFi nodes and receiver-side relay nodes are mobile.
 *
 * LR-WPAN-only mode (--activeWifi=0 --activeLrwpan=1)
 *
 *                        LR-WPAN + 6LoWPAN static star
 *                        -----------------------------
 *                 lr sender n1   lr sender n2 ... lr sender nk
 *                         \            |                 /
 *                                   [R node0]
 *                               coordinator + sinks 9000/9001
 *
 *   - Single LR-WPAN domain; no WiFi/p2p forwarding path is active.
 *   - Sender nodes are static and clustered around the coordinator.
 * ---------------------------------------------------------------------------
 */

#include "ns3/tcp-rledbat.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/energy-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/lr-wpan-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nix-vector-routing-module.h"
#include "ns3/olsr-module.h"
#include "ns3/packet-sink.h"
#include "ns3/point-to-point-module.h"
#include "ns3/sixlowpan-module.h"
#include "ns3/wifi-module.h"

#include <algorithm>
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

static void
AttachRledbatTraces(std::ofstream* wFile, std::ofstream* qFile, std::ofstream* bFile)
{
    auto matches = Config::LookupMatches("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*");
    for (auto it = matches.Begin(); it != matches.End(); ++it)
    {
        Ptr<TcpRledbat> r = DynamicCast<TcpRledbat>(*it);
        if (!r)
        {
            continue;
        }

        bool ok = r->TraceConnectWithoutContext("WLedbat",
                                                MakeBoundCallback(&TraceWLedbat, wFile));
        if (ok)
        {
            r->TraceConnectWithoutContext("QueuingDelay",
                                          MakeBoundCallback(&TraceDelay, qFile));
            r->TraceConnectWithoutContext("BaseDelay",
                                          MakeBoundCallback(&TraceDelay, bFile));
        }
    }
}

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

    uint64_t
    GetTotalRx() const
    {
        return m_totalRx;
    }
    uint32_t GetAcceptCount() const { return m_acceptCount; }

  private:
    // Listener startup sequence:
    //   1) temporarily set node TCP default to requested socket class,
    //   2) create/bind/listen accept socket,
    //   3) restore previous default so unrelated sockets are unaffected.
    void
    StartApplication() override
    {
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        TypeIdValue prevSocketClass;
        tcp->GetAttribute("SocketClass", prevSocketClass);
        tcp->SetAttribute("SocketClass", TypeIdValue(m_socketClassId));
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        tcp->SetAttribute("SocketClass", prevSocketClass);

        if (m_useEcn)
        {
            m_socket->SetAttribute("UseEcn", StringValue("On"));
        }

        if (!m_targetDelay.IsZero() &&
            m_socketClassId == TypeId::LookupByName("ns3::TcpRledbat"))
        {
            m_socket->SetAttribute("TargetDelay", TimeValue(m_targetDelay));
        }

        if (m_ipv6)
        {
            int brc = m_socket->Bind(Inet6SocketAddress(Ipv6Address::GetAny(), m_port));
            NS_ABORT_MSG_IF(brc != 0, "CustomSink IPv6 bind failed");
        }
        else
        {
            int brc = m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
            NS_ABORT_MSG_IF(brc != 0, "CustomSink IPv4 bind failed");
        }

        int lrc = m_socket->Listen();
        NS_ABORT_MSG_IF(lrc != 0, "CustomSink listen failed");
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

    bool
    HandleAcceptRequest(Ptr<Socket>, const Address&)
    {
        return m_acceptAll;
    }

    void
    HandlePeerClose(Ptr<Socket>)
    {
    }

    void
    HandlePeerError(Ptr<Socket>)
    {
    }

    void
    StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
        for (auto& s : m_connectedSockets)
        {
            s->Close();
        }
        m_connectedSockets.clear();
    }

    void
    HandleAccept(Ptr<Socket> socket, const Address&)
    {
        ++m_acceptCount;
        socket->SetRecvCallback(MakeCallback(&CustomSink::HandleRead, this));
        m_connectedSockets.push_back(socket);

        // rLEDBAT traces are attached only for update sockets that actually
        // accept as TcpRledbat. If this fails, .dat files remain header-only.
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
                        "BaseDelay", MakeBoundCallback(&TraceDelay, m_bFile));
                }
                else
                {
                    std::cerr << "[WARN] WLedbat trace attach failed on accepted socket at t="
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
        {
            if (pkt->GetSize() > 0)
            {
                m_totalRx += pkt->GetSize();
            }
        }
    }

    TypeId m_socketClassId;
    uint16_t m_port;
    Time m_targetDelay;
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
        : m_socket(nullptr), m_connected(false), m_connecting(false), m_running(false), m_connectWatchdogSeconds(2.0)
    {
    }

    void
    Setup(Address peer,
          uint32_t chunkSize = 1448,
          uint32_t pps = 300,
          TypeId socketClassId = TcpSocketBase::GetTypeId(),
          bool forceSocketClass = false,
          bool connectWatchdog = false,
          double connectWatchdogSeconds = 10.0,   // ← add this
          std::ofstream* wFile = nullptr,
          std::ofstream* qFile = nullptr,
          std::ofstream* bFile = nullptr)
    {
        m_peer = peer;
        m_chunkSize = chunkSize;
        m_pps = std::max(1u, pps);
        m_socketClassId = socketClassId;
        m_forceSocketClass = forceSocketClass;
        m_connectWatchdogSeconds = connectWatchdogSeconds;   // ← add this
        m_connectWatchdog = connectWatchdog;
        m_wFile = wFile;
        m_qFile = qFile;
        m_bFile = bFile;
    }

    uint32_t GetConnectAttempts() const { return m_connectAttempts; }
    uint32_t GetConnectImmediateFail() const { return m_connectImmediateFail; }
    uint32_t GetConnectSuccess() const { return m_connectSuccess; }
    uint32_t GetConnectFailCallback() const { return m_connectFailCallback; }
    uint32_t GetConnectTimeouts() const { return m_connectTimeouts; }
    uint32_t GetSendSuccess() const { return m_sendSuccess; }
    uint32_t GetSendBlocked() const { return m_sendBlocked; }
    uint32_t GetConnectionSucceededTrace() const { return m_connectionSucceededTrace; }
    uint32_t GetStateSynSent() const { return m_stateSynSent; }
    uint32_t GetStateEstablished() const { return m_stateEstablished; }

  private:
    // Create one fresh TCP socket and register callbacks.
    double m_connectWatchdogSeconds;
    void
    SetupSocket()
    {
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        NS_ABORT_MSG_IF(!tcp, "RetryBulkSender node missing TcpL4Protocol");
        TypeIdValue prevSocketClass(TcpSocketBase::GetTypeId());
        if (m_forceSocketClass)
        {
            tcp->GetAttribute("SocketClass", prevSocketClass);
            tcp->SetAttribute("SocketClass", TypeIdValue(m_socketClassId));
        }
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        if (m_forceSocketClass)
        {
            tcp->SetAttribute("SocketClass", prevSocketClass);
        }
        m_socket->SetConnectCallback(
            MakeCallback(&RetryBulkSender::OnConnected, this),
            MakeCallback(&RetryBulkSender::OnConnectFailed, this));
        m_socket->SetSendCallback(
            MakeCallback(&RetryBulkSender::OnSendPossible, this));
        m_socket->TraceConnectWithoutContext(
            "ConnectionSucceeded",
            MakeCallback(&RetryBulkSender::OnConnectionSucceededTrace, this));
        m_socket->TraceConnectWithoutContext(
            "State",
            MakeCallback(&RetryBulkSender::OnStateTrace, this));
    }

    void OnConnectionSucceededTrace(Ptr<Socket>) { ++m_connectionSucceededTrace; }
    void OnStateTrace(TcpSocket::TcpStates_t, TcpSocket::TcpStates_t newState)
    {
        if (newState == TcpSocket::SYN_SENT)
        {
            ++m_stateSynSent;
        }
        else if (newState == TcpSocket::ESTABLISHED)
        {
            ++m_stateEstablished;
        }
    }

    void
    StartApplication() override
    {
        m_running = true;
        SetupSocket();
        TryConnect();
    }

    void
    StopApplication() override
    {
        m_running = false;
        if (m_retryEvent.IsRunning())
        {
            Simulator::Cancel(m_retryEvent);
        }
        if (m_connectTimeoutEvent.IsRunning())
        {
            Simulator::Cancel(m_connectTimeoutEvent);
        }
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
        if (m_sendEvent.IsRunning())
        {
            Simulator::Cancel(m_sendEvent);
        }
    }

    void
    TryConnect()
    {
        if (!m_running || !m_socket || m_connected || m_connecting)
        {
            return;
        }

        ++m_connectAttempts;
        int rc = m_socket->Connect(m_peer);
        if (rc != 0)
        {
            ++m_connectImmediateFail;
            if (!m_retryEvent.IsRunning())
            {
                m_retryEvent = Simulator::Schedule(Seconds(1.0),
                                                   &RetryBulkSender::TryConnect,
                                                   this);
            }
        }
        else
        {
            m_connecting = true;
            if (m_connectWatchdog)
            {
                if (m_connectTimeoutEvent.IsRunning())
                {
                    Simulator::Cancel(m_connectTimeoutEvent);
                }
                m_connectTimeoutEvent = Simulator::Schedule(
                    Seconds(m_connectWatchdogSeconds),  // ← use the configured value
                    &RetryBulkSender::OnConnectTimeout, this);
            }
        }
    }

    void
    OnConnected(Ptr<Socket>)
    {
        ++m_connectSuccess;
        m_connected = true;
        m_connecting = false;
        if (m_wFile != nullptr &&
            m_socketClassId == TypeId::LookupByName("ns3::TcpRledbat"))
        {
            Ptr<TcpSocketBase> tcpSock = DynamicCast<TcpSocketBase>(m_socket);
            if (tcpSock)
            {
                bool ok = tcpSock->TraceConnectWithoutContext(
                    "WLedbat", MakeBoundCallback(&TraceWLedbat, m_wFile));
                if (ok)
                {
                    tcpSock->TraceConnectWithoutContext(
                        "QueuingDelay", MakeBoundCallback(&TraceDelay, m_qFile));
                    tcpSock->TraceConnectWithoutContext(
                        "BaseDelay", MakeBoundCallback(&TraceDelay, m_bFile));
                }
                else
                {
                    // Keep silent in normal runs; this is diagnostic-only.
                }
            }
        }
        if (m_retryEvent.IsRunning())
        {
            Simulator::Cancel(m_retryEvent);
        }
        if (m_connectTimeoutEvent.IsRunning())
        {
            Simulator::Cancel(m_connectTimeoutEvent);
        }
        ScheduleSend();
    }

    void
    OnConnectFailed(Ptr<Socket>)
    {
        ++m_connectFailCallback;
        if (!m_running)
        {
            return;
        }
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
        // In ns-3, failed TCP sockets are often not reusable for reconnect.
        // Recreate the socket before scheduling the next connect attempt.
        SetupSocket();
        m_connected = false;
        m_connecting = false;
        if (m_connectTimeoutEvent.IsRunning())
        {
            Simulator::Cancel(m_connectTimeoutEvent);
        }
        if (!m_retryEvent.IsRunning())
        {
            m_retryEvent = Simulator::Schedule(Seconds(1.0),
                                               &RetryBulkSender::TryConnect,
                                               this);
        }
    }

    void
    OnConnectTimeout()
    {
        if (!m_running || !m_connecting || m_connected)
        {
            return;
        }
        ++m_connectTimeouts;
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
        SetupSocket();
        m_connected = false;
        m_connecting = false;
        if (!m_retryEvent.IsRunning())
        {
            m_retryEvent = Simulator::Schedule(MilliSeconds(100), &RetryBulkSender::TryConnect, this);
        }
    }

    void
    OnSendPossible(Ptr<Socket>, uint32_t)
    {
        if (m_connected && !m_sendEvent.IsRunning())
        {
            ScheduleSend();
        }
    }

    void
    SendOnce()
    {
        if (!m_running || !m_connected || !m_socket)
        {
            return;
        }

        Ptr<Packet> pkt = Create<Packet>(m_chunkSize);
        int sent = m_socket->Send(pkt);
        if (sent > 0)
        {
            ++m_sendSuccess;
            ScheduleSend();
        }
        else
        {
            ++m_sendBlocked;
            m_sendEvent = Simulator::Schedule(MilliSeconds(5), &RetryBulkSender::SendOnce, this);
        }
    }

    void
    ScheduleSend()
    {
        if (!m_running || !m_connected || !m_socket || m_sendEvent.IsRunning())
        {
            return;
        }
        double interval = 1.0 / static_cast<double>(m_pps);
        m_sendEvent = Simulator::Schedule(Seconds(interval), &RetryBulkSender::SendOnce, this);
    }

    Address m_peer;
    Ptr<Socket> m_socket;
    EventId m_retryEvent;
    EventId m_sendEvent;
    EventId m_connectTimeoutEvent;
    uint32_t m_chunkSize{1448};
    uint32_t m_pps{300};
    TypeId m_socketClassId{TcpSocketBase::GetTypeId()};
    bool m_forceSocketClass{false};
    bool m_connectWatchdog{false};
    std::ofstream* m_wFile{nullptr};
    std::ofstream* m_qFile{nullptr};
    std::ofstream* m_bFile{nullptr};
    bool m_connected;
    bool m_connecting;
    bool m_running;
    uint32_t m_connectAttempts{0};
    uint32_t m_connectImmediateFail{0};
    uint32_t m_connectSuccess{0};
    uint32_t m_connectFailCallback{0};
    uint32_t m_connectTimeouts{0};
    uint32_t m_sendSuccess{0};
    uint32_t m_sendBlocked{0};
    uint32_t m_connectionSucceededTrace{0};
    uint32_t m_stateSynSent{0};
    uint32_t m_stateEstablished{0};
};

NS_OBJECT_ENSURE_REGISTERED(RetryBulkSender);

struct ScenarioConfig
{
    // Active domains
    bool activeWifi{true};
    bool activeLrwpan{true};
    bool activeWired{true};

    // Swept parameters
    uint32_t numNodes{60};
    uint32_t numFlows{30};
    uint32_t pps{300};
    double speedMps{15.0};         // WiFi mobility only
    double coverageMultiplier{3.0}; // LR-WPAN static only (1..5)

    // Timing
    double simTimeSec{60.0};
    double warmupSec{10.0};

    // RF reference
    double txRangeM{50.0};

    // rLEDBAT base params (fixed during these sweeps)
    double targetDelayMs{25.0};
    uint32_t baseDelayHistSize{10};
    double baseDelayRefreshSec{30.0};

    // rLEDBAT modifications (enabled in modified pass)
    bool enableMods{false};
    bool enableCadf{false};
    bool enableEcs{false};
    bool enableAtd{false};
    bool enableEcn{false};

    uint32_t cadfStreakThreshold{4};
    double cadfSpikeRatio{2.5};
    double cadfMinAbsoluteSpike{5.0};
    uint32_t cadfWindowSize{8};
    double ecsBeta{0.5};
    double atdAlpha{0.5};
    double atdMinTargetMs{5.0};

    bool enableTraceDat{true};
    std::string tag{"default"};
};

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
    // Coverage multiplier only affects LR-WPAN topology, not WiFi
    if (cfg.activeLrwpan)
    {
        ofs << " coverage=" << cfg.coverageMultiplier << "xTxRange";
    }
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
    {
        ofs << "    " << std::left << std::setw(42) << lbl
            << " : " << std::right << std::setprecision(4) << tp << " Mbps\n";
    }

    ofs << "  [Per-Flow Metrics]\n";
    for (const auto& f : m.perFlow)
    {
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
    }

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

struct SenderTarget
{
    Ptr<Node> node;
    Address updatePeer;
    Address interactivePeer;
    std::string domain;
};

static std::string
ExtractXmlAttr(const std::string& line, const std::string& key)
{
    std::string needle = key + "=\"";
    std::size_t p = line.find(needle);
    if (p == std::string::npos)
    {
        return "";
    }
    p += needle.size();
    std::size_t q = line.find('"', p);
    if (q == std::string::npos)
    {
        return "";
    }
    return line.substr(p, q - p);
}

static void
BuildFlowPortMap(Ptr<FlowClassifier> classifier,
                 std::map<uint32_t, std::pair<uint16_t, std::string>>& out)
{
    if (!classifier)
    {
        return;
    }

    std::ostringstream oss;
    classifier->SerializeToXmlStream(oss, 0);
    std::istringstream iss(oss.str());
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.find("<Flow ") == std::string::npos)
        {
            continue;
        }

        std::string sid = ExtractXmlAttr(line, "flowId");
        std::string src = ExtractXmlAttr(line, "sourceAddress");
        std::string dport = ExtractXmlAttr(line, "destinationPort");
        if (sid.empty() || dport.empty())
        {
            continue;
        }

        uint32_t fid = static_cast<uint32_t>(std::stoul(sid));
        uint16_t port = static_cast<uint16_t>(std::stoul(dport));
        out[fid] = {port, src};
    }
}

static RunMetrics
RunScenario(const ScenarioConfig& cfg)
{
    RunMetrics metrics;

    const uint16_t UPDATE_PORT = 9000;
    const uint16_t INTERACTIVE_PORT = 9001;

    Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(true));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpNewReno"));

    Config::SetDefault("ns3::TcpRledbat::TargetDelay",
                       TimeValue(MilliSeconds(cfg.targetDelayMs)));
    Config::SetDefault("ns3::TcpRledbat::BaseDelayHistorySize",
                       UintegerValue(cfg.baseDelayHistSize));
    Config::SetDefault("ns3::TcpRledbat::BaseDelayRefreshInterval",
                       TimeValue(Seconds(cfg.baseDelayRefreshSec)));

    Config::SetDefault("ns3::TcpRledbat::EnableCadf", BooleanValue(cfg.enableCadf));
    Config::SetDefault("ns3::TcpRledbat::EnableEcs", BooleanValue(cfg.enableEcs));
    Config::SetDefault("ns3::TcpRledbat::EnableAtd", BooleanValue(cfg.enableAtd));

    Config::SetDefault("ns3::TcpRledbat::CadfStreakThreshold",
                       UintegerValue(cfg.cadfStreakThreshold));
    Config::SetDefault("ns3::TcpRledbat::CadfSpikeRatio",
                       DoubleValue(cfg.cadfSpikeRatio));
    Config::SetDefault("ns3::TcpRledbat::CadfMinAbsoluteSpike",
                       DoubleValue(cfg.cadfMinAbsoluteSpike));
    Config::SetDefault("ns3::TcpRledbat::CadfWindowSize",
                       UintegerValue(cfg.cadfWindowSize));

    Config::SetDefault("ns3::TcpRledbat::EcsBeta", DoubleValue(cfg.ecsBeta));
    Config::SetDefault("ns3::TcpRledbat::AtdAlpha", DoubleValue(cfg.atdAlpha));
    Config::SetDefault("ns3::TcpRledbat::AtdMinTarget",
                       TimeValue(MilliSeconds(cfg.atdMinTargetMs)));

    Config::SetDefault("ns3::TcpSocketBase::UseEcn",
                       StringValue(cfg.enableEcn ? "On" : "Off"));

    // Exactly one active domain is supported per run.
    bool wifiMode = cfg.activeWifi && !cfg.activeLrwpan;
    bool lrwpanMode = cfg.activeLrwpan && !cfg.activeWifi;
    NS_ABORT_MSG_IF(!(wifiMode || lrwpanMode),
                    "Run either WiFi-only or LR-WPAN-only mode (not both)");

    // In 6LoWPAN/LR-WPAN mode, TCP timestamps add 10 bytes to every TCP header.
    // With 802.15.4's 127-byte frame limit, a data segment of 80B + 30B TCP
    // (with timestamps) + 40B IPv6 already exceeds the frame size, forcing
    // 6LoWPAN fragmentation on EVERY data packet.  Under any non-trivial load
    // reassembly failures multiply, causing zero effective throughput.
    // Keep timestamps for WiFi; disable for LR-WPAN.
    Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(!lrwpanMode));

    // Keep Stage1 behavior where pps controls offered load aggressiveness.
    uint32_t tcpSeg = lrwpanMode ? 80u : 1448u;
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(tcpSeg));
    uint32_t bufSize = cfg.pps * tcpSeg;
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(bufSize));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(bufSize));

    NodeContainer nodes;
    nodes.Create(cfg.numNodes);

    Ptr<Node> receiver = nodes.Get(0);
    uint32_t cursor = 1;
    int32_t gwWifiTxId = -1;
    int32_t coreRouterId = -1;
    int32_t coreWifiId = -1;
    std::vector<uint32_t> wifiTxIds, wifiRxRelayIds, lrIds;

    if (wifiMode)
    {
        // WiFi mode layout:
        // sender WiFi net + p2p backbone + receiver WiFi net.
        // node0=receiver, node1=GW_wifi_tx, node2=CORE, node3=CORE_wifi
        uint32_t infra = 4;
        NS_ABORT_MSG_IF(cfg.numNodes <= infra, "numNodes too small for WiFi infra + senders/relays");
        gwWifiTxId = static_cast<int32_t>(cursor++);
        coreRouterId = static_cast<int32_t>(cursor++);
        coreWifiId = static_cast<int32_t>(cursor++);
        uint32_t rem = cfg.numNodes - infra;
        uint32_t txN = rem / 2;
        uint32_t rxN = rem - txN;
        for (uint32_t i = 0; i < txN; ++i) wifiTxIds.push_back(cursor++);
        for (uint32_t i = 0; i < rxN; ++i) wifiRxRelayIds.push_back(cursor++);
    }
    else
    {
        // LR-WPAN mode: coordinator(receiver) + all other static star nodes
        NS_ABORT_MSG_IF(cfg.numNodes < 2, "numNodes must be >=2 for LR-WPAN");
        for (uint32_t i = 1; i < cfg.numNodes; ++i) lrIds.push_back(i);
    }

    Ipv4NixVectorHelper nixRouting;
    InternetStackHelper internet;
    if (wifiMode)
    {
        internet.SetRoutingHelper(nixRouting);
    }
    else
    {
        // Match the known-good 6LoWPAN IPv6-only setup for LR-WPAN.
        internet.SetIpv4StackInstall(false);
    }
    internet.Install(nodes);

    BasicEnergySourceHelper energySourceHelper;
    energySourceHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(100.0));
    EnergySourceContainer energySources = energySourceHelper.Install(nodes);

    // Default: static position for all nodes.
    MobilityHelper staticMob;
    staticMob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    staticMob.Install(nodes);

    Ptr<ConstantPositionMobilityModel> rxPos = receiver->GetObject<ConstantPositionMobilityModel>();
    if (rxPos)
    {
        rxPos->SetPosition(Vector(0.0, 0.0, 0.0));
    }

    Ipv4Address recvWifiAddr("0.0.0.0");
    Ipv6Address recvLrAddr = Ipv6Address::GetAny();

    std::vector<SenderTarget> senderPool;

    // WiFi mode: two WiFi domains explicitly bridged by p2p backbone.
        if (wifiMode)
        {
            // Anchor sender-side domain center near GW_wifi_tx and receiver-side
            // domain center near receiver/coreWifi to avoid partitioning.
            Ptr<ConstantPositionMobilityModel> gwTxPos =
                nodes.Get(static_cast<uint32_t>(gwWifiTxId))->GetObject<ConstantPositionMobilityModel>();
            if (gwTxPos)
            {
                gwTxPos->SetPosition(Vector(0.0, 0.0, 0.0));
            }
            Ptr<ConstantPositionMobilityModel> corePos =
                nodes.Get(static_cast<uint32_t>(coreRouterId))->GetObject<ConstantPositionMobilityModel>();
            if (corePos)
            {
                corePos->SetPosition(Vector(50.0, 0.0, 0.0));
            }
            Ptr<ConstantPositionMobilityModel> coreWifiPos =
                nodes.Get(static_cast<uint32_t>(coreWifiId))->GetObject<ConstantPositionMobilityModel>();
            if (coreWifiPos)
            {
                coreWifiPos->SetPosition(Vector(0.0, 0.0, 0.0));
            }

            NodeContainer wifiTxNodes;
        wifiTxNodes.Add(nodes.Get(static_cast<uint32_t>(gwWifiTxId)));
        for (uint32_t id : wifiTxIds)
        {
            wifiTxNodes.Add(nodes.Get(id));
        }
        NodeContainer wifiRxNodes;
        wifiRxNodes.Add(receiver);
        wifiRxNodes.Add(nodes.Get(static_cast<uint32_t>(coreWifiId)));
        for (uint32_t id : wifiRxRelayIds) wifiRxNodes.Add(nodes.Get(id));

        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211b);
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode", StringValue("DsssRate11Mbps"),
                                     "ControlMode", StringValue("DsssRate1Mbps"));

        // Each Create() call returns a NEW channel object.
        // This intentionally isolates sender-side and receiver-side WiFi media.
        // Build a shared channel helper; each call to Create() produces a
        // separate, independent YansWifiChannel object so the two WiFi
        // networks cannot hear each other directly.
        YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
        channelHelper.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
        channelHelper.AddPropagationLoss("ns3::RangePropagationLossModel",
                                         "MaxRange", DoubleValue(4.0 * cfg.txRangeM));

        WifiMacHelper mac;
        mac.SetType("ns3::AdhocWifiMac");

        // --- Sender-side WiFi (GW_wifi_tx + wifiTxNodes) ---
        // channelHelper.Create() returns a NEW channel object each time it is
        // called, giving this side its own isolated medium.
        YansWifiPhyHelper phyTx;
        phyTx.SetChannel(channelHelper.Create());
        phyTx.Set("TxPowerStart", DoubleValue(20.0));
        phyTx.Set("TxPowerEnd", DoubleValue(20.0));
        NetDeviceContainer wifiTxDevs = wifi.Install(phyTx, mac, wifiTxNodes);

        // --- Receiver-side WiFi (receiver + coreWifi + relay nodes) ---
        // A second call to Create() produces a completely separate channel.
        YansWifiPhyHelper phyRx;
        phyRx.SetChannel(channelHelper.Create());
        phyRx.Set("TxPowerStart", DoubleValue(20.0));
        phyRx.Set("TxPowerEnd", DoubleValue(20.0));
        NetDeviceContainer wifiRxDevs = wifi.Install(phyRx, mac, wifiRxNodes);

        // --- Energy models ---
        // WifiRadioEnergyModelHelper::Install(devs, sources) requires that
        // devs[i] and sources[i] belong to the SAME node (assert in ns-3).
        // Build two separate source containers so the indices always match.
        WifiRadioEnergyModelHelper radioEnergyHelper;

        EnergySourceContainer txEnergySources;
        for (uint32_t i = 0; i < wifiTxNodes.GetN(); ++i)
        {
            txEnergySources.Add(energySources.Get(wifiTxNodes.Get(i)->GetId()));
        }
        radioEnergyHelper.Install(wifiTxDevs, txEnergySources);

        EnergySourceContainer rxEnergySources;
        for (uint32_t i = 0; i < wifiRxNodes.GetN(); ++i)
        {
            rxEnergySources.Add(energySources.Get(wifiRxNodes.Get(i)->GetId()));
        }
        radioEnergyHelper.Install(wifiRxDevs, rxEnergySources);

        // Backbone p2p links force cross-domain forwarding path in WiFi mode:
        // GW_wifi_tx <-> CORE <-> CORE_wifi.
        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
        p2p.SetChannelAttribute("Delay", StringValue("2ms"));
        NetDeviceContainer d1 = p2p.Install(nodes.Get(static_cast<uint32_t>(gwWifiTxId)),
                                            nodes.Get(static_cast<uint32_t>(coreRouterId)));
        NetDeviceContainer d2 = p2p.Install(nodes.Get(static_cast<uint32_t>(coreRouterId)),
                                            nodes.Get(static_cast<uint32_t>(coreWifiId)));

        Ipv4AddressHelper ipv4;
        ipv4.SetBase("10.1.0.0", "255.255.255.0");
        Ipv4InterfaceContainer txIf = ipv4.Assign(wifiTxDevs);
        ipv4.SetBase("10.1.1.0", "255.255.255.0");
        Ipv4InterfaceContainer rxIf = ipv4.Assign(wifiRxDevs);
        ipv4.SetBase("10.10.0.0", "255.255.255.252");
        Ipv4InterfaceContainer p1 = ipv4.Assign(d1);
        ipv4.SetBase("10.10.0.4", "255.255.255.252");
        Ipv4InterfaceContainer p2 = ipv4.Assign(d2);
        (void)txIf; (void)p1; (void)p2;
        recvWifiAddr = rxIf.GetAddress(0);

        // Mobility model:
        // Keep movers in bounded regions around their domain anchor so all
        // sender-side nodes stay reachable to GW_wifi_tx and all receiver-side
        // relays stay reachable to receiver/coreWifi.
        double moverHalfSpan = 0.3 * cfg.txRangeM;
        std::string txMinX = std::to_string(-moverHalfSpan);
        std::string txMaxX = std::to_string(moverHalfSpan);
        std::string txMinY = std::to_string(-moverHalfSpan);
        std::string txMaxY = std::to_string(moverHalfSpan);
        std::string rxMinX = std::to_string(-moverHalfSpan);
        std::string rxMaxX = std::to_string(moverHalfSpan);
        std::string rxMinY = std::to_string(-moverHalfSpan);
        std::string rxMaxY = std::to_string(moverHalfSpan);
        std::string speedStr = std::to_string(cfg.speedMps);

        ObjectFactory txPosFac;
        txPosFac.SetTypeId("ns3::RandomRectanglePositionAllocator");
        txPosFac.Set("X", StringValue("ns3::UniformRandomVariable[Min=" + txMinX + "|Max=" + txMaxX + "]"));
        txPosFac.Set("Y", StringValue("ns3::UniformRandomVariable[Min=" + txMinY + "|Max=" + txMaxY + "]"));
        Ptr<PositionAllocator> txWaypointAlloc =
            txPosFac.Create()->GetObject<PositionAllocator>();

        ObjectFactory rxPosFac;
        rxPosFac.SetTypeId("ns3::RandomRectanglePositionAllocator");
        rxPosFac.Set("X", StringValue("ns3::UniformRandomVariable[Min=" + rxMinX + "|Max=" + rxMaxX + "]"));
        rxPosFac.Set("Y", StringValue("ns3::UniformRandomVariable[Min=" + rxMinY + "|Max=" + rxMaxY + "]"));
        Ptr<PositionAllocator> rxWaypointAlloc =
            rxPosFac.Create()->GetObject<PositionAllocator>();

        MobilityHelper txMob;
        txMob.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                   "X", StringValue("ns3::UniformRandomVariable[Min=" + txMinX + "|Max=" + txMaxX + "]"),
                                   "Y", StringValue("ns3::UniformRandomVariable[Min=" + txMinY + "|Max=" + txMaxY + "]"));
        txMob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                               "Speed", StringValue("ns3::ConstantRandomVariable[Constant=" + speedStr + "]"),
                               "Pause", StringValue("ns3::ConstantRandomVariable[Constant=0.5]"),
                               "PositionAllocator", PointerValue(txWaypointAlloc));

        MobilityHelper rxMob;
        rxMob.SetPositionAllocator("ns3::RandomRectanglePositionAllocator",
                                   "X", StringValue("ns3::UniformRandomVariable[Min=" + rxMinX + "|Max=" + rxMaxX + "]"),
                                   "Y", StringValue("ns3::UniformRandomVariable[Min=" + rxMinY + "|Max=" + rxMaxY + "]"));
        rxMob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                               "Speed", StringValue("ns3::ConstantRandomVariable[Constant=" + speedStr + "]"),
                               "Pause", StringValue("ns3::ConstantRandomVariable[Constant=0.5]"),
                               "PositionAllocator", PointerValue(rxWaypointAlloc));

        NodeContainer txMovers;
        NodeContainer rxMovers;
        for (uint32_t id : wifiTxIds)
        {
            txMovers.Add(nodes.Get(id));
        }
        for (uint32_t id : wifiRxRelayIds)
        {
            rxMovers.Add(nodes.Get(id));
        }
        if (txMovers.GetN() > 0)
        {
            txMob.Install(txMovers);
        }
        if (rxMovers.GetN() > 0)
        {
            rxMob.Install(rxMovers);
        }

        for (uint32_t id : wifiTxIds)
        {
            SenderTarget st;
            st.node = nodes.Get(id);
            st.updatePeer = InetSocketAddress(recvWifiAddr, UPDATE_PORT);
            st.interactivePeer = InetSocketAddress(recvWifiAddr, INTERACTIVE_PORT);
            st.domain = "wifi";
            senderPool.push_back(st);
        }
    }

    bool hasLrwpan = false;

    // LR-WPAN mode: pure star with receiver as PAN coordinator.
    // No WiFi or wired backbone is involved in this branch.
    if (lrwpanMode)
    {
        hasLrwpan = true;
        NodeContainer lrNodes;
        lrNodes.Add(receiver);
        for (uint32_t id : lrIds)
        {
            lrNodes.Add(nodes.Get(id));
        }

        LrWpanHelper lr;
        NetDeviceContainer lrDevs = lr.Install(lrNodes);
        // PAN ID 0 is valid per 802.15.4 spec but some ns-3 paths treat it as
        // "unassociated"; use the same non-zero ID as LrwpanTcpSanity.cc.
        lr.CreateAssociatedPan(lrDevs, 10);

        SixLowPanHelper six;
        NetDeviceContainer sixDevs = six.Install(lrDevs);

        Ipv6AddressHelper ipv6;
        ipv6.SetBase(Ipv6Address("2001:1::"), Ipv6Prefix(64));
        Ipv6InterfaceContainer if6 = ipv6.Assign(sixDevs);
        // Pure 6LoWPAN single-L2 star: no routed next-hop needed.
        // Setting all nodes' default route to node0 can distort neighbor
        // discovery/forwarding behavior in this flat topology.
        recvLrAddr = if6.GetAddress(0, 1);
        if (recvLrAddr.IsLinkLocal())
        {
            recvLrAddr = if6.GetAddress(0, 0);
        }

        // Mirror the known-good sanity topology: deterministic star ring.
        // This avoids random geometry/pathological hidden-terminal layouts
        // during debugging and keeps LR-WPAN TCP handshake behavior stable.
        double clusterRadius = 6.0;
        const double twoPi = 6.283185307179586;
        uint32_t idx = 0;
        for (uint32_t id : lrIds)
        {
            Ptr<ConstantPositionMobilityModel> mm =
                nodes.Get(id)->GetObject<ConstantPositionMobilityModel>();
            if (mm)
            {
                double theta = twoPi * (static_cast<double>(idx) / std::max<uint32_t>(1, lrIds.size()));
                double r = clusterRadius;
                mm->SetPosition(Vector(r * std::cos(theta), r * std::sin(theta), 0.0));
            }
            idx++;

            SenderTarget st;
            st.node = nodes.Get(id);
            st.updatePeer = Inet6SocketAddress(recvLrAddr, UPDATE_PORT);
            st.interactivePeer = Inet6SocketAddress(recvLrAddr, INTERACTIVE_PORT);
            st.domain = "lrwpan";
            senderPool.push_back(st);
        }
    }

    NS_ABORT_MSG_IF(senderPool.empty(), "No sender nodes available");

    // Nix-vector routing computes compact paths without explicit global-table build.

    // Sinks are co-located on receiver node0:
    // - WiFi mode uses IPv4 peers/sinks
    // - LR-WPAN mode uses IPv6 peers/sinks
    std::ofstream wFile, qFile, bFile;
    if (cfg.enableTraceDat)
    {
        wFile.open("scratch/final-wledbat-" + cfg.tag + ".dat");
        qFile.open("scratch/final-qdelay-" + cfg.tag + ".dat");
        bFile.open("scratch/final-basedelay-" + cfg.tag + ".dat");
        wFile << "# t(s) W_ledbat(bytes)\n";
        qFile << "# t(s) QueuingDelay(ms)\n";
        bFile << "# t(s) BaseDelay(ms)\n";
    }

    std::vector<Ptr<CustomSink>> sinks;

    if (wifiMode)
    {
        Ptr<CustomSink> updateV4 = CreateObject<CustomSink>();
        updateV4->Setup(TypeId::LookupByName("ns3::TcpRledbat"),
                        UPDATE_PORT,
                        MilliSeconds(cfg.targetDelayMs),
                        cfg.enableEcn,
                        false);
        if (cfg.enableTraceDat)
        {
            updateV4->SetTraceFiles(&wFile, &qFile, &bFile);
        }
        receiver->AddApplication(updateV4);
        updateV4->SetStartTime(Seconds(0.0));
        updateV4->SetStopTime(Seconds(cfg.simTimeSec));
        sinks.push_back(updateV4);

        Ptr<CustomSink> interactiveV4 = CreateObject<CustomSink>();
        interactiveV4->Setup(TcpSocketBase::GetTypeId(), INTERACTIVE_PORT, Time(0), false, false);
        receiver->AddApplication(interactiveV4);
        interactiveV4->SetStartTime(Seconds(0.05));
        interactiveV4->SetStopTime(Seconds(cfg.simTimeSec));
        sinks.push_back(interactiveV4);
    }

    if (lrwpanMode)
    {
        // Use CustomSink (not PacketSink) so that SocketClass is set inside
        // StartApplication() at socket-creation time.  Setting SocketClass
        // around PacketSinkHelper::Install() has no effect because the actual
        // socket is not created until the application starts.
        Ptr<CustomSink> lrUpdateSink = CreateObject<CustomSink>();
        lrUpdateSink->Setup(TypeId::LookupByName("ns3::TcpRledbat"),
                            UPDATE_PORT,
                            MilliSeconds(cfg.targetDelayMs),
                            cfg.enableEcn,
                            true /*ipv6*/);
        if (cfg.enableTraceDat)
        {
            lrUpdateSink->SetTraceFiles(&wFile, &qFile, &bFile);
        }
        receiver->AddApplication(lrUpdateSink);
        lrUpdateSink->SetStartTime(Seconds(0.5));
        lrUpdateSink->SetStopTime(Seconds(cfg.simTimeSec));
        sinks.push_back(lrUpdateSink);

        Ptr<CustomSink> lrInteractiveSink = CreateObject<CustomSink>();
        lrInteractiveSink->Setup(TcpSocketBase::GetTypeId(),
                                 INTERACTIVE_PORT,
                                 Time(0),
                                 false,
                                 true /*ipv6*/);
        receiver->AddApplication(lrInteractiveSink);
        lrInteractiveSink->SetStartTime(Seconds(0.55));
        lrInteractiveSink->SetStopTime(Seconds(cfg.simTimeSec));
        sinks.push_back(lrInteractiveSink);
    }

    // Flow creation policy:
    // - fixed numFlows from scenario
    // - first half update (port 9000), second half interactive (port 9001)
    // - sender nodes selected round-robin from senderPool
    uint32_t numUpdate = cfg.numFlows / 2;
    std::vector<Ptr<RetryBulkSender>> senderApps;
    for (uint32_t f = 0; f < cfg.numFlows; ++f)
    {
        const SenderTarget& st = senderPool[f % senderPool.size()];
        bool isUpdate = (f < numUpdate);

        bool isLr = (hasLrwpan && st.domain == "lrwpan");

        // Use smaller TCP chunks on LR-WPAN senders to avoid over-large bursts
        // on low-rate 802.15.4 links.
        uint32_t chunkSize = isLr ? 80u : 1448u;

        // LR-WPAN start-time: use the same tight stagger as the sanity test
        // (1.0 + 0.2*f).  The previous 1.5s-per-flow spacing pushed the last
        // flow to t=53.5 s in a 60 s simulation, leaving < 7 s for handshake
        // and data — too little with 802.15.4 retries.
        double start;
        if (isLr)
        {
            start = 1.0 + 0.2 * static_cast<double>(f);
        }
        else
        {
            start = cfg.warmupSec + 0.1 * (f % senderPool.size()) + 0.02 * f;
        }

        // LR-WPAN: cap offered rate to avoid saturating the 250 kbps 802.15.4
        // channel.  Unlimited BulkSend (pps=300, ~24 KB/s per flow) from just
        // two flows already exceeds channel capacity, making subsequent SYNs
        // impossible to deliver.  Use RetryBulkSender at 3 pps (240 B/s per
        // flow) so 30 flows contribute ~7.2 kbps — well inside channel budget.
        uint32_t effectivePps = isLr ? 3u : cfg.pps;

        // In LR-WPAN, let TCP's own SYN retransmission/backoff run uninterrupted.
        // An aggressive app-level watchdog causes repeated socket resets and can
        // keep every flow stuck in SYN_SENT under contention.
        bool connectWatchdog = false;
        double watchdogSec   = 10.0;

        Ptr<RetryBulkSender> app = CreateObject<RetryBulkSender>();
        // Sender transport stays regular TCP; prioritization is receiver-side by
        // sink socket class (port 9000=rLEDBAT accept class, 9001=normal TCP).
        TypeId sockClass = TcpSocketBase::GetTypeId();
        app->Setup(isUpdate ? st.updatePeer : st.interactivePeer,
                   chunkSize,
                   effectivePps,
                   sockClass,
                   false,
                   connectWatchdog,
                   watchdogSec,
                   nullptr, nullptr, nullptr);
        st.node->AddApplication(app);
        senderApps.push_back(app);
        app->SetStartTime(Seconds(start));
        app->SetStopTime(Seconds(cfg.simTimeSec));
    }

    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> flowMon = fmHelper.InstallAll();

    Simulator::Stop(Seconds(cfg.simTimeSec));
    Simulator::Run();

    flowMon->CheckForLostPackets();
    Ptr<FlowClassifier> c4 = fmHelper.GetClassifier();
    Ptr<FlowClassifier> c6 = fmHelper.GetClassifier6();
    const auto& stats = flowMon->GetFlowStats();

    if (lrwpanMode)
    {
        metrics.lrwpanSenderApps = cfg.numFlows;
        for (const auto& app : senderApps)
        {
            metrics.lrConnectAttempts += app->GetConnectAttempts();
            metrics.lrConnectImmediateFail += app->GetConnectImmediateFail();
            metrics.lrConnectSuccess += app->GetConnectSuccess();
            metrics.lrConnectFailCallback += app->GetConnectFailCallback();
            metrics.lrConnectTimeouts += app->GetConnectTimeouts();
            metrics.lrSendSuccess += app->GetSendSuccess();
            metrics.lrSendBlocked += app->GetSendBlocked();
            metrics.lrConnectionSucceededTrace += app->GetConnectionSucceededTrace();
            metrics.lrStateSynSent += app->GetStateSynSent();
            metrics.lrStateEstablished += app->GetStateEstablished();
        }
        if (sinks.size() >= 2)
        {
            metrics.lrUpdateAccepts = sinks[0]->GetAcceptCount();
            metrics.lrInteractiveAccepts = sinks[1]->GetAcceptCount();
            metrics.lrUpdateRxBytes = sinks[0]->GetTotalRx();
            metrics.lrInteractiveRxBytes = sinks[1]->GetTotalRx();
        }
    }

    std::map<uint32_t, std::pair<uint16_t, std::string>> flowInfo;
    BuildFlowPortMap(c4, flowInfo);
    BuildFlowPortMap(c6, flowInfo);

    double measDuration = std::max(1e-6, cfg.simTimeSec - cfg.warmupSec);

    double rRxBytes = 0, rTxPkts = 0, rRxPkts = 0, rDelaySum = 0, rDurSum = 0, rJitterSum = 0;
    double iRxBytes = 0, iTxPkts = 0, iRxPkts = 0, iDelaySum = 0, iDurSum = 0, iJitterSum = 0;

    std::map<std::string, std::pair<double, double>> senderUpdateBytesDur;
    std::map<std::string, std::pair<double, double>> senderInteractiveBytesDur;

    for (const auto& [fid, fs] : stats)

    {
        // Skip flows with no received data — these are failed handshakes
        // whose SYN/SYN-ACK packets FlowMonitor recorded at the IP layer.
        if (fs.rxBytes == 0 && fs.rxPackets == 0)
        {
            continue;
        }
        auto it = flowInfo.find(fid);
        if (it == flowInfo.end())
        {
            continue;
        }
        uint16_t dport = it->second.first;
        std::string srcLabel = it->second.second.empty() ? "unknown" : it->second.second;

        if (dport != UPDATE_PORT && dport != INTERACTIVE_PORT)
        {
            continue;
        }

        double txStart = std::max(cfg.warmupSec, fs.timeFirstTxPacket.GetSeconds());
        double rxEnd = fs.timeLastRxPacket.GetSeconds();
        double activeDur = std::max(0.0, rxEnd - txStart);
        if (fs.rxBytes > 0 && activeDur <= 0.0)
        {
            activeDur = measDuration;
        }
        double flowTp = (activeDur > 0.0)
                            ? (static_cast<double>(fs.rxBytes) * 8.0 / activeDur / 1e6)
                            : 0.0;
        double flowPdr = (fs.txPackets > 0)
                             ? (static_cast<double>(fs.rxPackets) / static_cast<double>(fs.txPackets))
                             : 0.0;
        double flowLost = (fs.txPackets >= fs.rxPackets)
                              ? static_cast<double>(fs.txPackets - fs.rxPackets)
                              : 0.0;
        double flowDrop = (fs.txPackets > 0)
                              ? (flowLost / static_cast<double>(fs.txPackets))
                              : 0.0;
        double flowDelay = (fs.rxPackets > 0)
                               ? (fs.delaySum.GetMilliSeconds() / static_cast<double>(fs.rxPackets))
                               : 0.0;
        // Jitter: average jitter per packet. jitterSum covers rxPackets - 1 samples.
        double flowJitter = (fs.rxPackets > 1)
                                ? (fs.jitterSum.GetMilliSeconds() / static_cast<double>(fs.rxPackets - 1))
                                : 0.0;

        RunMetrics::FlowMetrics rec;
        rec.flowId = fid;
        rec.src = srcLabel;
        rec.dport = dport;
        rec.klass = (dport == UPDATE_PORT) ? "rLEDBAT" : "Interactive";
        rec.activeDurSec = activeDur;
        rec.tputMbps = flowTp;
        rec.delayMs = flowDelay;
        rec.jitterMs = flowJitter;
        rec.pdr = flowPdr;
        rec.dropRatio = flowDrop;
        metrics.perFlow.push_back(rec);

        if (dport == UPDATE_PORT)
        {
            rRxBytes += static_cast<double>(fs.rxBytes);
            rTxPkts += static_cast<double>(fs.txPackets);
            rRxPkts += static_cast<double>(fs.rxPackets);
            rDelaySum += fs.delaySum.GetMilliSeconds();
            rJitterSum += fs.jitterSum.GetMilliSeconds();
            rDurSum += activeDur;

            senderUpdateBytesDur[srcLabel].first += static_cast<double>(fs.rxBytes);
            senderUpdateBytesDur[srcLabel].second += activeDur;
        }
        else
        {
            iRxBytes += static_cast<double>(fs.rxBytes);
            iTxPkts += static_cast<double>(fs.txPackets);
            iRxPkts += static_cast<double>(fs.rxPackets);
            iDelaySum += fs.delaySum.GetMilliSeconds();
            iJitterSum += fs.jitterSum.GetMilliSeconds();
            iDurSum += activeDur;

            senderInteractiveBytesDur[srcLabel].first += static_cast<double>(fs.rxBytes);
            senderInteractiveBytesDur[srcLabel].second += activeDur;
        }
    }

    metrics.rledbatTputMbps = (rDurSum > 0.0) ? (rRxBytes * 8.0 / measDuration / 1e6) : 0.0;
    metrics.interactiveTputMbps = (iDurSum > 0.0) ? (iRxBytes * 8.0 / measDuration / 1e6) : 0.0;
    metrics.totalTputMbps = metrics.rledbatTputMbps + metrics.interactiveTputMbps;
    metrics.yieldingRatio = (metrics.interactiveTputMbps > 0)
                                ? metrics.rledbatTputMbps / metrics.interactiveTputMbps
                                : 0.0;

    metrics.rledbatDelayMs = (rRxPkts > 0) ? rDelaySum / rRxPkts : 0.0;
    metrics.interactiveDelayMs = (iRxPkts > 0) ? iDelaySum / iRxPkts : 0.0;
    double totRxPkts = rRxPkts + iRxPkts;
    metrics.avgDelayMs = (totRxPkts > 0) ? (rDelaySum + iDelaySum) / totRxPkts : 0.0;

    // Jitter calculation: jitterSum covers (rxPackets - 1) samples per flow
    double rJitterSamples = (rRxPkts > 0) ? rRxPkts - 1.0 : 0.0;
    double iJitterSamples = (iRxPkts > 0) ? iRxPkts - 1.0 : 0.0;
    metrics.rledbatJitterMs = (rJitterSamples > 0) ? rJitterSum / rJitterSamples : 0.0;
    metrics.interactiveJitterMs = (iJitterSamples > 0) ? iJitterSum / iJitterSamples : 0.0;
    double totJitterSamples = rJitterSamples + iJitterSamples;
    metrics.avgJitterMs = (totJitterSamples > 0) ? (rJitterSum + iJitterSum) / totJitterSamples : 0.0;

    metrics.rledbatPDR = (rTxPkts > 0) ? rRxPkts / rTxPkts : 0.0;
    metrics.interactivePDR = (iTxPkts > 0) ? iRxPkts / iTxPkts : 0.0;
    double totTxPkts = rTxPkts + iTxPkts;
    metrics.totalPDR = (totTxPkts > 0) ? (rRxPkts + iRxPkts) / totTxPkts : 0.0;

    double rLost = (rTxPkts >= rRxPkts) ? (rTxPkts - rRxPkts) : 0.0;
    double iLost = (iTxPkts >= iRxPkts) ? (iTxPkts - iRxPkts) : 0.0;
    metrics.rledbatDropRatio = (rTxPkts > 0) ? rLost / rTxPkts : 0.0;
    metrics.interactiveDropRatio = (iTxPkts > 0) ? iLost / iTxPkts : 0.0;
    metrics.totalDropRatio = (totTxPkts > 0) ? (rLost + iLost) / totTxPkts : 0.0;

    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<BasicEnergySource> src = DynamicCast<BasicEnergySource>(energySources.Get(i));
        if (src)
        {
            metrics.totalEnergyJ += 100.0 - src->GetRemainingEnergy();
        }
    }
    metrics.avgEnergyPerNodeJ = metrics.totalEnergyJ / static_cast<double>(cfg.numNodes);

    for (const auto& [src, bd] : senderUpdateBytesDur)
    {
        double bytes = bd.first;
        double dur = bd.second;
        double tp = (dur > 0.0) ? (bytes * 8.0 / dur / 1e6) : 0.0;
        metrics.perSenderTput.push_back({"rLEDBAT src=" + src, tp});
    }
    for (const auto& [src, bd] : senderInteractiveBytesDur)
    {
        double bytes = bd.first;
        double dur = bd.second;
        double tp = (dur > 0.0) ? (bytes * 8.0 / dur / 1e6) : 0.0;
        metrics.perSenderTput.push_back({"Interactive src=" + src, tp});
    }

    double sumX = 0.0, sumX2 = 0.0;
    int n = 0;
    for (const auto& [lbl, tp] : metrics.perSenderTput)
    {
        (void)lbl;
        sumX += tp;
        sumX2 += tp * tp;
        n++;
    }
    metrics.jainFairness = (n > 0 && sumX2 > 0)
                               ? (sumX * sumX) / (static_cast<double>(n) * sumX2)
                               : 1.0;

    Simulator::Destroy();
    return metrics;
}

static void
PrintSectionHeader(std::ofstream& ofs, const std::string& title)
{
    std::string bar(68, '=');
    std::cout << "\n" << bar << "\n  " << title << "\n" << bar << "\n";
    ofs << "\n" << bar << "\n  " << title << "\n" << bar << "\n";
}

int
main(int argc, char* argv[])
{
    Time::SetResolution(Time::NS);

    // Active domains (you can toggle any combination)
    bool activeWifi = true;
    bool activeLrwpan = true;
    bool activeWired = false;

    // Fixed baseline values (middle-ish)
    uint32_t fixedNodes = 60;
    uint32_t fixedFlows = 30;
    uint32_t fixedPps = 300;
    double fixedSpeed = 15.0;
    double fixedCoverage = 3.0;

    double simTimeSec = 60.0;
    double warmupSec = 10.0;
    double txRangeM = 50.0;

    // rLEDBAT base parameters (kept fixed here)
    double targetDelayMs = 10;
    uint32_t baseHistSize = 2;
    double baseRefreshSec = 5.0;

    // Sweep arrays (set any array to one value for quick single-point runs).
    std::vector<uint32_t> sweepNodes{20, 40, 60, 80, 100};
    std::vector<uint32_t> sweepFlows{10, 20, 30, 40, 50};
    std::vector<uint32_t> sweepPps{100, 200, 300, 400, 500};
    std::vector<double> sweepSpeed{5, 10, 15, 20, 25};
    std::vector<double> sweepCoverage{1, 2, 3, 4, 5};

    bool enableTraceDat = true;
    bool singleRun = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("activeWifi", "Enable WiFi mobile domain", activeWifi);
    cmd.AddValue("activeLrwpan", "Enable LR-WPAN static domain", activeLrwpan);
    cmd.AddValue("activeWired", "Deprecated; must remain false", activeWired);

    cmd.AddValue("fixedNodes", "Fixed node count", fixedNodes);
    cmd.AddValue("fixedFlows", "Fixed flow count", fixedFlows);
    cmd.AddValue("fixedPps", "Fixed pps", fixedPps);
    cmd.AddValue("fixedSpeed", "Fixed WiFi speed", fixedSpeed);
    cmd.AddValue("fixedCoverage", "Fixed LR-WPAN coverage multiplier", fixedCoverage);

    cmd.AddValue("simTime", "Simulation time", simTimeSec);
    cmd.AddValue("warmup", "Warmup time", warmupSec);
    cmd.AddValue("txRange", "Reference Tx range", txRangeM);

    cmd.AddValue("targetDelay", "rLEDBAT target delay ms", targetDelayMs);
    cmd.AddValue("baseHistSize", "rLEDBAT BaseDelayHistorySize", baseHistSize);
    cmd.AddValue("baseRefresh", "rLEDBAT BaseDelayRefreshInterval sec", baseRefreshSec);

    cmd.AddValue("traceDat", "Enable rLEDBAT trace dat files", enableTraceDat);
    cmd.AddValue("singleRun",
                 "Run exactly one scenario (fixed params only), skip sweeps",
                 singleRun);
    cmd.Parse(argc, argv);

    // Avoid a known TCP SACK retransmit assert corner-case in heavy Wi-Fi loss.
    // This is scoped to this experiment only.
    Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(false));

    std::ofstream ofs("scratch/final_integrated_results.txt", std::ios::out | std::ios::trunc);
    ofs << "====================================================================\n";
    ofs << " Final Integrated Test Results\n";
    ofs << " Domains: wifi=" << (activeWifi ? "on" : "off")
        << " lrwpan=" << (activeLrwpan ? "on" : "off")
        << " wired=" << (activeWired ? "on" : "off") << "\n";
    ofs << " One-factor-at-a-time sweeps; each test repeated for base and modified rLEDBAT\n";
    ofs << "====================================================================\n";

    auto runOne = [&](const std::string& sweepName,
                      const std::string& valLabel,
                      const ScenarioConfig& inCfg) {
        std::cout << "  Running [" << inCfg.tag << "] ... " << std::flush;
        auto m = RunScenario(inCfg);
        std::cout << "done\n";
        WriteMetrics(ofs, inCfg, m);
        (void)sweepName;
        (void)valLabel;
    };

    auto baseTemplate = [&]() {
        ScenarioConfig cfg;
        cfg.activeWifi = activeWifi;
        cfg.activeLrwpan = activeLrwpan;
        cfg.activeWired = activeWired;

        cfg.numNodes = fixedNodes;
        cfg.numFlows = fixedFlows;
        cfg.pps = fixedPps;
        cfg.speedMps = fixedSpeed;
        cfg.coverageMultiplier = fixedCoverage;

        cfg.simTimeSec = simTimeSec;
        cfg.warmupSec = warmupSec;
        cfg.txRangeM = txRangeM;

        cfg.targetDelayMs = targetDelayMs;
        cfg.baseDelayHistSize = baseHistSize;
        cfg.baseDelayRefreshSec = baseRefreshSec;

        cfg.enableTraceDat = enableTraceDat;
        return cfg;
    };

    if (singleRun)
    {
        ScenarioConfig cfg = baseTemplate();
        cfg.enableMods = true;
        cfg.enableCadf = true;
        cfg.enableEcs = true;
        cfg.enableAtd = true;
        cfg.tag = "single_run";
        runOne("single", "single", cfg);

        std::cout << "\nSingle-run test complete.\n";
        std::cout << "Results: scratch/final_integrated_results.txt\n";
        return 0;
    }

    for (int pass = 0; pass < 2; ++pass)
    {
        bool mods = (pass == 1);
        std::string passLabel = mods ? "modified" : "base";

        PrintSectionHeader(ofs, std::string("PASS: ") + passLabel);

        // Sweep: nodes
        PrintSectionHeader(ofs, "Sweep NODES");
        for (uint32_t v : sweepNodes)
        {
            ScenarioConfig cfg = baseTemplate();
            cfg.enableMods = mods;
            cfg.enableCadf = mods;
            cfg.enableEcs = mods;
            cfg.enableAtd = mods;

            cfg.numNodes = v;
            std::ostringstream ss;
            ss << "pass_" << passLabel << "_nodes_" << v;
            cfg.tag = ss.str();
            runOne("nodes", std::to_string(v), cfg);
        }

        // Sweep: flows
        PrintSectionHeader(ofs, "Sweep FLOWS");
        for (uint32_t v : sweepFlows)
        {
            ScenarioConfig cfg = baseTemplate();
            cfg.enableMods = mods;
            cfg.enableCadf = mods;
            cfg.enableEcs = mods;
            cfg.enableAtd = mods;

            cfg.numFlows = v;
            std::ostringstream ss;
            ss << "pass_" << passLabel << "_flows_" << v;
            cfg.tag = ss.str();
            runOne("flows", std::to_string(v), cfg);
        }

        // Sweep: pps
        PrintSectionHeader(ofs, "Sweep PPS");
        for (uint32_t v : sweepPps)
        {
            ScenarioConfig cfg = baseTemplate();
            cfg.enableMods = mods;
            cfg.enableCadf = mods;
            cfg.enableEcs = mods;
            cfg.enableAtd = mods;

            cfg.pps = v;
            std::ostringstream ss;
            ss << "pass_" << passLabel << "_pps_" << v;
            cfg.tag = ss.str();
            runOne("pps", std::to_string(v), cfg);
        }

        // Sweep: speed (only if WiFi active)
        if (activeWifi)
        {
            PrintSectionHeader(ofs, "Sweep SPEED (WiFi mobile)");
            for (double v : sweepSpeed)
            {
                ScenarioConfig cfg = baseTemplate();
                cfg.enableMods = mods;
                cfg.enableCadf = mods;
                cfg.enableEcs = mods;
                cfg.enableAtd = mods;

                cfg.speedMps = v;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(0)
                   << "pass_" << passLabel << "_speed_" << v;
                cfg.tag = ss.str();
                runOne("speed", std::to_string(v), cfg);
            }
        }

        // Sweep: coverage (only if LR-WPAN active)
        if (activeLrwpan)
        {
            PrintSectionHeader(ofs, "Sweep COVERAGE (LR-WPAN static)");
            for (double v : sweepCoverage)
            {
                ScenarioConfig cfg = baseTemplate();
                cfg.enableMods = mods;
                cfg.enableCadf = mods;
                cfg.enableEcs = mods;
                cfg.enableAtd = mods;

                cfg.coverageMultiplier = v;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(0)
                   << "pass_" << passLabel << "_coverage_" << v << "x";
                cfg.tag = ss.str();
                runOne("coverage", std::to_string(v), cfg);
            }
        }
    }

    std::cout << "\nFinal integrated test complete.\n";
    std::cout << "Results: scratch/final_integrated_results.txt\n";

    return 0;
}
