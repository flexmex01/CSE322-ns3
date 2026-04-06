#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// Required for real-time simulation
#include "ns3/realtime-simulator-impl.h"

#include <cstdio>
#include <cstdint>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("FirstScriptExample");

static const uint32_t DLT_PPP = 9;
FILE* g_pcapFile = nullptr;

void
WritePcapGlobalHeader(FILE* f)
{
    uint32_t magic        = 0xa1b2c3d4;
    uint16_t versionMajor = 2;
    uint16_t versionMinor = 4;
    int32_t  thisZone     = 0;
    uint32_t sigFigs      = 0;
    uint32_t snapLen      = 65535;
    uint32_t network      = DLT_PPP;

    fwrite(&magic,        4, 1, f);
    fwrite(&versionMajor, 2, 1, f);
    fwrite(&versionMinor, 2, 1, f);
    fwrite(&thisZone,     4, 1, f);
    fwrite(&sigFigs,      4, 1, f);
    fwrite(&snapLen,      4, 1, f);
    fwrite(&network,      4, 1, f);
    fflush(f);
}

void
RealTimePcapSink(Ptr<const Packet> packet)
{
    if (!g_pcapFile)
        return;

    Time now      = Simulator::Now();
    uint32_t sec  = static_cast<uint32_t>(now.GetSeconds());
    uint32_t usec = static_cast<uint32_t>(now.GetMicroSeconds() % 1000000);
    uint32_t pktLen = packet->GetSize();

    fwrite(&sec,    4, 1, g_pcapFile);
    fwrite(&usec,   4, 1, g_pcapFile);
    fwrite(&pktLen, 4, 1, g_pcapFile);
    fwrite(&pktLen, 4, 1, g_pcapFile);

    uint8_t* buf = new uint8_t[pktLen];
    packet->CopyData(buf, pktLen);
    fwrite(buf, 1, pktLen, g_pcapFile);
    delete[] buf;

    fflush(g_pcapFile);

    // Optional: print to stdout so you can see events happening in real time
    NS_LOG_UNCOND("Packet captured at t=" << now.GetSeconds()
                  << "s  size=" << pktLen << " bytes"
                  << "  file_pos=" << ftell(g_pcapFile) << " bytes");
}

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.Parse(argc, argv);

    Time::SetResolution(Time::NS);
    LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
    LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);

    // -----------------------------------------------------------------------
    // KEY: Switch to real-time simulator so simulation paces with wall clock
    // -----------------------------------------------------------------------
    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::RealtimeSimulatorImpl"));
    // "BestEffort" means: keep up with real time, don't abort if it falls behind
    Config::SetDefault("ns3::RealtimeSimulatorImpl::SynchronizationMode",
                       StringValue("BestEffort"));

    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("2ms"));

    NetDeviceContainer devices;
    devices = pointToPoint.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // Open PCAP file
    const char* pcapPath = "/scratch/realtime-first.pcap";
    g_pcapFile = fopen(pcapPath, "wb");
    if (!g_pcapFile)
    {
        NS_FATAL_ERROR("Cannot open PCAP file: " << pcapPath);
    }
    WritePcapGlobalHeader(g_pcapFile);
    NS_LOG_UNCOND("PCAP file opened. Initial size: " << ftell(g_pcapFile) << " bytes");

    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        devices.Get(i)->TraceConnectWithoutContext("PromiscSniffer",
                                                    MakeCallback(&RealTimePcapSink));
    }

    UdpEchoServerHelper echoServer(9);
    ApplicationContainer serverApps = echoServer.Install(nodes.Get(1));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(25.0));

    UdpEchoClientHelper echoClient(interfaces.GetAddress(1), 9);
    // Send 20 packets, one per second — gives you 20 seconds to watch the file grow
    echoClient.SetAttribute("MaxPackets", UintegerValue(20));
    echoClient.SetAttribute("Interval",   TimeValue(Seconds(1.0)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(nodes.Get(0));
    clientApps.Start(Seconds(2.0));
    clientApps.Stop(Seconds(23.0));

    Simulator::Stop(Seconds(23.0));  // slightly after last packet at t=22s
    Simulator::Run();
    Simulator::Destroy();

    fclose(g_pcapFile);
    NS_LOG_UNCOND("Simulation done.");
    return 0;
}