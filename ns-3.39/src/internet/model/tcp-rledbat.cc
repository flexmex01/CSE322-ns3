#include "tcp-rledbat.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/tcp-option-ts.h"
#include "ns3/ipv4-interface.h"


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("TcpRledbat");
NS_OBJECT_ENSURE_REGISTERED(TcpRledbat);


TypeId
TcpRledbat::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TcpRledbat")
        .SetParent<TcpSocketBase>()
        .SetGroupName("Internet")
        .AddConstructor<TcpRledbat>()
        .AddAttribute("TargetDelay",
                    "Target queueing delay for rLEDBAT",
                    TimeValue(MilliSeconds(25)),
                    MakeTimeAccessor(&TcpRledbat::m_target),
                    MakeTimeChecker())
        .AddAttribute("Gain",
                    "Gain parameter for proportional controller",
                    DoubleValue(1.0),
                    MakeDoubleAccessor(&TcpRledbat::m_gain),
                    MakeDoubleChecker<double>(0.0))
        .AddAttribute("MaxWindow",
              "Maximum advertised window size in bytes",
              UintegerValue(65535),
              MakeUintegerAccessor(&TcpRledbat::m_maxWindow),
              MakeUintegerChecker<uint32_t>())
        .AddAttribute("MinWindow",
                    "Minimum advertised window size in bytes",
                    UintegerValue(2*1460),
                    MakeUintegerAccessor(&TcpRledbat::m_minWindow),
                    MakeUintegerChecker<uint32_t>())
         .AddAttribute("BaseDelayHistorySize",
                    "Number of RTT samples to maintain for base delay",
                    UintegerValue(10),
                    MakeUintegerAccessor(&TcpRledbat::m_baseDelayHistorySize),
                    MakeUintegerChecker<uint32_t>())
        .AddAttribute("BaseDelayRefreshInterval",
                        "Interval to refresh base delay estimate",
                    TimeValue(Minutes(1)),
                    MakeTimeAccessor(&TcpRledbat::m_baseDelayRefreshInterval),
                    MakeTimeChecker())
        .AddTraceSource("WLedbat",
                        "rLEDBAT computed window size",
                        MakeTraceSourceAccessor(&TcpRledbat::m_tracedWLedbat),
                        "ns3::TracedValueCallback::Uint32")
        .AddTraceSource("QueuingDelay",
                        "Estimated queuing delay",
                        MakeTraceSourceAccessor(&TcpRledbat::m_tracedQueuingDelay),
                        "ns3::TracedValueCallback::Time")
        .AddTraceSource("BaseDelay",
                        "Base delay estimate",
                        MakeTraceSourceAccessor(&TcpRledbat::m_tracedBaseDelay),
                        "ns3::TracedValueCallback::Time")
        // ── Mod 1: Channel-Aware Delay Filtering (CADF) ──────────────────
        .AddAttribute("EnableCadf",
                    "Enable Channel-Aware Delay Filtering to suppress transient spikes",
                    BooleanValue(false),
                    MakeBooleanAccessor(&TcpRledbat::m_enableCadf),
                    MakeBooleanChecker())
        .AddAttribute("CadfWindowSize",
                    "Number of recent qdelay samples kept for spike detection",
                    UintegerValue(20),
                    MakeUintegerAccessor(&TcpRledbat::m_cadfWindowSize),
                    MakeUintegerChecker<uint32_t>(2, 64))
        .AddAttribute("CadfSpikeRatio",
                    "qdelay is a spike if it exceeds CadfSpikeRatio x recent mean",
                    DoubleValue(2.5),
                    MakeDoubleAccessor(&TcpRledbat::m_cadfSpikeRatio),
                    MakeDoubleChecker<double>(1.0))
        .AddAttribute("CadfStreakThreshold",
                    "Consecutive spike count before treating elevated delay as "
                    "persistent congestion (and allowing window decrease). "
                    "Transient spikes shorter than this many packets are suppressed.",
                    UintegerValue(4),
                    MakeUintegerAccessor(&TcpRledbat::m_cadfStreakThreshold),
                    MakeUintegerChecker<uint32_t>(1))
        .AddAttribute("CadfMinAbsoluteSpike",
                    "Minimum absolute rise in qdelay (ms) required to classify a sample "
                    "as a spike, regardless of the ratio test.  Prevents false positives "
                    "when the baseline qdelay is near zero and tiny TCP jitter (1-2 ms) "
                    "would otherwise exceed CadfSpikeRatio x mean.",
                    DoubleValue(5.0),
                    MakeDoubleAccessor(&TcpRledbat::m_cadfMinAbsoluteSpike),
                    MakeDoubleChecker<double>(0.0))
        // ── Mod 2: Explicit Congestion Signalling (ECS) ───────────────────
        .AddAttribute("EnableEcs",
                    "Enable multiplicative window decrease on ECN-CE reception",
                    BooleanValue(false),
                    MakeBooleanAccessor(&TcpRledbat::m_enableEcs),
                    MakeBooleanChecker())
        .AddAttribute("EcsBeta",
                    "Multiplicative decrease factor applied on ECN-CE (0 < beta < 1)",
                    DoubleValue(0.5),
                    MakeDoubleAccessor(&TcpRledbat::m_ecsBeta),
                    MakeDoubleChecker<double>(0.0, 1.0))
        // ── Mod 3: Adaptive Target Delay (ATD) ───────────────────────────
        .AddAttribute("EnableAtd",
                    "Enable adaptive target delay scaling with base delay",
                    BooleanValue(false),
                    MakeBooleanAccessor(&TcpRledbat::m_enableAtd),
                    MakeBooleanChecker())
        .AddAttribute("AtdAlpha",
                    "Scaling factor: effective_target = max(base_delay * alpha, AtdMinTarget)",
                    DoubleValue(0.0),
                    MakeDoubleAccessor(&TcpRledbat::m_atdAlpha),
                    MakeDoubleChecker<double>(0.0))
        .AddAttribute("AtdMinTarget",
                    "Minimum allowed target when ATD is active",
                    TimeValue(MilliSeconds(5)),
                    MakeTimeAccessor(&TcpRledbat::m_atdMinTarget),
                    MakeTimeChecker());

        return tid;

}
TypeId
TcpRledbat::GetInstanceTypeId() const
{
    return GetTypeId();
}

