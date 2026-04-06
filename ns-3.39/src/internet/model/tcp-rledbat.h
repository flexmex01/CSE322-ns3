

#ifndef TCP_RLEDBAT_H
#define TCP_RLEDBAT_H


#include "ns3/tcp-socket-base.h"
#include "ns3/traced-value.h"
#include "ns3/tcp-option-ts.h"
#include  <deque>
#include "tcp-congestion-ops.h"



namespace ns3{

    
class TcpRledbat : public TcpSocketBase
{
    public:
        static TypeId GetTypeId();
        TypeId GetInstanceTypeId() const override;
        TcpRledbat();
        TcpRledbat(const TcpRledbat& sock);
        ~TcpRledbat() override;
        Ptr <TcpSocketBase> Fork () override;
        std::string GetName() const;
        void ForwardUp(Ptr<Packet> packet,
                         Ipv4Header header,
                         uint16_t port,
                         Ptr<Ipv4Interface> incomingInterface) override;

    protected:
        uint16_t AdvertisedWindowSize(bool scale = true)const  override;
        void ReceivedData(Ptr<Packet> packet, const TcpHeader& tcpHeader) override;
        Time EstimateDelay(const TcpHeader& tcpHeader);
        void UpdateBaseDelay(Time currentDelay);
        Time GetQueueingDelay() const;
        void UpdateRledbatWindow();
        void InitializeRledbatState();
        // ── New methods ───────────────────────────────────────────────────────────
        Time GetEffectiveTarget() const;    // Mod 3
        bool IsSpike(double qdelayMs) const; // Mod 1
        void ApplyEcsDecrease();             // Mod 2

    
    private:

        Time m_target;
        double m_gain;
        uint32_t m_minWindow;
        uint32_t m_baseDelayHistorySize;
        Time m_baseDelayRefreshInterval;

        uint32_t m_wLedbat;
        uint32_t m_maxWindow;
        Time m_currentDelay;
        Time m_baseDelay;
        std::deque<Time> m_baseHistory;
        Time m_currentEpochMin;   
        Time m_epochStart; 
        bool m_initialized;
        mutable uint32_t m_ackedBytes;        // bytes received since last send, for anti-shrink
        mutable uint32_t m_lastAdvWindow;     // last window we actually advertised

        uint32_t m_lastReceivedTimestamp;
        Time m_lastReceivedTime;

        uint32_t            m_lastSeenTSecr;         // TSecr dedup
        std::deque<Time>    m_rttSamples;            // min-of-K=4 filter
        SequenceNumber32    m_rcvHgh;               // highest seq# received
        uint32_t            m_tsvHgh;               // sender TSval at RCV.HGH
        bool                m_rcvHghValid;          // first-packet guard
        Time                m_lastLossDecreaseTime; // once-per-RTT gate for loss

        TracedValue<uint32_t> m_tracedWLedbat;
        TracedValue<Time> m_tracedQueuingDelay;
        TracedValue<Time> m_tracedBaseDelay;

        // ── Mod 1: CADF ──────────────────────────────────────────────────────────
        bool              m_enableCadf;
        uint32_t          m_cadfWindowSize;
        double            m_cadfSpikeRatio;
        uint32_t            m_cadfSpikeStreak;
        uint32_t           m_cadfStreakThreshold;
        double          m_cadfMinAbsoluteSpike;
        std::deque<double> m_cadfHistory;   // recent qdelay samples in ms

        // ── Mod 2: ECS ───────────────────────────────────────────────────────────
        bool     m_enableEcs;
        double   m_ecsBeta;
        Time     m_lastEcsDecreaseTime;  // once-per-RTT gate for ECS

        // ── Mod 3: ATD ───────────────────────────────────────────────────────────
        bool     m_enableAtd;
        double   m_atdAlpha;
        Time     m_atdMinTarget;
        // Latches true when a CE-marked packet arrives, cleared after ECS applied
        bool m_ceMarkedThisPacket;


};
}

#endif