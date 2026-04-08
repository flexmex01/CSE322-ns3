/* =========================================================================
 * RledbatTest.cc  —  Focused 3-Combination rLEDBAT Evaluation
 * =========================================================================
 *
 * PURPOSE:
 *   A clean, reproducible test harness for the base rLEDBAT algorithm
 *   and the three planned modifications from the project proposal:
 *
 *     Mod 1 – Channel-Aware Delay Filtering (CADF)
 *             → Delay spikes injected at t=8s (Combo1, Spike1) and t=11s
 *               (Combo1, Spike2) via two different mechanisms:
 *                 Spike1: n3 cross-traffic burst fills bottleneck queue
 *                         (real queue congestion; affects all flows)
 *                 Spike2: n1↔r0 channel delay raised 2ms→40ms for 50ms
 *                         (path jitter artifact; bottleneck queue stays empty)
 *               With base rLEDBAT the window drops on both; CADF filters
 *               single-RTT spikes and keeps the window stable.
 *
 *     Mod 2 – Explicit Congestion Signalling (ECS)
 *             → Bottleneck uses RED + ECN so the router MARKS packets
 *               instead of silently dropping them.  Base rLEDBAT ignores
 *               CE marks; ECS adds multiplicative decrease on CE reception
 *               at the receiver (IP-layer detection, not TCP ECE flags).
 *
 *     Mod 3 – Adaptive Target Delay (ATD) Scaling
 *             → Combo 1 (rLEDBAT alone) is the primary test case because
 *               no competing flow exists.  Base rLEDBAT uses a fixed TARGET
 *               (default 5 ms in this file); ATD will scale it to
 *               max(base_delay × α , TARGET_min) automatically.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * TOPOLOGY (5 nodes)
 *
 *   n1 (rLEDBAT update)   ──[100Mbps / 2ms]──\
 *   n2 (interactive bulk) ──[100Mbps / 2ms]──── r0 ──[10Mbps/20ms,RED+ECN]── n0
 *   n3 (spike burst)      ──[100Mbps / 2ms]──/
 *
 *   Bottleneck: r0 → n0  (10 Mbps, 20 ms, RED+ECN queue disc)
 *   Access    : senders → r0  (100 Mbps, 2 ms)
 *
 *   Node indices: 0=n0(receiver), 1=n1(update/rLEDBAT), 2=n2(interactive),
 *                 3=n3(spike), 4=r0(router)
 *
 * ─────────────────────────────────────────────────────────────────────────
 * COMBINATIONS  (all senders use BulkSend — no OnOff, no null-ACK crash)
 *
 *   Combo 1  [  1 – 20 s]   n1 alone
 *     Spike1 [  8 –  9 s]   n3 burst  → real queue congestion (bottleneck fills)
 *     Spike2 [ 11 – 11.05s] n1↔r0 channel delay 2ms→40ms → path jitter artifact
 *                            (bottleneck queue stays empty; tests CADF filtering)
 *
 *   Combo 2  [ 22 – 40 s]   n2 alone  → interactive reference baseline
 *
 *   Combo 3  [ 42 – 62 s]   n1 + n2   → rLEDBAT yielding test
 *                            n1 should throttle; n2 should hold ~10 Mbps
 *                            (ECS: faster reaction via CE marks from RED router)
 *
 *   2-second gaps between combos let TCP connections drain cleanly.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * KEY FIXES vs. FinatTest.cc
 *   – No OnOff apps  →  eliminates null-ACK crash for short off-times
 *   – No TCP stack reinstall / node restart between sessions
 *   – Auto trace-attach in HandleAccept  →  traces follow every new
 *     rLEDBAT socket (Combo1 connection AND Combo3 re-connection)
 *   – Per-combination flow metrics via FlowMonitor timeFirstTxPacket
 *   – Each BulkSend Install() creates an independent app / TCP connection
 *     → new 5-tuple → FlowMonitor naturally separates combos
 *
 * ─────────────────────────────────────────────────────────────────────────
 * OUTPUT FILES
 *   scratch/rledbat-wledbat.dat      time(s)  W_ledbat(bytes)  [all combos]
 *   scratch/rledbat-qdelay.dat       time(s)  queuing delay(ms)
 *   scratch/rledbat-basedelay.dat    time(s)  base delay(ms)
 *   scratch/rledbat-n1-r0-*.pcap    PCAP on n1↔r0 link
 *   scratch/rledbat-n2-r0-*.pcap    PCAP on n2↔r0 link
 *   scratch/rledbat-n3-r0-*.pcap    PCAP on n3↔r0 link
 *   scratch/rledbat-bn-*.pcap       PCAP on bottleneck r0↔n0 link
 *
 *   Filter PCAP by time to isolate a combination:
 *     Combo1: 1–20s  |  Combo2: 22–40s  |  Combo3: 42–62s
 *
 * =========================================================================
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <fstream>
#include <iomanip>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("RledbatTest");

// =========================================================================
// Trace callbacks  (append one line per change to a .dat file)
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
// CustomSink  —  per-port socket-class injection + auto trace attachment
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
          m_wFile(nullptr),
          m_qFile(nullptr),
          m_bFile(nullptr)
    {
    }

    ~CustomSink() override = default;

    /**
     * Call once before Simulator::Run().
     * socketClassId = TcpRledbat::GetTypeId() or TcpSocketBase::GetTypeId().
     * targetDelay   = rLEDBAT TargetDelay attribute (ignored for base sockets).
     * useEcn        = enable TCP-level ECN on the listening socket (UseEcn="On").
     *                 Set this to match enableEcn (network infrastructure ECN),
     *                 NOT enableEcs (rLEDBAT ECS modification).  ECS uses IP-layer
     *                 CE detection in ForwardUp and is controlled via the
     *                 TcpRledbat::EnableEcs attribute / Config::SetDefault.
     */
    void
    Setup(TypeId socketClassId, uint16_t port, Time targetDelay = Time(0), bool useEcn = false)
    {
        m_socketClassId = socketClassId;
        m_port          = port;
        m_targetDelay   = targetDelay;
        m_useEcn        = useEcn;
    }

    /**
     * Attach .dat file pointers so traces are wired automatically inside
     * HandleAccept() for every future rLEDBAT connection.
     * Must be called before Simulator::Run().
     */
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

    // ── Snapshot mechanism for snapshot-based per-combination throughput ──
    struct Snap
    {
        std::string label;
        uint64_t    bytes;
        double      timeS;
    };

    void
    TakeSnapshot(const std::string& label)
    {
        m_snaps.push_back({label, m_totalRx, Simulator::Now().GetSeconds()});
    }

    const std::vector<Snap>&
    GetSnaps() const
    {
        return m_snaps;
    }