TcpRledbat::TcpRledbat()
        : TcpSocketBase(),
        m_target(MilliSeconds(25)),
        m_gain(1.0),
        m_minWindow(2*1460),
        m_baseDelayHistorySize(10),
        m_baseDelayRefreshInterval(Minutes(1)),
        m_wLedbat(65535),
        m_maxWindow(65535),
        m_currentDelay(Time(0)),
        m_baseDelay(Time::Max()),
        m_currentEpochMin(Time::Max()),
        m_epochStart(Time(0)),
        m_initialized(false),
        // Anti-shrink state (mutable; must be zero-initialised here because
        // AdvertisedWindowSize() is const and cannot reset them on first call
        // without in-constructor initialisation).
        m_ackedBytes(0),
        m_lastAdvWindow(0),
        m_lastReceivedTimestamp(0),
        m_lastReceivedTime(Time(0)),
        // RTT filter state
        m_lastSeenTSecr(0),
        // Retransmission detection
        m_rcvHgh(SequenceNumber32(0)),
        m_tsvHgh(0),
        m_rcvHghValid(false),
        m_lastLossDecreaseTime(Time(0)),

        // Mod 1 – CADF
        m_enableCadf(false),
        m_cadfWindowSize(20),
        m_cadfSpikeRatio(2.5),
        m_cadfSpikeStreak(0),
        m_cadfStreakThreshold(4),
        m_cadfMinAbsoluteSpike(5.0),
        // Mod 2 – ECS
        m_enableEcs(false),
        m_ecsBeta(0.5),
        m_lastEcsDecreaseTime(Time(0)),
        // Mod 3 – ATD
        m_enableAtd(false),
        m_atdAlpha(0.5),
        m_atdMinTarget(MilliSeconds(5)),
        m_ceMarkedThisPacket(false)
{
    NS_LOG_FUNCTION_NOARGS();
}

