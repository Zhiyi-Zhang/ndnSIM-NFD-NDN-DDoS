#ifndef NFD_DAEMON_FW_STRATEGY_HELPER_HPP
#define NFD_DAEMON_FW_STRATEGY_HELPER_HPP

//#include "../../common.hpp"
#include "../../../utils/ndn-ns3-packet-tag.hpp"
#include "../../../utils/ndn-ns3-cc-tag.hpp"
#include "../../../../core/model/ptr.h"
#include "mt-forwarding-info.hpp"
#include "algorithm.hpp"

namespace nfd {
namespace fw {

using ndn::Name; 


class StrHelper
{

public:

  static void
  reduceFwPerc(MtForwardingInfo* forwInfo,
      const FaceId reducedFaceId,
      const double change)
  {

    if (forwInfo->getFaceCount() == 1) {
      std::cout << "Trying to update fw perc of single face!\n";
      return;
    }

    // Reduction is at most the current forwarding percentage of the face that is reduced.
    double changeRate = 0 - std::min(change, forwInfo->getforwPerc(reducedFaceId));

    // Decrease fw percentage of the given face:
    forwInfo->increaseforwPerc(reducedFaceId, changeRate);
    double sumFWPerc = 0;
    sumFWPerc += forwInfo->getforwPerc(reducedFaceId);
    const auto forwMap = forwInfo->getForwPercMap();

//		std::cout << "\n";
    for (auto f : forwMap) {
      auto tempChangeRate = changeRate;
      auto &faceId = f.first;
      if (faceId == reducedFaceId) { // Do nothing. Percentage has already been added.
      }
      else {
        // Increase forwarding percentage of all other faces by and equal amount.
        tempChangeRate = std::abs(changeRate / (double) (forwMap.size() - 1));
        forwInfo->increaseforwPerc((faceId), tempChangeRate);
        sumFWPerc += forwInfo->getforwPerc(faceId);
      }
    }

    if (sumFWPerc < 0.999 || sumFWPerc > 1.001) {
      std::cout << StrHelper::getTime() << "ERROR! Sum of fw perc out of range: " << sumFWPerc
          << "\n";
    }
  }

  /** \brief determines whether a NextHop is eligible
   */
  static bool
  predicate_NextHop_eligible(const Face& inFace, const Interest& interest,
                  const fib::NextHop& nexthop,
                  const shared_ptr<pit::Entry>& pitEntry,
                  bool wantUnused = false,
                  time::steady_clock::TimePoint now = time::steady_clock::TimePoint::min())
  {

    const Face& outFace = nexthop.getFace();

    // do not forward back to the same face, unless it is ad hoc
    if (outFace.getId() == inFace.getId() && outFace.getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC)
      return false;

    // forwarding would violate scope
    if (wouldViolateScope(inFace, interest, outFace))
      return false;

    if (wantUnused) {
      // NextHop must not have unexpired OutRecord
      pit::OutRecordCollection::const_iterator outRecord = pitEntry->getOutRecord(outFace);
      if (outRecord != pitEntry->getOutRecords().end() && outRecord->getExpiry() > now) {
        return false;
      }
    }

    return true;
  }

  static bool
  getCongMark(const ::ndn::TagHost& packet)
  { 
    shared_ptr<ns3::ndn::Ns3CCTag> tag = getCCTag(packet);
    if (tag != nullptr) {
      return tag->getCongMark();
    }
    else {
      return false;
    }
  }  