private:
    // ── ns3 Application lifecycle ──────────────────────────────────────────

    void
    StartApplication() override
    {
        /*
         * Key pattern for per-socket class injection:
         * Temporarily set TcpL4Protocol::SocketClass to our desired class,
         * create the listening socket (which inherits the class via CreateSocket),
         * then immediately reset SocketClass to the ns3 default so that any
         * other application starting later on the same node is unaffected.
         *
         * When a sender connects, TcpL4Protocol calls Fork() on the listening
         * socket to produce the accepted data socket — Fork() is overridden in
         * TcpRledbat to return a TcpRledbat clone.  So all accepted sockets for
         * this port will be TcpRledbat without permanently changing the node.
         */
        Ptr<TcpL4Protocol> tcp = GetNode()->GetObject<TcpL4Protocol>();
        tcp->SetAttribute("SocketClass", TypeIdValue(m_socketClassId));
        m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        tcp->SetAttribute("SocketClass", TypeIdValue(TcpSocketBase::GetTypeId()));

        // TCP-level ECN: enables the socket to recognise CE marks and send
        // ECE flags in ACKs (standard TCP ECN).  This is separate from the
        // ECS modification, which uses IP-layer CE detection in ForwardUp.
        // m_useEcn should reflect enableEcn (network ECN infrastructure), not
        // enableEcs (the ECS algorithm modification flag).
        if (m_useEcn)
        {
            m_socket->SetAttribute("UseEcn", StringValue("On"));
        }

        if (!m_targetDelay.IsZero() &&
            m_socketClassId == TypeId::LookupByName("ns3::TcpRledbat"))
        {
            m_socket->SetAttribute("TargetDelay", TimeValue(m_targetDelay));
        }

        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_socket->Listen();
        m_socket->SetAcceptCallback(
            MakeNullCallback<bool, Ptr<Socket>, const Address&>(),
            MakeCallback(&CustomSink::HandleAccept, this));
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

    // ── Accept + auto trace attachment ────────────────────────────────────

    void
    HandleAccept(Ptr<Socket> socket, const Address& /* from */)
    {
        socket->SetRecvCallback(MakeCallback(&CustomSink::HandleRead, this));
        m_connectedSockets.push_back(socket);

        /*
         * Auto-attach trace sources to every newly accepted rLEDBAT socket.
         * This fires for BOTH the Combo-1 connection (t≈1s) and the
         * Combo-3 re-connection (t≈42s), so the .dat files are continuous
         * across all combinations with no manual Simulator::Schedule needed.
         */
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
                    NS_LOG_INFO("[CustomSink port=" << m_port
                                << "] rLEDBAT traces attached at t="
                                << Simulator::Now().GetSeconds() << "s");
                }
            }
        }
    }

    // ── RX data handler ───────────────────────────────────────────────────

    void
    HandleRead(Ptr<Socket> socket)
    {
        Ptr<Packet> pkt;
        Address     from;
        while ((pkt = socket->RecvFrom(from)))
        {
            if (pkt->GetSize() > 0)
            {
                m_totalRx += pkt->GetSize();
            }
        }
    }

    // ── Members ───────────────────────────────────────────────────────────
    TypeId                   m_socketClassId;
    uint16_t                 m_port;
    Time                     m_targetDelay;
    Ptr<Socket>              m_socket;
    uint64_t                 m_totalRx;
    std::vector<Ptr<Socket>> m_connectedSockets;
    std::vector<Snap>        m_snaps;
    bool                     m_useEcn{false};

    // Dat-file pointers (non-owning; files live in main())
    std::ofstream* m_wFile;
    std::ofstream* m_qFile;
    std::ofstream* m_bFile;
};

NS_OBJECT_ENSURE_REGISTERED(CustomSink);