TcpRledbat::TcpRledbat(const TcpRledbat& sock)
        : TcpSocketBase(sock),
        m_target(sock.m_target),
        m_gain(sock.m_gain),
        m_minWindow(sock.m_minWindow),
        m_baseDelayHistorySize(sock.m_baseDelayHistorySize),
        m_baseDelayRefreshInterval(sock.m_baseDelayRefreshInterval),
        m_wLedbat(sock.m_wLedbat),
        m_maxWindow(sock.m_maxWindow),
        m_currentDelay(sock.m_currentDelay),
        m_baseDelay(sock.m_baseDelay),
        m_baseHistory(sock.m_baseHistory),
        m_currentEpochMin(sock.m_currentEpochMin),
        m_epochStart(sock.m_epochStart),
        m_initialized(sock.m_initialized),
        // Anti-shrink state — copy from parent; the forked socket starts with
        // the parent's last advertised window so first ACK is consistent.
        m_ackedBytes(sock.m_ackedBytes),
        m_lastAdvWindow(sock.m_lastAdvWindow),
        m_lastReceivedTimestamp(sock.m_lastReceivedTimestamp),
        m_lastReceivedTime(sock.m_lastReceivedTime),
        // RTT filter state
        m_lastSeenTSecr(sock.m_lastSeenTSecr),
        m_rttSamples(sock.m_rttSamples),
        // Retransmission detection
        m_rcvHgh(sock.m_rcvHgh),
        m_tsvHgh(sock.m_tsvHgh),
        m_rcvHghValid(sock.m_rcvHghValid),
        m_lastLossDecreaseTime(sock.m_lastLossDecreaseTime),
        // Mod 1 – CADF
        m_enableCadf(sock.m_enableCadf),
        m_cadfWindowSize(sock.m_cadfWindowSize),
        m_cadfSpikeRatio(sock.m_cadfSpikeRatio),
        m_cadfSpikeStreak(sock.m_cadfSpikeStreak),
        m_cadfStreakThreshold(sock.m_cadfStreakThreshold),
        m_cadfMinAbsoluteSpike(sock.m_cadfMinAbsoluteSpike),
        m_cadfHistory(sock.m_cadfHistory),
        // Mod 2 – ECS
        m_enableEcs(sock.m_enableEcs),
        m_ecsBeta(sock.m_ecsBeta),
        m_lastEcsDecreaseTime(sock.m_lastEcsDecreaseTime),
        // Mod 3 – ATD
        m_enableAtd(sock.m_enableAtd),
        m_atdAlpha(sock.m_atdAlpha),
        m_atdMinTarget(sock.m_atdMinTarget),
        m_ceMarkedThisPacket(sock.m_ceMarkedThisPacket)
{
    NS_LOG_FUNCTION_NOARGS();
}

TcpRledbat::~TcpRledbat(){
    NS_LOG_FUNCTION_NOARGS();
}

Ptr<TcpSocketBase>
TcpRledbat::Fork()
{
    NS_LOG_FUNCTION_NOARGS();
    return CopyObject<TcpRledbat>(this);
}

void
TcpRledbat::ForwardUp(Ptr<Packet> packet,
                      Ipv4Header header,
                      uint16_t port,
                      Ptr<Ipv4Interface> incomingInterface)
{
    // Check CE bit in IP header BEFORE base class processes it.
    // Base class will clear/transition m_ecnState after this point.
    if (m_enableEcs)
    {
        uint8_t ecn = header.GetEcn();
        m_ceMarkedThisPacket = (ecn == Ipv4Header::ECN_CE);
        if (m_ceMarkedThisPacket)
        {
            NS_LOG_INFO("ECS: CE-marked packet detected in IP header at t="
                        << Simulator::Now().GetSeconds() << "s");
        }
    }

    TcpSocketBase::ForwardUp(packet, header, port, incomingInterface);
}


Time
TcpRledbat::GetEffectiveTarget() const
{
    if (!m_enableAtd || m_baseDelay == Time::Max())
    {
        // ATD disabled or base not yet measured — use fixed target
        return m_target;
    }

    // effective = max(base_delay * alpha, atdMinTarget)
    double scaledMs = m_baseDelay.GetMilliSeconds() * m_atdAlpha;
    Time scaled = MilliSeconds(static_cast<int64_t>(scaledMs));
    Time effective = (scaled > m_atdMinTarget) ? scaled : m_atdMinTarget;

    NS_LOG_DEBUG("ATD: base=" << m_baseDelay.GetMilliSeconds()
                 << "ms alpha=" << m_atdAlpha
                 << " effective_target=" << effective.GetMilliSeconds() << "ms"
                 << " (fixed_target=" << m_target.GetMilliSeconds() << "ms)");
    return effective;
}