  static shared_ptr<ns3::ndn::Ns3CCTag>
  getCCTag(const ::ndn::TagHost& packet)
  {
    auto tag = packet.getTag<ns3::ndn::Ns3PacketTag>();
    if (tag != nullptr) {
      ns3::Ptr<const ns3::Packet> pkt = tag->getPacket();
      ns3::ndn::Ns3CCTag tempTag;  
      bool hasTag = pkt->PeekPacketTag(tempTag);
//      std::cout << "Has CC Tag? " << hasTag << "\n";
      shared_ptr<ns3::ndn::Ns3CCTag> ns3ccTag = make_shared<ns3::ndn::Ns3CCTag>();
      if (hasTag) {
        auto it = pkt->GetPacketTagIterator();
        int i = 0;
        while (it.HasNext()) {
          i++;
          auto n = it.Next();
          if (n.GetTypeId() == ns3::ndn::Ns3CCTag::GetTypeId()) {
            n.GetTag(*ns3ccTag);
            break;
          }
        }
        return ns3ccTag;
      }
      else {
        return nullptr;
      }
    }
    return nullptr;
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

  static std::tuple<Name, MtForwardingInfo*>
  findPrefixMeasurementsLPM(const pit::Entry& pitEntry, MeasurementsAccessor& measurements)
  {
    
//    shared_ptr<measurements::Entry> me = measurements.get(name);
//    shared_ptr<measurements::Entry> me = measurements.findLongestPrefixMatch(name);
    measurements::Entry* me = measurements.findLongestPrefixMatch(pitEntry);
    if (me == nullptr) {
      std::cout << "Name " << pitEntry.getName().toUri() << " not found!\n";
      return std::forward_as_tuple(Name(), nullptr); 
    }
    MtForwardingInfo* mi = me->getStrategyInfo<MtForwardingInfo>();
    assert(mi != nullptr);
    return std::forward_as_tuple(me->getName(), mi);
  }

  static MtForwardingInfo*
  addPrefixMeasurementsLPM(const Name& name, MeasurementsAccessor& measurements)
  {
    measurements::Entry* me;
    // Save info one step up.
    me = measurements.get(name);
    // parent of Interest Name is not in this strategy, or Interest Name is empty
    return std::get<0>(me->insertStrategyInfo<MtForwardingInfo>());
  }

  static bool 
  isCongested(Forwarder& forwarder,
      int faceid,
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

  static std::string
  getTime()
  {
    std::ostringstream oss;
    oss << ndn::time::steady_clock::now().time_since_epoch().count() / 1000000 << " ms";
    return oss.str();
  }

  // Print table: time, node, faceid, type, fwPerc
  static void 
  printFwPerc(shared_ptr<std::ostream> os,
      uint32_t nodeId,
      std::string prefix,
      FaceId faceId,
      std::string type,
      double fwPerc)
  {
    *os << ns3::Simulator::Now().ToDouble(ns3::Time::S) << "\t" << nodeId << "\t" << prefix << "\t"
        << faceId << "\t" << type << "\t" << fwPerc << "\n";
//		os->flush();
  }

  static double
  getDoubleEnvVariable(std::string name, double defaultValue)
  {
    std::string tmp = StrHelper::getEnvVar(name);
    if (!tmp.empty()) {
      return std::stod(tmp);
    }
    else {
      return defaultValue;
    }
  }

  static bool
  getEnvVariable(std::string name, bool defaultValue)
  {
    std::string tmp = StrHelper::getEnvVar(name);
    if (!tmp.empty()) {
      return (tmp == "TRUE" || tmp == "true");
    }
    else {
      return defaultValue;
    }
  }

  static int
  getEnvVariable(std::string name, int defaultValue)
  {
    std::string tmp = StrHelper::getEnvVar(name);
    if (!tmp.empty()) {
      return std::stoi(tmp);
    }
    else {
      return defaultValue;
    }
  }

private:
 
  static std::string
  getEnvVar(std::string const& key)
  {
    char const* val = getenv(key.c_str());
    return val == NULL ? std::string() : std::string(val);
  }

  static void
  printFIB(Fib& fib)
  { 
    Fib::const_iterator it = fib.begin();
    while (it != fib.end()) {
      std::cout << it->getPrefix() << ": ";
      for (auto b : it->getNextHops()) {
        std::cout << b.getFace().getLocalUri() << b.getFace().getId() << ", local:" << ", ";
      }
      std::cout << "\n";
      it++;
    }
  }

  static void
  printFIBEntry(const shared_ptr<fib::Entry> entry)
  {
    for (auto f : entry->getNextHops()) {
      std::cout << f.getFace().getLocalUri() << f.getFace().getId() << ", ";
    }
    std::cout << "\n";
  } 

};
}
}

#endif