// =========================================================================
// main
// =========================================================================
int
main(int argc, char* argv[])
{
    Time::SetResolution(Time::NS);

    // ── Configurable parameters ───────────────────────────────────────────
    std::string bottleneckBw    = "10Mbps";
    std::string bottleneckDelay = "20ms";
    std::string accessBw        = "100Mbps";
    std::string accessDelay     = "2ms";
    double      targetDelayMs   = 10;  // rLEDBAT TargetDelay (ms) — must be < Combo3 qdelay peaks (~15ms)
    bool        enableEcn       = false;  // RED+ECN on bottleneck (Mod 2 infra)
    bool        enablePcap      = false; // off by default — files grow very large
    bool        enableSpike2    = true;  // second spike (path jitter) at SP2_START in Combo1
    bool enableCadf = false, enableEcs = false, enableAtd = false;


    CommandLine cmd(__FILE__);
    cmd.AddValue("bottleneckBw",    "Bottleneck link bandwidth",        bottleneckBw);
    cmd.AddValue("bottleneckDelay", "Bottleneck one-way propagation",   bottleneckDelay);
    cmd.AddValue("targetDelay",     "rLEDBAT target queuing delay ms (default 5)", targetDelayMs);
    cmd.AddValue("ecn",             "Enable RED+ECN on bottleneck",     enableEcn);
    cmd.AddValue("pcap",            "Enable PCAP traces",               enablePcap);
    cmd.AddValue("spike2",          "Enable path-jitter spike at t=11s in Combo1 (default true)", enableSpike2);
    cmd.AddValue("cadf", "Enable CADF", enableCadf);
    cmd.AddValue("ecs",  "Enable ECS",  enableEcs);
    cmd.AddValue("atd",  "Enable ATD",  enableAtd);
    cmd.Parse(argc, argv);

    // ── Combination / spike time windows ─────────────────────────────────
    //  2-second gaps between combos allow TCP connections to drain cleanly.
    const double C1_START  =  1.0, C1_END  = 20.0;  // Combo 1: n1 alone
    const double SP1_START =  8.0, SP1_END =  9.0;  //   Spike1: n3 burst (queue congestion, 1 s)
    const double SP2_START = 11.0, SP2_END = 11.04; //   Spike2: n1↔r0 path jitter (50 ms ≈ 1 RTT)

    const double C2_START  = 22.0, C2_END  = 40.0;  // Combo 2: n2 alone

    const double C3_START  = 42.0, C3_END  = 62.0;  // Combo 3: n1 + n2 (yielding + ECS test)

    const double SIM_TIME  = 63.0;

    // ── Global TCP config ─────────────────────────────────────────────────
    Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(true));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       StringValue("ns3::TcpNewReno"));
    // Enable ECN at the TCP layer so sockets participate in ECE/CWR handshake
    // (required for standard TCP ECN behaviour; ECS uses IP-layer CE detection
    // separately and is controlled via TcpRledbat::EnableEcs below).
    Config::SetDefault("ns3::TcpRledbat::EnableCadf", BooleanValue(enableCadf));
    Config::SetDefault("ns3::TcpRledbat::EnableEcs",  BooleanValue(enableEcs));
    Config::SetDefault("ns3::TcpRledbat::EnableAtd",  BooleanValue(enableAtd));
    // Base delay: 5s epochs so each ~19-20s connection accumulates 3-4 epochs,
    // enough to push past BaseDelayHistorySize=2 and exercise the pop path.
    // (With 10s epochs, each connection only saw 1-2 epochs → pop never fired.)
    Config::SetDefault("ns3::TcpRledbat::BaseDelayRefreshInterval", TimeValue(Seconds(5)));
    Config::SetDefault("ns3::TcpRledbat::BaseDelayHistorySize",     UintegerValue(2));
    // CADF: CadfWindowSize should be approximately 2 × CadfStreakThreshold.
    // This ensures the mean fully adapts to persistent congestion within
    // roughly two streak-lengths of samples after the threshold is crossed.
    // With streakThreshold=4, windowSize=8 gives this property.
    // CadfMinAbsoluteSpike=5ms prevents 1-2ms TCP jitter from being flagged
    // as a spike when the baseline qdelay is near zero.
    Config::SetDefault("ns3::TcpRledbat::CadfStreakThreshold",  UintegerValue(4));
    Config::SetDefault("ns3::TcpRledbat::CadfWindowSize",       UintegerValue(8));
    Config::SetDefault("ns3::TcpRledbat::CadfMinAbsoluteSpike", DoubleValue(5.0));
    if (enableEcn)
    {
        Config::SetDefault("ns3::TcpSocketBase::UseEcn", StringValue("On"));
        Config::SetDefault("ns3::TcpRledbat::EcsBeta", DoubleValue(0.5));
    }
    if (enableAtd)
    {
        Config::SetDefault("ns3::TcpRledbat::AtdAlpha", DoubleValue(0.5));
        //Config::SetDefault("ns3::TcpRledbat::AtdMinTarget", DoubleValue(0.0));
    }

    // ── Nodes ─────────────────────────────────────────────────────────────
    //  0 = n0 (receiver / sink)
    //  1 = n1 (background update sender, rLEDBAT)
    //  2 = n2 (interactive sender, normal TCP)
    //  3 = n3 (delay-spike cross-traffic sender)
    //  4 = r0 (aggregation router / bottleneck origin)
    NodeContainer nodes;
    nodes.Create(5);

    InternetStackHelper stack;
    stack.Install(nodes);

    // ── Links ─────────────────────────────────────────────────────────────
    PointToPointHelper accLink;
    accLink.SetDeviceAttribute("DataRate", StringValue(accessBw));
    accLink.SetChannelAttribute("Delay",   StringValue(accessDelay));
    accLink.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("500p"));

    PointToPointHelper bnLink;
    bnLink.SetDeviceAttribute("DataRate", StringValue(bottleneckBw));
    bnLink.SetChannelAttribute("Delay",   StringValue(bottleneckDelay));
    // Keep device-layer queue tiny  → all buffering goes through the TC queue disc
    bnLink.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("4p"));

    NetDeviceContainer dev_n1_r0 = accLink.Install(nodes.Get(1), nodes.Get(4));
    NetDeviceContainer dev_n2_r0 = accLink.Install(nodes.Get(2), nodes.Get(4));
    NetDeviceContainer dev_n3_r0 = accLink.Install(nodes.Get(3), nodes.Get(4));
    NetDeviceContainer dev_r0_n0 = bnLink.Install(nodes.Get(4), nodes.Get(0));

    // ── Queue discipline on r0's outgoing bottleneck port ────────────────────
    //
    //  ECN=true  → RED + ECN  (Mod 2 / ECS infrastructure)
    //    MinTh and MaxTh are calculated so that marking begins around the
    //    rLEDBAT target queuing depth.  This avoids RED fighting rLEDBAT by
    //    dropping packets before rLEDBAT has a chance to see the delay rise.
    //    Formula: MinTh_pkts = targetDelay_ms / pktTxTime_ms
    //             pktTxTime_ms = 1460*8 / (10e6) = 1.168 ms → ~21 pkts for 25 ms
    //
    //    ECS detects CE marks at the IP layer (TcpRledbat::ForwardUp).
    //    Setting MinTh at 50% of targetDelay makes CE marking fire before the
    //    delay-based controller reacts, giving ECS its early-warning advantage.
    //
    //  ECN=false → large DropTail (for clean baseline rLEDBAT testing)
    //    Queue is free to build; rLEDBAT detects queuing via delay and
    //    self-throttles.  No early drops interfering with the delay signal.
    {
        TrafficControlHelper tch;
        double pktTxMs = (1460.0 * 8.0) / (10.0e6) * 1000.0; // ≈ 1.168 ms

        double minThPkts;
        double maxThPkts;

        if (enableEcn)
        {
            // ECN case: start marking CE early so ECS fires before the
            // delay-based controller sees the queue build.
            // MinTh = 3 pkts (≈3.5ms queuing) fires well before targetDelay=5ms.
            // MaxTh = 80 pkts gives a wide probabilistic marking range.
            // Note: max(3.0, ...) dominates for small targets; setting it
            // explicitly makes the intent clear.
            minThPkts = 3.0;
            maxThPkts = 100.0;
        }
        else
        {
            // Non-ECN / baseline case: behave like a large DropTail.
            // rLEDBAT must be allowed to see queuing delay build freely and
            // self-throttle via its delay signal.  If RED drops packets early
            // it destroys the delay measurement that rLEDBAT depends on.
            // Set MinTh near queue capacity so RED only fires as a hard backstop,
            // not as an active controller competing with rLEDBAT.
            // MaxSize = 200p → MinTh=150p (≈175ms), MaxTh=185p
            minThPkts = 150.0;
            maxThPkts = 185.0;
        }

        tch.SetRootQueueDisc("ns3::RedQueueDisc",
                            "MinTh",         DoubleValue(minThPkts),
                            "MaxTh",         DoubleValue(maxThPkts),
                            "MaxSize",       QueueSizeValue(QueueSize("200p")),
                            "LinkBandwidth", StringValue(bottleneckBw),
                            "LinkDelay",     StringValue(bottleneckDelay),
                            "UseEcn",        BooleanValue(enableEcn));
        tch.Install(dev_r0_n0.Get(0));
    }

    // ── IP addressing ─────────────────────────────────────────────────────
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(dev_n1_r0);

    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    ipv4.Assign(dev_n2_r0);

    ipv4.SetBase("10.1.3.0", "255.255.255.0");
    ipv4.Assign(dev_n3_r0);

    ipv4.SetBase("10.1.4.0", "255.255.255.0");
    Ipv4InterfaceContainer iface_r0_n0 = ipv4.Assign(dev_r0_n0);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // n0's address seen by all senders (second address on dev_r0_n0)
    Ipv4Address n0Addr = iface_r0_n0.GetAddress(1);

    // ── Well-known destination ports ──────────────────────────────────────
    const uint16_t UPDATE_PORT      = 9000;  // rLEDBAT background update
    const uint16_t INTERACTIVE_PORT = 9001;  // interactive / high-priority
    const uint16_t SPIKE_PORT       = 9002;  // delay-spike cross-traffic

    // ── Sinks on n0  (persistent – run entire simulation) ─────────────────
    //  updateSink   accepts TcpRledbat connections from n1
    //  interactiveSink  accepts TcpSocketBase connections from n2
    //  spikeSink    accepts TcpSocketBase connections from n3
    //
    //  The 4th argument to Setup() is useEcn, which enables TCP-level ECN
    //  (ECE/CWR) on the listening socket.  This must follow enableEcn (the
    //  network infrastructure flag), not enableEcs (the ECS algorithm mod).
    //  ECS operates at the IP layer via TcpRledbat::ForwardUp and is
    //  independently controlled by Config::SetDefault for EnableEcs above.
    //
    //  StartTime offsets avoid a race where multiple sinks change SocketClass
    //  simultaneously:  updateSink (rLEDBAT) starts first, immediately resets
    //  SocketClass, then interactiveSink starts, etc.

    Ptr<CustomSink> updateSink = CreateObject<CustomSink>();
    updateSink->Setup(TypeId::LookupByName("ns3::TcpRledbat"),
                      UPDATE_PORT,
                      MilliSeconds(targetDelayMs),
                      enableEcn);  // useEcn = enableEcn (TCP-level ECN, not ECS mod)
    nodes.Get(0)->AddApplication(updateSink);
    updateSink->SetStartTime(Seconds(0.00));
    updateSink->SetStopTime(Seconds(SIM_TIME));

    Ptr<CustomSink> interactiveSink = CreateObject<CustomSink>();
    interactiveSink->Setup(TcpSocketBase::GetTypeId(), INTERACTIVE_PORT);
    nodes.Get(0)->AddApplication(interactiveSink);
    interactiveSink->SetStartTime(Seconds(0.05));
    interactiveSink->SetStopTime(Seconds(SIM_TIME));

    Ptr<CustomSink> spikeSink = CreateObject<CustomSink>();
    spikeSink->Setup(TcpSocketBase::GetTypeId(), SPIKE_PORT);
    nodes.Get(0)->AddApplication(spikeSink);
    spikeSink->SetStartTime(Seconds(0.10));
    spikeSink->SetStopTime(Seconds(SIM_TIME));

    // ── Trace dat files ───────────────────────────────────────────────────
    //  These three files are written continuously across all combinations.
    //  Comment markers are inserted at phase boundaries so you can see
    //  exactly which combo produced which variation.  Visualise with gnuplot:
    //    plot "rledbat-wledbat.dat"    u 1:2 w l title "W_ledbat"
    //    plot "rledbat-qdelay.dat"     u 1:2 w l title "Queuing delay (ms)"
    //    plot "rledbat-basedelay.dat"  u 1:2 w l title "Base delay (ms)"

    std::ofstream wFile("scratch/rledbat-wledbat.dat");
    std::ofstream qFile("scratch/rledbat-qdelay.dat");
    std::ofstream bFile("scratch/rledbat-basedelay.dat");

    wFile << "# t(s)  W_ledbat(bytes)\n"
          << "# Combo1:[1-20s]  Combo2:[22-40s]  Combo3:[42-62s]\n"
          << "# Spike1 at t=8-9s (n3 queue congestion, Combo1)\n"
          << "# Spike2 at t=11-11.05s (n1<->r0 path jitter, Combo1)\n";
    qFile << "# t(s)  QueuingDelay(ms)\n";
    bFile << "# t(s)  BaseDelay(ms)\n";

    // Wire trace file pointers into updateSink — HandleAccept auto-attaches
    // them to EVERY future TcpRledbat accepted socket (Combo1 + Combo3)
    updateSink->SetTraceFiles(&wFile, &qFile, &bFile);

    // Phase-boundary markers written into dat files for easy gnuplot annotation
    auto markPhase = [&](double t, const std::string& lbl) {
        Simulator::Schedule(Seconds(t), [t, lbl, &wFile, &qFile, &bFile]() {
            std::string marker =
                std::string("# --- ") + lbl + "  t=" + std::to_string(t) + "s ---\n";
            wFile << marker;
            qFile << marker;
            bFile << marker;
        });
    };
    markPhase(C1_START,  "Combo1_START (n1 alone)");
    markPhase(SP1_START, "Spike1_START (n3 queue burst)");
    markPhase(SP1_END,   "Spike1_END");
    if (enableSpike2)
    {
        markPhase(SP2_START, "Spike2_START (n1<->r0 path jitter, not queue congestion)");
        markPhase(SP2_END,   "Spike2_END");
    }
    markPhase(C1_END,    "Combo1_END");
    markPhase(C2_START,  "Combo2_START (n2 alone)");
    markPhase(C2_END,    "Combo2_END");
    markPhase(C3_START,  "Combo3_START (n1+n2)");
    markPhase(C3_END,    "Combo3_END");

    // ── Snapshots at phase boundaries (for byte-delta throughput) ─────────
    //  Snap indices (in order of scheduling):
    //    0 = C1_START   1 = C1_END
    //    2 = C2_START   3 = C2_END
    //    4 = C3_START   5 = C3_END
    auto snap = [&](double t, const std::string& lbl) {
        Simulator::Schedule(Seconds(t), &CustomSink::TakeSnapshot, updateSink,     lbl);
        Simulator::Schedule(Seconds(t), &CustomSink::TakeSnapshot, interactiveSink, lbl);
        Simulator::Schedule(Seconds(t), &CustomSink::TakeSnapshot, spikeSink,       lbl);
    };
    snap(C1_START, "c1_start");
    snap(C1_END,   "c1_end");
    snap(C2_START, "c2_start");
    snap(C2_END,   "c2_end");
    snap(C3_START, "c3_start");
    snap(C3_END,   "c3_end");

    // ── Senders  (each Install() creates an independent BulkSend app
    //            with its own TCP connection → own 5-tuple → own FlowMonitor
    //            flow → naturally separated per-combination metrics) ────────

    BulkSendHelper bulk("ns3::TcpSocketFactory", Address());
    bulk.SetAttribute("MaxBytes", UintegerValue(0)); // unlimited (stopped by time)

    // Install n1 Combo1 sender
    bulk.SetAttribute("Remote", AddressValue(InetSocketAddress(n0Addr, UPDATE_PORT)));
    ApplicationContainer c1_n1 = bulk.Install(nodes.Get(1));
    c1_n1.Start(Seconds(C1_START));
    c1_n1.Stop(Seconds(C1_END));

    // ── Spike 1: n3 short burst to fill bottleneck queue (Combo1, t=8-9s) ──
    //  Causes real queue congestion visible in rLEDBAT's RTT-based delay estimate.
    //  Base rLEDBAT will reduce W_ledbat; CADF should suppress window decrease
    //  for this single-RTT event.
    bulk.SetAttribute("Remote", AddressValue(InetSocketAddress(n0Addr, SPIKE_PORT)));
    ApplicationContainer sp1_n3 = bulk.Install(nodes.Get(3));
    sp1_n3.Start(Seconds(SP1_START));
    sp1_n3.Stop(Seconds(SP1_END));

    // Install n2 Combo2 sender
    bulk.SetAttribute("Remote", AddressValue(InetSocketAddress(n0Addr, INTERACTIVE_PORT)));
    ApplicationContainer c2_n2 = bulk.Install(nodes.Get(2));
    c2_n2.Start(Seconds(C2_START));
    c2_n2.Stop(Seconds(C2_END));

    // Install n1 Combo3 sender
    bulk.SetAttribute("Remote", AddressValue(InetSocketAddress(n0Addr, UPDATE_PORT)));
    ApplicationContainer c3_n1 = bulk.Install(nodes.Get(1));
    c3_n1.Start(Seconds(C3_START));
    c3_n1.Stop(Seconds(C3_END));

    // Install n2 Combo3 sender
    bulk.SetAttribute("Remote", AddressValue(InetSocketAddress(n0Addr, INTERACTIVE_PORT)));
    ApplicationContainer c3_n2 = bulk.Install(nodes.Get(2));
    c3_n2.Start(Seconds(C3_START));
    c3_n2.Stop(Seconds(C3_END));

    // ── Spike 2: n1↔r0 path jitter mid-Combo1 (optional, --spike2=false to disable) ──
    //  Raises the bidirectional n1↔r0 channel delay from 2ms to 40ms for 50ms
    //  (≈1 RTT).  The bottleneck queue stays EMPTY — this is a pure propagation-
    //  delay artifact, not queue congestion.  One-way n1→n0 delay rises from 22ms
    //  to 60ms; RTT at n0 (measured via TSecr) rises from ~44ms to ~120ms, giving
    //  a qdelay spike of ~76ms >> target.  CADF should detect this as a spike
    //  (76ms >> 2.5 × recent mean ≈ 0ms) and suppress the window decrease.
    //  Spike2 is intentionally during Combo1, not Combo3.
    if (enableSpike2)
    {
        Ptr<PointToPointChannel> ch_n1_r0 =
            DynamicCast<PointToPointChannel>(dev_n1_r0.Get(0)->GetChannel());

        // Raise n1↔r0 bidirectional channel delay from 2ms to 40ms.
        Simulator::Schedule(Seconds(SP2_START), [ch_n1_r0]() {
            ch_n1_r0->SetAttribute("Delay", StringValue("40ms"));
            NS_LOG_INFO("Spike2: n1<->r0 channel delay raised to 40ms (path jitter)");
        });

        // Restore after 50 ms (≈1 RTT at normal delay of 44ms).
        // Only packets in flight during this window see the elevated delay.
        Simulator::Schedule(Seconds(SP2_END), [ch_n1_r0]() {
            ch_n1_r0->SetAttribute("Delay", StringValue("2ms"));
            NS_LOG_INFO("Spike2: n1<->r0 channel delay restored to 2ms");
        });
    }

    // ── PCAP per link ─────────────────────────────────────────────────────
    if (enablePcap)
    {
        accLink.EnablePcap("scratch/rledbat-n1-r0", dev_n1_r0.Get(0), false);
        accLink.EnablePcap("scratch/rledbat-n2-r0", dev_n2_r0.Get(0), false);
        accLink.EnablePcap("scratch/rledbat-n3-r0", dev_n3_r0.Get(0), false);
        bnLink.EnablePcap("scratch/rledbat-bn",     dev_r0_n0.Get(0), false);
    }

    // ── Flow Monitor ──────────────────────────────────────────────────────
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor>   flowMon = fmHelper.InstallAll();

    // ── Print test configuration ─────────────────────────────────────────
    std::cout << "\nrLEDBAT Test Configuration\n";
    std::cout << "  Target delay  : " << targetDelayMs << " ms\n";
    std::cout << "  Bottleneck    : " << bottleneckBw << " / " << bottleneckDelay << "\n";
    std::cout << "  Queue mode    : RED ("
              << (enableEcn ? "MinTh=3p, MaxTh=80p, ECN marking"
                            : "MinTh=150p, MaxTh=185p, DropTail-like")
              << ")\n";
    std::cout << "  ECN at TCP    : " << (enableEcn ? "On" : "Off") << "\n";
    std::cout << "  CADF          : " << (enableCadf ? "On" : "Off") << "\n";
    std::cout << "  ECS           : " << (enableEcs  ? "On" : "Off") << "\n";
    std::cout << "  ATD           : " << (enableAtd  ? "On" : "Off") << "\n";
    std::cout << "\n";

    // ── Run ───────────────────────────────────────────────────────────────
    Simulator::Stop(Seconds(SIM_TIME));
    Simulator::Run();

    // Close dat files immediately after run (all trace events have fired)
    wFile.close();
    qFile.close();
    bFile.close();

    // =========================================================================
    // Post-simulation analysis
    // =========================================================================
    flowMon->CheckForLostPackets();
    auto classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    const auto& stats = flowMon->GetFlowStats();

    // ── Per-flow record ───────────────────────────────────────────────────
    struct FlowRec
    {
        std::string                           label;
        Ipv4FlowClassifier::FiveTuple         tuple;
        uint32_t                              txPkts{0}, rxPkts{0};
        uint64_t                              rxBytes{0};
        double delayMs{0}, jitterMs{0}, tputMbps{0}, pdr{1}, dropRatio{0};
        bool   isData{true};
        int    combo{0}; // 1, 2, or 3
    };

    std::vector<FlowRec> allFlows;

    for (const auto& [fid, fs] : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(fid);
        double startSec = fs.timeFirstTxPacket.GetSeconds();
        double dur      = std::max(fs.timeLastRxPacket.GetSeconds() -
                                       fs.timeFirstTxPacket.GetSeconds(),
                                   0.5);

        FlowRec rec;
        rec.tuple    = t;
        rec.txPkts   = fs.txPackets;
        rec.rxPkts   = fs.rxPackets;
        rec.rxBytes  = fs.rxBytes;
        rec.delayMs  = (fs.rxPackets > 0)
                           ? fs.delaySum.GetMilliSeconds() / fs.rxPackets
                           : 0.0;
        rec.jitterMs = (fs.rxPackets > 1)
                           ? fs.jitterSum.GetMilliSeconds() / (fs.rxPackets - 1)
                           : 0.0;
        rec.tputMbps = fs.rxBytes * 8.0 / dur / 1e6;
        rec.pdr      = (fs.txPackets > 0)
                           ? static_cast<double>(fs.rxPackets) / fs.txPackets
                           : 1.0;
        uint32_t lost  = (fs.txPackets >= fs.rxPackets)
                             ? fs.txPackets - fs.rxPackets
                             : 0;
        rec.dropRatio  = (fs.txPackets > 0)
                             ? static_cast<double>(lost) / fs.txPackets
                             : 0.0;

        rec.isData = (t.destinationPort == UPDATE_PORT ||
                      t.destinationPort == INTERACTIVE_PORT ||
                      t.destinationPort == SPIKE_PORT);

        if      (t.destinationPort == UPDATE_PORT)
            rec.label = "n1→n0  update [rLEDBAT, port 9000]";
        else if (t.destinationPort == INTERACTIVE_PORT)
            rec.label = "n2→n0  interactive [TcpSocketBase, port 9001]";
        else if (t.destinationPort == SPIKE_PORT)
            rec.label = "n3→n0  delay-spike [port 9002]";
        else if (t.sourcePort == UPDATE_PORT)
            rec.label = "n0→n1  update ACKs";
        else if (t.sourcePort == INTERACTIVE_PORT)
            rec.label = "n0→n2  interactive ACKs";
        else if (t.sourcePort == SPIKE_PORT)
            rec.label = "n0→n3  spike ACKs";
        else
            rec.label = "unknown";

        if      (startSec <  21.0) rec.combo = 1;
        else if (startSec <  41.0) rec.combo = 2;
        else                       rec.combo = 3;

        allFlows.push_back(rec);
    }

    // ── Per-combination printer ───────────────────────────────────────────
    auto printCombo = [&](int comboId,
                          const std::string& title,
                          double cStart,
                          double cEnd) {
        double dur = cEnd - cStart;

        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  " << title << "\n";
        std::cout << "║  t = " << cStart << "–" << cEnd << " s  |  duration = "
                  << dur << " s\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

        double sumX = 0.0, sumX2 = 0.0;
        int    nFair = 0;
        uint64_t totalRxPkts = 0;

        for (const auto& rec : allFlows)
        {
            if (rec.combo != comboId) continue;

            bool    isSpike = (rec.tuple.destinationPort == SPIKE_PORT ||
                               rec.tuple.sourcePort      == SPIKE_PORT);
            uint32_t lost   = (rec.txPkts >= rec.rxPkts)
                                  ? rec.txPkts - rec.rxPkts
                                  : 0;

            std::cout << "\n  ── " << rec.label;
            if (isSpike) std::cout << "  (SPIKE burst, duration≈1s)";
            std::cout << "\n";
            std::cout << "     " << rec.tuple.sourceAddress << ":"
                      << rec.tuple.sourcePort << " → "
                      << rec.tuple.destinationAddress << ":"
                      << rec.tuple.destinationPort << "\n";
            std::cout << std::fixed << std::setprecision(4);
            std::cout << "     Throughput   : " << rec.tputMbps << " Mbps\n";
            std::cout << std::setprecision(2);
            std::cout << "     E2E Delay    : " << rec.delayMs   << " ms\n";
            std::cout << "     Jitter       : " << rec.jitterMs  << " ms\n";
            std::cout << std::setprecision(4);
            std::cout << "     PDR          : " << rec.pdr * 100.0 << " %  ("
                      << rec.rxPkts << "/" << rec.txPkts << ")\n";
            std::cout << "     Drop Ratio   : " << rec.dropRatio * 100.0 << " %  ("
                      << lost << " lost)\n";

            if (rec.isData) totalRxPkts += rec.rxPkts;

            if (rec.isData && !isSpike && rec.tputMbps > 0.0)
            {
                sumX  += rec.tputMbps;
                sumX2 += rec.tputMbps * rec.tputMbps;
                nFair++;
            }
        }

        std::cout << "\n  Network Packet Rate : "
                  << std::fixed << std::setprecision(1)
                  << static_cast<double>(totalRxPkts) / dur << " pkts/s\n";

        if (nFair > 1 && sumX2 > 0.0)
        {
            double jain = (sumX * sumX) / (nFair * sumX2);
            std::cout << "  Jain's Fairness     : " << std::setprecision(6)
                      << jain << "  (n=" << nFair << " data flows)\n";
            if (comboId == 3)
            {
                std::cout << "  ↳ Jain < 1 confirms rLEDBAT yielded to interactive\n";
                std::cout << "    (rLEDBAT tput ↓, interactive tput stays near Combo-2 value)\n";
            }
        }
    };

    std::cout << "\n";
    std::cout << "============================================================\n";
    std::cout << "  rLEDBAT Focused Test — Per-Combination Flow Metrics\n";
    std::cout << "============================================================\n";

    printCombo(1, "Combo 1: rLEDBAT Alone  [baseline + CADF + ATD test]",
               C1_START, C1_END);
    printCombo(2, "Combo 2: Interactive Alone  [reference baseline]",
               C2_START, C2_END);
    printCombo(3, "Combo 3: rLEDBAT + Interactive  [yielding + ECS test]",
               C3_START, C3_END);

    // ── Snapshot-based per-combination throughput (byte deltas) ──────────
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  Snapshot-Based Throughput (byte deltas at phase boundaries)\n";
    std::cout << "════════════════════════════════════════════════════════════\n";

    const auto& us = updateSink->GetSnaps();
    const auto& is = interactiveSink->GetSnaps();

    auto getB = [](const std::vector<CustomSink::Snap>& v, size_t i) -> uint64_t {
        return (i < v.size()) ? v[i].bytes : 0ULL;
    };
    auto mbps = [](uint64_t bytes, double dur) -> double {
        return (dur > 0 && bytes > 0) ? bytes * 8.0 / dur / 1e6 : 0.0;
    };

    double c1dur = C1_END - C1_START;
    double c2dur = C2_END - C2_START;
    double c3dur = C3_END - C3_START;

    uint64_t u_c1 = getB(us, 1) - getB(us, 0);
    uint64_t i_c2 = getB(is, 3) - getB(is, 2);
    uint64_t u_c3 = getB(us, 5) - getB(us, 4);
    uint64_t i_c3 = getB(is, 5) - getB(is, 4);

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n  Combo 1  n1 (rLEDBAT)    : " << mbps(u_c1, c1dur) << " Mbps\n";
    std::cout << "  Combo 2  n2 (interactive) : " << mbps(i_c2, c2dur) << " Mbps\n";
    std::cout << "  Combo 3  n1 (rLEDBAT)    : " << mbps(u_c3, c3dur) << " Mbps\n";
    std::cout << "  Combo 3  n2 (interactive) : " << mbps(i_c3, c3dur) << " Mbps\n";

    // =========================================================================
    // Overall per-node and network-wide aggregate metrics
    // =========================================================================
    struct NodeAgg
    {
        uint64_t txPkts{0}, rxPkts{0}, rxBytes{0};
        double   jitterSum{0};
        int      jitterSamples{0};
    };

    NodeAgg aggN1, aggN2, aggN3, aggNet;

    for (const auto& rec : allFlows)
    {
        if (!rec.isData) continue;

        NodeAgg* bucket = nullptr;
        if      (rec.tuple.destinationPort == UPDATE_PORT)      bucket = &aggN1;
        else if (rec.tuple.destinationPort == INTERACTIVE_PORT) bucket = &aggN2;
        else if (rec.tuple.destinationPort == SPIKE_PORT)       bucket = &aggN3;

        if (bucket)
        {
            bucket->txPkts        += rec.txPkts;
            bucket->rxPkts        += rec.rxPkts;
            bucket->rxBytes       += rec.rxBytes;
            bucket->jitterSum     += rec.jitterMs * std::max(0, (int)rec.rxPkts - 1);
            bucket->jitterSamples += std::max(0, (int)rec.rxPkts - 1);
        }

        aggNet.txPkts        += rec.txPkts;
        aggNet.rxPkts        += rec.rxPkts;
        aggNet.rxBytes       += rec.rxBytes;
        aggNet.jitterSum     += rec.jitterMs * std::max(0, (int)rec.rxPkts - 1);
        aggNet.jitterSamples += std::max(0, (int)rec.rxPkts - 1);
    }

    double durN1  = (C1_END - C1_START) + (C3_END - C3_START);
    double durN2  = (C2_END - C2_START) + (C3_END - C3_START);
    double durN3  = SP1_END - SP1_START;
    double durNet = SIM_TIME;

    auto pct   = [](uint64_t rx, uint64_t tx) {
        return tx > 0 ? 100.0 * static_cast<double>(rx) / tx : 0.0;
    };
    auto dropP = [](uint64_t rx, uint64_t tx) {
        uint64_t lost = (tx >= rx) ? tx - rx : 0;
        return tx > 0 ? 100.0 * static_cast<double>(lost) / tx : 0.0;
    };
    auto jAvg  = [](const NodeAgg& a) {
        return a.jitterSamples > 0 ? a.jitterSum / a.jitterSamples : 0.0;
    };
    auto tput  = [](uint64_t bytes, double dur) {
        return dur > 0 ? bytes * 8.0 / dur / 1e6 : 0.0;
    };

    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  Overall Per-Node Metrics  (data flows only, all combos)\n";
    std::cout << "════════════════════════════════════════════════════════════\n";

    auto printNodeAgg = [&](const std::string& name, const NodeAgg& a, double dur) {
        uint64_t lost = (a.txPkts >= a.rxPkts) ? a.txPkts - a.rxPkts : 0;
        std::cout << "\n  ── " << name << "\n";
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "     Throughput   : " << tput(a.rxBytes, dur) << " Mbps"
                << "  (active duration = " << dur << " s)\n";
        std::cout << std::setprecision(4);
        std::cout << "     Jitter       : " << jAvg(a)             << " ms\n";
        std::cout << "     PDR          : " << pct(a.rxPkts, a.txPkts) << " %"
                << "  (" << a.rxPkts << "/" << a.txPkts << ")\n";
        std::cout << "     Drop Ratio   : " << dropP(a.rxPkts, a.txPkts) << " %"
                << "  (" << lost << " lost)\n";
    };

    printNodeAgg("n1  (rLEDBAT)      active in Combo1 + Combo3", aggN1, durN1);
    printNodeAgg("n2  (interactive)  active in Combo2 + Combo3", aggN2, durN2);
    if (aggN3.txPkts > 0)
        printNodeAgg("n3  (spike)        active during Spike1 only", aggN3, durN3);

    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  Network-Wide Aggregate  (all data flows, full sim time)\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    {
        uint64_t lost = (aggNet.txPkts >= aggNet.rxPkts)
                            ? aggNet.txPkts - aggNet.rxPkts : 0;
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "\n     Total Throughput : " << tput(aggNet.rxBytes, durNet) << " Mbps\n";
        std::cout << "     Avg Jitter       : " << jAvg(aggNet)                  << " ms\n";
        std::cout << "     PDR              : " << pct(aggNet.rxPkts, aggNet.txPkts)  << " %"
                << "  (" << aggNet.rxPkts << "/" << aggNet.txPkts << ")\n";
        std::cout << "     Drop Ratio       : " << dropP(aggNet.rxPkts, aggNet.txPkts) << " %"
                << "  (" << lost << " lost)\n";
    }
    std::cout << "\n";

    // ── Expected behaviour summary ────────────────────────────────────────
    std::cout << "\n";
    std::cout << "\n";
    if (enablePcap)
    {
        std::cout << "  PCAP files (filter by time for per-combination view):\n";
        std::cout << "    scratch/rledbat-n1-r0-*.pcap\n";
        std::cout << "    scratch/rledbat-n2-r0-*.pcap\n";
        std::cout << "    scratch/rledbat-n3-r0-*.pcap\n";
        std::cout << "    scratch/rledbat-bn-*.pcap\n";
    }
    std::cout << "\n";

    Simulator::Destroy();
    return 0;
}