bool
TcpRledbat::IsSpike(double qdelayMs) const
{
    if (m_cadfHistory.size() < 2)
    {
        // Not enough history yet — cannot classify as spike
        return false;
    }

    double sum = 0.0;
    for (double v : m_cadfHistory)
        sum += v;
    double mean = sum / static_cast<double>(m_cadfHistory.size());

    // Both conditions must hold:
    //   1. Ratio test  — proportionally large jump relative to baseline
    //   2. Absolute test — rise must exceed CadfMinAbsoluteSpike (default 5 ms)
    //      Guards against false positives when baseline qdelay is near zero
    //      (e.g. 0.5 ms mean → 1.5 ms sample triggers 2.5x ratio but is only
    //      1 ms of jitter, not a real spike).
    bool ratioExceeded    = (mean > 0.0) && (qdelayMs > m_cadfSpikeRatio * mean);
    bool absoluteExceeded = (qdelayMs - mean) > m_cadfMinAbsoluteSpike;

    if (ratioExceeded && absoluteExceeded)
    {
        NS_LOG_INFO("CADF: spike candidate at t=" << Simulator::Now().GetSeconds()
                    << "s qdelay=" << qdelayMs << "ms mean=" << mean << "ms");
    }
    return  absoluteExceeded && ratioExceeded;
}

void
TcpRledbat::InitializeRledbatState()
{
    NS_LOG_FUNCTION_NOARGS();
    if (m_initialized)
        return;

    m_wLedbat         = m_maxWindow;
    m_initialized     = true;
    m_epochStart      = Simulator::Now();
    m_currentEpochMin = Time::Max();
    NS_LOG_INFO("rLEDBAT initialized with W_ledbat=" << m_wLedbat);
}

void
TcpRledbat::ApplyEcsDecrease()
{
    // Once-per-RTT gate: only apply if at least one RTT has elapsed since
    // the last ECS decrease.
    Time now        = Simulator::Now();
    bool rttElapsed = m_currentDelay.IsZero() ||
                      (now - m_lastEcsDecreaseTime) >= m_currentDelay;
    if (!rttElapsed)
    {
        NS_LOG_DEBUG("ECS: skipping — within one RTT of last decrease");
        return;
    }

    uint32_t oldWindow = m_wLedbat;
    uint32_t decreased = static_cast<uint32_t>(m_wLedbat * m_ecsBeta);
    m_wLedbat             = std::max(decreased, m_minWindow);
    m_tracedWLedbat       = m_wLedbat;
    m_lastEcsDecreaseTime = now;

    NS_LOG_INFO("ECS: ECN-CE W_ledbat " << oldWindow
                << " -> " << m_wLedbat
                << " (beta=" << m_ecsBeta << ")");
}

std::string
TcpRledbat::GetName() const
{
    return "TcpRledbat";
}

