#ifndef NFD_DAEMON_FW_DDOS_HELPER_HPP
#define NFD_DAEMON_FW_DDOS_HELPER_HPP

#include "../../../utils/ndn-ns3-packet-tag.hpp"
#include "../../../utils/ndn-ns3-cc-tag.hpp"
#include "../../../../core/model/ptr.h"
#include "mt-forwarding-info.hpp"
#include "algorithm.hpp"

namespace nfd {
namespace fw {

using ndn::Name;

class DDoSHelper
{
public: // DDoS Related

  // @return the number of pending interests with the same prefix
  // @param the Interest name
  // @param prefix length (component number). Default: FIB entry size or + 2?
  // @note help to calculate the traffic change
  static uint32_t
  getInterestNumberWithPrefix(Forwarder& forwarder,
                              const Name& interestName,
                              const int length)
  {
    Name prefix = interestName.getPrefix(length);
    uint32_t counter = 0;

    const auto& pitTable = forwarder.m_pit;
    for (const auto& entry : pitTable) {
      if (prefix.isPrefixOf(entry.getInterest().getName())) {
        // if the prefix matches the Interest, add the number of inRecords
        counter += entry.getInRecords().size();
      }
    }
    return counter;
  }

  // @return the current PIT table usage rate
  static double
  getCurrentPitTableUsage(Forwarder& forwarder, int maxSize)
  {
    int pitSize = getPitTableSize(forwarder);
    return pitSize/maxSize;
  }

  // @return the current prefix success ratio: suc/all in 1 second
  // @param TODO
  // @param the Interest name
  // @param prefix length (component number). Default: FIB entry size
  static double
  getPrefixSuccessRatio(const Name& interestName,
                        const int length)
  {
    // TODO
    return 0.0;
  }

  // @return whether is under DDoS or not, decided by decision machine
  // @param TODO
  static bool
  isUnderDDoS()
  {
    // TODO
    return false;
  }

public: // congestion related APIs
  static bool
  getCongMark(const ::ndn::TagHost& packet)
  {
    auto tag = packet.getTag<ns3::ndn::Ns3PacketTag>();
    if (tag == nullptr) {
      return false;
    }
    ns3::Ptr<const ns3::Packet> pkt = tag->getPacket();
    ns3::ndn::Ns3CCTag tempTag;
    bool hasTag = pkt->PeekPacketTag(tempTag);
    // std::cout << "Has CC Tag? " << hasTag << "\n";
    shared_ptr<ns3::ndn::Ns3CCTag> ns3ccTag = make_shared<ns3::ndn::Ns3CCTag>();
    if (hasTag) {
      auto it = pkt->GetPacketTagIterator();
      while (it.HasNext()) {
        auto n = it.Next();
        if (n.GetTypeId() == ns3::ndn::Ns3CCTag::GetTypeId()) {
          n.GetTag(*ns3ccTag);
          return ns3ccTag->getCongMark()
            break;
        }
      }
    }
    else {
      return false;
    }
  }

  static bool
  isCongestedByRTT(Forwarder& forwarder, int faceid,
                   std::map<int, double> avgRTTMap,
                   double thresholdRTTInMs = 1000){

    /* Congestion Detection by RTT */
    if (avgRTTMap.find(faceid) == avgRTTMap.end()){
      return false;
    }

    auto avgRTT = avgRTTMap[faceid]; // get average RTT for this face
    if (avgRTT > thresholdRTTInMs){
      return true;
    }
    return false;
  }

  static bool
  isCongestedByCoDel(Forwarder& forwarder, int faceid)
  {
    auto codelQueue = getCodelQueue(forwarder, faceid);

    if (codelQueue != nullptr) {
      // std::cout << "Got CodelQueue!\n";
      // std::cout << "Codel dropping state: " << codelQueue->isInDroppingState() << "\n";
      bool shouildBeMarked = codelQueue->isOkToMark();
      return shouildBeMarked;
    }

    return false;
  }

public: // general APIs

  static uint32_t
  getPitTableSize(Forwarder& forwarder)
  {
    const auto& pitTable = forwarder.m_pit;
    return pitTable.size();
  }


  static ns3::Ptr<ns3::CoDelQueue2>
  getCodelQueue(Forwarder& forwarder, int faceid)
  {
    auto face = forwarder.getFace(faceid);
    auto netDeviceFace = std::dynamic_pointer_cast<ns3::ndn::NetDeviceFace>(face);
    if (netDeviceFace == nullptr || netDeviceFace == 0) {
      return nullptr;
    }
    else {
      ns3::Ptr<ns3::NetDevice> netdevice { netDeviceFace->GetNetDevice() };
      if (netdevice == nullptr) {
        std::cout << "Netdev nullptr!\n";
      }
      auto ptp = ns3::DynamicCast<ns3::PointToPointNetDevice>(netdevice);
      if (ptp == nullptr) {
        std::cout << "Not a PointToPointNetDevice\n";
      }
      else {
        auto q = ptp->GetQueue();
        auto codelQueue = ns3::DynamicCast<ns3::CoDelQueue2>(q);
        return codelQueue;
      }
    }
  }

  static uint32_t
  getTime()
  {
    return ndn::time::steady_clock::now().time_since_epoch().count() / 1000000;
  }

  static MtForwardingInfo*
  getPrefixMeasurements(const fib::Entry& fibEntry, MeasurementsAccessor& measurements)
  {
    measurements::Entry* me = measurements.get(fibEntry);
    if (me == nullptr) {
      std::cout << "Didn't find measurement entry for name: " << fibEntry.getPrefix() << "\n";
      return nullptr;
    }
    return me->getStrategyInfo<MtForwardingInfo>();
  }

  static MtForwardingInfo*
  addPrefixMeasurements(const fib::Entry& fibEntry, MeasurementsAccessor& measurements)
  {
    measurements::Entry* me = measurements.get(fibEntry);
    return std::get<0>(me->insertStrategyInfo<MtForwardingInfo>());
  }
};


} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_HELPER_HPP