Time
TcpRledbat::EstimateDelay(const TcpHeader& tcpHeader)
{
    NS_LOG_FUNCTION_NOARGS();
    Ptr<const TcpOptionTS> tsOption =
        DynamicCast<const TcpOptionTS>(tcpHeader.GetOption(TcpOption::TS));

    if (!tsOption)
    {
        NS_LOG_WARN("No timestamp option found");
        return Time(0);
    }

    // RTT estimation via TSecr (sec. 4.2.1):
    // The sender echoes our outgoing TSval back in TSecr.
    // In ns-3 the TS clock runs in simulation milliseconds, so
    // RTT = Simulator::Now() - MilliSeconds(TSecr).
    uint32_t tSecr = tsOption->GetEcho();
    if (tSecr == 0)
    {
        // No echo yet (first SYN or sender has not seen our TSval)
        NS_LOG_DEBUG("TSecr=0, skipping RTT sample");
        return Time(0);
    }

    // Deduplication (sec. 4.2.1): multiple in-flight packets within one
    // sender clock tick carry the same TSecr.  Use only the first.
    if (tSecr == m_lastSeenTSecr)
    {
        NS_LOG_DEBUG("Duplicate TSecr=" << tSecr << ", skipping");
        return Time(0);
    }
    m_lastSeenTSecr = tSecr;

    Time echoedSendTime = MilliSeconds(tSecr);
    Time now            = Simulator::Now();

    if (now < echoedSendTime)
    {
        NS_LOG_WARN("RTT sample negative (clock wrap?), skipping");
        return Time(0);
    }

    Time rtt = now - echoedSendTime;
    NS_LOG_DEBUG("RTT (via TSecr)=" << rtt.GetMilliSeconds() << "ms");

    m_lastReceivedTimestamp = tSecr;
    m_lastReceivedTime      = now;

    return rtt;
}

void
TcpRledbat::UpdateBaseDelay(Time currentDelay)
{
    NS_LOG_FUNCTION_NOARGS();

    if (currentDelay.IsZero())
        return;

    if (currentDelay < m_currentEpochMin)
        m_currentEpochMin = currentDelay;

    Time now = Simulator::Now();
    if ((now - m_epochStart) >= m_baseDelayRefreshInterval)
    {
        m_baseHistory.push_back(m_currentEpochMin);
        if (m_baseHistory.size() > m_baseDelayHistorySize)
            m_baseHistory.pop_front();

        NS_LOG_INFO("Epoch complete t=" << now.GetSeconds()
                    << "s epoch_min=" << m_currentEpochMin.GetMilliSeconds() << "ms");

        m_currentEpochMin = currentDelay;
        m_epochStart      = now;
    }

    // base delay = minimum across current epoch and all stored epoch minima
    Time minDelay = m_currentEpochMin;
    for (const auto& epochMin : m_baseHistory)
        if (epochMin < minDelay) minDelay = epochMin;

    Time oldDelay = m_baseDelay;
    m_baseDelay        = minDelay;
    m_tracedBaseDelay  = minDelay;

    if (oldDelay != m_baseDelay)
    {
        NS_LOG_DEBUG("Base delay: " << oldDelay.GetMilliSeconds()
                     << "ms -> " << m_baseDelay.GetMilliSeconds() << "ms");
    }
}

Time
TcpRledbat::GetQueueingDelay() const
{
    if (m_baseDelay == Time::Max() || m_currentDelay.IsZero())
        return Time(0);

    Time qdelay = m_currentDelay - m_baseDelay;
    return qdelay.IsNegative() ? Time(0) : qdelay;
}

void
TcpRledbat::UpdateRledbatWindow()
{
    NS_LOG_FUNCTION_NOARGS();

    // Only bail if base delay has not yet been measured.
    // qdelay==0 (currentRTT <= baseRTT) means no queuing → qd < TARGET
    // → offTarget > 0 → window should increase, not stall.
    if (m_baseDelay == Time::Max())
    {
        NS_LOG_DEBUG("Base delay not yet measured, skipping");
        return;
    }

    Time   qdelay    = GetQueueingDelay();
    m_tracedQueuingDelay = qdelay;
    double qdelayMs  = qdelay.GetMilliSeconds();

    // ── Mod 1: CADF ───────────────────────────────────────────────────────
    // Distinguishes transient path-jitter spikes from persistent congestion.
    //
    // streak < threshold → transient: suppress window change, skip history
    // streak >= threshold → persistent congestion: allow window update and
    //   add to history so the mean tracks the new operating point
    // not a spike → reset streak, add to history normally
    if (m_enableCadf)
    {
        if (IsSpike(qdelayMs))
        {
            m_cadfSpikeStreak++;
            if (m_cadfSpikeStreak < m_cadfStreakThreshold)
            {
                NS_LOG_INFO("CADF: transient spike suppressed (streak="
                            << m_cadfSpikeStreak << "/" << m_cadfStreakThreshold
                            << ") qdelay=" << qdelayMs << "ms");
                return;
            }
            NS_LOG_INFO("CADF: persistent congestion (streak="
                        << m_cadfSpikeStreak << ") qdelay=" << qdelayMs
                        << "ms — window update allowed");
        }
        else
        {
            m_cadfSpikeStreak = 0;
        }
        m_cadfHistory.push_back(qdelayMs);
        if (m_cadfHistory.size() > m_cadfWindowSize)
            m_cadfHistory.pop_front();
    }

    // ── Mod 3: ATD ────────────────────────────────────────────────────────
    Time   effectiveTarget = GetEffectiveTarget();
    double targetMs        = effectiveTarget.GetMilliSeconds();

    // ── LEDBAT RFC 6817 window formula ────────────────────────────────────
    // off_target = (TARGET - queuing_delay) / TARGET        [dimensionless]
    // cwnd += GAIN * off_target * bytes_newly_acked * MSS / cwnd
    static const double mss = 1460.0;
    double offTarget   = (targetMs > 0.0) ? ((targetMs - qdelayMs) / targetMs) : 0.0;
    double ackedBytesD = (m_ackedBytes > 0) ? static_cast<double>(m_ackedBytes) : mss;
    double denominator = std::max(static_cast<double>(m_wLedbat), 1.0);

    int32_t windowAdjustment = static_cast<int32_t>(
        m_gain * offTarget * ackedBytesD * mss / denominator);

    NS_LOG_DEBUG("qdelay=" << qdelayMs << "ms target=" << targetMs
                 << "ms offTarget=" << offTarget
                 << " ackedBytes=" << m_ackedBytes
                 << " adj=" << windowAdjustment
                 << (m_enableAtd ? " [ATD]" : ""));

    int64_t newWindow = static_cast<int64_t>(m_wLedbat) + windowAdjustment;
    newWindow = std::max(newWindow, static_cast<int64_t>(m_minWindow));
    newWindow = std::min(newWindow, static_cast<int64_t>(m_maxWindow));

    uint32_t oldWindow = m_wLedbat;
    m_wLedbat           = static_cast<uint32_t>(newWindow);
    m_tracedWLedbat     = m_wLedbat;

    if (oldWindow != m_wLedbat)
    {
        NS_LOG_INFO("W_ledbat: " << oldWindow << " -> " << m_wLedbat
                    << " adj=" << windowAdjustment
                    << " offTarget=" << offTarget
                    << " target=" << targetMs << "ms"
                    << " t=" << Simulator::Now().GetSeconds() << "s");
    }
}

void
TcpRledbat::ReceivedData(Ptr<Packet> packet, const TcpHeader& tcpHeader)
{
    NS_LOG_FUNCTION_NOARGS();
    if (!m_initialized)
        InitializeRledbatState();

    // ── Mod 2: ECS ────────────────────────────────────────────────────────
    if (m_enableEcs && m_ceMarkedThisPacket)
    {
        ApplyEcsDecrease();
        m_ceMarkedThisPacket = false;
    }

    // Accumulate received bytes BEFORE UpdateRledbatWindow() so the LEDBAT
    // formula sees the current packet's contribution (paper pseudocode:
    // ackedBytes += receiveBytes  *before*  IncreaseWindow).
    m_ackedBytes += packet->GetSize();

    // ── RTT sample with min-of-K=4 filter (Appendix A) ───────────────────
    // filteredRTT = min of last K=4 samples (LEDBAT MIN filter, sec. 4.2.1.1)
    Time rawRtt = EstimateDelay(tcpHeader);
    if (!rawRtt.IsZero())
    {
        m_rttSamples.push_back(rawRtt);
        if (m_rttSamples.size() > 4)
            m_rttSamples.pop_front();
        Time filteredRtt = *std::min_element(m_rttSamples.begin(), m_rttSamples.end());

        m_currentDelay = filteredRtt;
        UpdateBaseDelay(filteredRtt);
    }

    // ── Retransmission detection (sec. 4.3) ──────────────────────────────
    // "If SEG.SEQ < RCV.HGH and TSV.SEQ > TSV.HGH → retransmission"
    // TSV.SEQ is the sender's own TSval (GetTimestamp), NOT the echoed TSecr.
    bool lossDetected = false;
    Ptr<const TcpOptionTS> tsOpt =
        DynamicCast<const TcpOptionTS>(tcpHeader.GetOption(TcpOption::TS));
    if (tsOpt)
    {
        SequenceNumber32 segSeq = tcpHeader.GetSequenceNumber();
        uint32_t         tsvSeq = tsOpt->GetTimestamp();   // sender's TSval

        if (!m_rcvHghValid)
        {
            m_rcvHgh      = segSeq;
            m_tsvHgh      = tsvSeq;
            m_rcvHghValid = true;
        }
        else
        {
            if (segSeq < m_rcvHgh && tsvSeq > m_tsvHgh)
            {
                NS_LOG_INFO("Retransmission: SEG.SEQ=" << segSeq
                            << " < RCV.HGH=" << m_rcvHgh
                            << " TSV.SEQ=" << tsvSeq
                            << " > TSV.HGH=" << m_tsvHgh);
                lossDetected = true;
            }
            if (segSeq > m_rcvHgh)
            {
                m_rcvHgh = segSeq;
                m_tsvHgh = tsvSeq;
            }
        }
    }

    // ── Loss-triggered decrease (once per RTT, sec. 4.1.1 / Appendix A) ──
    if (lossDetected)
    {
        Time now        = Simulator::Now();
        bool rttElapsed = m_currentDelay.IsZero() ||
                          (now - m_lastLossDecreaseTime) >= m_currentDelay;
        if (rttElapsed)
        {
            uint32_t old = m_wLedbat;
            m_wLedbat = std::max(static_cast<uint32_t>(m_wLedbat / 2), m_minWindow);
            m_tracedWLedbat        = m_wLedbat;
            m_lastLossDecreaseTime = now;
            NS_LOG_INFO("Loss decrease: W_ledbat " << old << " -> " << m_wLedbat);
        }
    }

    // ── Delay-based window update ─────────────────────────────────────────
    if (!m_currentDelay.IsZero())
        UpdateRledbatWindow();

    TcpSocketBase::ReceivedData(packet, tcpHeader);
}

uint16_t
TcpRledbat::AdvertisedWindowSize(bool scale) const
{
    NS_LOG_FUNCTION_NOARGS();
    uint32_t flowControlWindow = TcpSocketBase::AdvertisedWindowSize(scale);

    if (!m_initialized)
    {
        NS_LOG_DEBUG("Not initialized, using fcwnd=" << flowControlWindow);
        return static_cast<uint16_t>(std::min(flowControlWindow,
                                              static_cast<uint32_t>(0xFFFF)));
    }

    // Desired RLWND: LBE algorithm output, capped by flow control (sec. 4.1)
    // SND.WND = min(cwnd, RLWND, fcwnd)
    uint32_t desired = std::min(m_wLedbat, flowControlWindow);
    if (desired == 0 && flowControlWindow > 0)
        desired = m_minWindow;

    // ── Anti-shrink (sec. 4.1.1 / sendPacket pseudocode) ──────────────────
    // RFC 9293 discourages shrinking the window below what is already in
    // flight.  We may only reduce the advertised window by at most m_ackedBytes
    // (bytes received since the last ACK was sent) per outgoing packet.
    uint32_t advertised;
    if (desired >= m_lastAdvWindow)
    {
        advertised = desired;   // growing or flat — no restriction
    }
    else
    {
        uint32_t floor = (m_lastAdvWindow > m_ackedBytes)
                             ? (m_lastAdvWindow - m_ackedBytes) : 0u;
        // clamp: never go below desired (LBE controller output)
        //        never go above lastAdvWindow (would un-shrink)
        advertised = std::max(desired, floor);
    }

    advertised = std::min(advertised, static_cast<uint32_t>(0xFFFF));

    NS_LOG_DEBUG("AdvertisedWindow: desired=" << desired
                 << " lastAdv=" << m_lastAdvWindow
                 << " ackedBytes=" << m_ackedBytes       // logged BEFORE reset
                 << " -> advertised=" << advertised);

    // Update mutable state for next call
    m_lastAdvWindow = advertised;
    m_ackedBytes    = 0;   // reset: accumulation restarts for the next ACK

    return static_cast<uint16_t>(advertised);
}

} // namespace ns3
