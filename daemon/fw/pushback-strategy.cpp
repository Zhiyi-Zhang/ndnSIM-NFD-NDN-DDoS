#include "pushback-strategy.hpp"

#include "str-helper.hpp"
#include "forwarder.hpp"
#include "src/ndnSIM/NFD/core/common.hpp"

namespace nfd {
namespace fw {

const Name PushbackStrategy::STRATEGY_NAME(
    "ndn:/localhost/nfd/strategy/pushback-strategy/%FD%01");
NFD_REGISTER_STRATEGY(PushbackStrategy);

// Used for logging:
shared_ptr<std::ofstream> PushbackStrategy::m_os;
boost::mutex PushbackStrategy::m_mutex;

PushbackStrategy::PushbackStrategy(Forwarder& forwarder, const Name& name)
    : Strategy(forwarder),
        m_ownForwarder(forwarder),
        m_lastFWRatioUpdate(time::steady_clock::TimePoint::min()),
        m_lastFWWrite(time::steady_clock::TimePoint::min()),
        sharedInfo(make_shared<MtForwardingInfo>()),
        TIME_BETWEEN_FW_UPDATE(time::milliseconds(110)),
        // 20ms between each writing of the forwarding table
        TIME_BETWEEN_FW_WRITE(time::milliseconds(20))
{
  
  // Write header of forwarding percentage table
  if (m_os == nullptr) {
    m_os = make_shared<std::ofstream>();
    m_os->open("results/fwperc.txt");
    *m_os << "Time" << "\tNode" << "\tPrefix" << "\tFaceId" << "\ttype" << "\tvalue"
        << "\n";
  }

  // Start all FIB entries by sending on the shortest path? 
  // If false: start with an equal split.
  INIT_SHORTEST_PATH = StrHelper::getEnvVariable("INIT_SHORTEST_PATH", true);

  // How much the forwarding percentage changes for each received congestion mark
  CHANGE_PER_MARK = StrHelper::getDoubleEnvVariable("CHANGE_PER_MARK", 0.02);

  // How much of the traffic should be used for probing?
  // TODO: skipping probing for now
  //PROBING_PERCENTAGE = StrHelper::getDoubleEnvVariable("PROBING_PERCENTAGE", 0.001);

}

PushbackStrategy::~PushbackStrategy()
{
  m_os->close();
}

const Name&
PushbackStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/pushback/%FD%01");
  return strategyName;
}

void
PushbackStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                       const shared_ptr<pit::Entry>& pitEntry)
{
  // retrieve fib entry
  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
 
  // Retrieving measurement info
  MtForwardingInfo* measurementInfo = StrHelper::getPrefixMeasurements(
      fibEntry, this->getMeasurements());

  // Prefix info not found, create new prefix measurements
  if (measurementInfo == nullptr) {
    measurementInfo = StrHelper::addPrefixMeasurements(fibEntry,
        this->getMeasurements());
    const Name prefixName = fibEntry.getPrefix();
    measurementInfo->setPrefix(prefixName.toUri());
    initializeForwMap(measurementInfo, fibEntry.getNextHops());
  }

  // TODO: Figure out how to use this
  //bool wantNewNonce = false;  

  // PIT entry for incoming Interest already exists.
  if (pitEntry->hasOutRecords()) {

    // Check if request comes from a new incoming face:
    bool requestFromNewFace = false;
    for (const auto& in : pitEntry->getInRecords()) {
      time::steady_clock::Duration lastRenewed = in.getLastRenewed()
          - time::steady_clock::now();
      if (lastRenewed.count() >= 0) {
        requestFromNewFace = true;
      }
    }

    // Received retx from same face. Forward with new nonce.
    if (!requestFromNewFace) {
      // TODO: handle retransmission 
      //wantNewNonce = true;
    }
    // Request from new face. Suppress.
    else {
      std::cout << StrHelper::getTime() << " Node " << m_ownForwarder.getNodeId()
          << " suppressing request from new face: " << inFace.getId() << "!\n";
      return;
    }
  }

  Face* outFace = nullptr;

  // Random number between 0 and 1.
  double r = ((double) rand() / (RAND_MAX));

  double percSum = 0;
  // Add all eligbile faces to list (excludes current downstream)
  std::vector<Face*> eligbleFaces;
  for (const fib::NextHop& n : fibEntry.getNextHops()) {
    if (StrHelper::predicate_NextHop_eligible(inFace, interest, n, pitEntry)) {
      eligbleFaces.push_back(&n.getFace());
      // Add up percentage Sum.
      percSum += measurementInfo->getforwPerc(n.getFace().getId());
    }
  }

  if (eligbleFaces.size() < 1) {
    std::cout << "Blocked interest from face: " << inFace.getId()
        << " (no eligible faces)\n";
    return;
  }

  // If only one face: Send out on it.
  else if (eligbleFaces.size() == 1) {
    outFace = eligbleFaces.front();
  }

  // More than 1 eligible face!
  else {
    // If percSum == 0, there is likely a problem in the routing configuration,
    // e.g. only the downstream has a forwPerc > 0.
    assert(percSum > 0);

    // Write fw percentage to file
    if (time::steady_clock::now() >= m_lastFWWrite + TIME_BETWEEN_FW_WRITE) {
      m_lastFWWrite = time::steady_clock::now();
      writeFwPercMap(m_ownForwarder, measurementInfo);
    }

    // Choose face according to current forwarding percentage:
    double forwPerc = 0;
    for (auto face : eligbleFaces) {
      forwPerc += measurementInfo->getforwPerc(face->getId()) / percSum;
      assert(forwPerc >= 0);
      assert(forwPerc <= 1.1);
      if (r < forwPerc) {
        outFace = face;
        break;
      }
    }
  }

  // Error: No possible outgoing face.
  if (outFace == nullptr) {
    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
        << ", inface " << inFace.getLocalUri() << inFace.getId() << ", interest: "
        << interest.getName() << " outface nullptr\n\n";
  }
  assert(outFace != nullptr);

  // If outgoing face is congested: mark PIT entry as congested. 
  if (StrHelper::isCongested(m_ownForwarder, outFace->getId(), m_avgRTTMap)) {
    pitEntry->m_congMark = true;
    // TODO: Reduce forwarding percentage on outgoing face? 
    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
        << " marking PIT Entry " << pitEntry->getName() << ", inface: " << inFace.getId()
        << ", outface: " << outFace->getId() << "\n";
  }

  /*** Code for RTT Tracking ***/


  /** Record time for interest before it is sent out */

  // this face doesnt exist
  if (m_interestEntryTime.find(outFace->getId()) == m_interestEntryTime.end()){
    m_interestEntryTime[outFace->getId()][interest.getName().toUri()] = time::steady_clock::now();
  } 

  // the face exist but interest doesn't
  else if (m_interestEntryTime[outFace->getId()].find(interest.getName().toUri()) == m_interestEntryTime[outFace->getId()].end()){
    m_interestEntryTime[outFace->getId()][interest.getName().toUri()] = time::steady_clock::now();
  } 
 
  // interest of same name already exists at interface (waiting for data). Do nothing in this case as we will consider "worst" RTT (first interest-data pair)
  else {
    
  }

  this->sendInterest(pitEntry, *outFace, interest);

  // TODO: finish probing
  /*// Probe other faces
  if (r <= PROBING_PERCENTAGE) {
    probeInterests(outFace->getId(), fibEntry.getNextHops(), pitEntry);
  }*/
}

void
PushbackStrategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& inFace, const Data& data)
{

  Name currentPrefix;
  MtForwardingInfo* measurementInfo;
  std::tie(currentPrefix, measurementInfo) = StrHelper::findPrefixMeasurementsLPM(
      *pitEntry, this->getMeasurements());
  if (measurementInfo == nullptr) {
    std::cout << StrHelper::getTime() << "Didn't find measurement entry: "
        << pitEntry->getName() << "\n";
  }
  assert(measurementInfo != nullptr);

  int8_t congMark = StrHelper::getCongMark(data);

  // TODO: Handle NACK
  //int8_t nackType = StrHelper::getNackType(data);

  // If there is more than 1 face and data doesn't come from local content store
  // Then update forwarding ratio
  // TODO: Handle local content store
  if (measurementInfo->getFaceCount() > 1 /*&& !inFace.isLocal()*/
      && inFace.getLocalUri().toString() != "contentstore://") {

    //TODO: Handle NACKs
    /*bool updateBasedOnNACK = false;
    // For marked NACKs: Only adapt fw percentage each Update Interval (default: 110ms)
    if (nackType == NACK_TYPE_MARK
        && (time::steady_clock::now() >= m_lastFWRatioUpdate + TIME_BETWEEN_FW_UPDATE)) {
      m_lastFWRatioUpdate = time::steady_clock::now();
      updateBasedOnNACK = true;
    }*/


    /** Handle RTT Tracking **/
    // assumption: there will be an interest corresponding to data for sure i.e. interest hasnt expired
    // or no unintended data received
    
    time::steady_clock::Duration rtt = time::steady_clock::now() - m_interestEntryTime[inFace.getId()][data.getName().toUri()];
    
    // face id not in map
    if (m_rttFaceMap.find(inFace.getId()) == m_rttFaceMap.end()){
      m_rttFaceMap[inFace.getId()] = time::duration_cast<time::milliseconds>(rtt).count();
      m_rttCountFaceMap[inFace.getId()] = 1;
    } else {
      m_rttFaceMap[inFace.getId()] += time::duration_cast<time::milliseconds>(rtt).count();
      m_rttCountFaceMap[inFace.getId()] += 1;
    }

    // we now have lastNRTT entries so we take average
    if (m_lastNRTT == m_rttCountFaceMap[inFace.getId()]){
      m_avgRTTMap[inFace.getId()] = m_rttFaceMap[inFace.getId()]/m_lastNRTT;

      // reset RTT counts and values for this face
      m_rttFaceMap[inFace.getId()] = 0;
      m_rttCountFaceMap[inFace.getId()] = 0;
    }

    // If Data congestion marked or NACK updates ratio:
    if (congMark){ //|| updateBasedOnNACK) {

      double fwPerc = measurementInfo->getforwPerc(inFace.getId());
      double change_perc = CHANGE_PER_MARK * fwPerc;

      StrHelper::reduceFwPerc(measurementInfo, inFace.getId(), change_perc);
      writeFwPercMap(m_ownForwarder, measurementInfo);
    }
  }

  bool pitMarkedCongested = false;
  if (pitEntry->m_congMark == true) {
    std::cout << StrHelper::getTime() << " node " << m_ownForwarder.getNodeId()
        << " found marked PIT Entry: " << pitEntry->getName() << ", face: "
        << inFace.getId() << "!\n";
    pitMarkedCongested = true;
  }

  for (const auto& n : pitEntry->getInRecords()) {
    bool downStreamCongested = StrHelper::isCongested(m_ownForwarder,
        n.getFace().getId(), m_avgRTTMap);

    int8_t markSentPacket = std::max(
        std::max((int8_t) congMark, (int8_t) downStreamCongested),
        (int8_t) pitMarkedCongested);

    ndn::CongestionTag tag3 = ndn::CongestionTag(false);
    //    tag3.setCongMark(markSentPacket);
    assert(tag3.getCongMark() == markSentPacket);

    data.setTag(make_shared<ndn::CongestionTag>(tag3));
  }

}

void
PushbackStrategy::beforeExpirePendingInterest(std::shared_ptr<nfd::pit::Entry> const&)
{

}

void
PushbackStrategy::initializeForwMap(MtForwardingInfo* measurementInfo,
    std::vector<fib::NextHop> nexthops)
{
  int lowestId = std::numeric_limits<int>::max();

  int localFaceCount = 0;
  for (auto n : nexthops) {

    // TODO: Handle local face count
    //if (n.getFace().isLocal()) {
    //  localFaceCount++;
    //}
    if (n.getFace().getId() < (unsigned) lowestId) {
      lowestId = n.getFace().getId();
    }
  }

  std::cout << StrHelper::getTime() << " Init FW node " << m_ownForwarder.getNodeId()
      << ": ";
  for (auto n : nexthops) {
    double perc = 0.0;
    if (localFaceCount > 0 || INIT_SHORTEST_PATH) {
      if (n.getFace().getId() == (unsigned) lowestId) {
        perc = 1.0;
      }
    }
    // Split forwarding percentage equally.
    else {
      perc = 1.0 / (double) nexthops.size();
    }
    std::cout << "face " << n.getFace().getLocalUri() << n.getFace().getId();
    std::cout << "=" << perc << ", ";
    measurementInfo->setforwPerc(n.getFace().getId(), perc);
  }
  std::cout << "\n";

  writeFwPercMap(m_ownForwarder, measurementInfo);
}


void
PushbackStrategy::writeFwPercMap(Forwarder& ownForwarder,
    MtForwardingInfo* measurementInfo)
{
  boost::mutex::scoped_lock scoped_lock(m_mutex);
  std::string prefix = measurementInfo->getPrefix();

  for (auto f : measurementInfo->getForwPercMap()) {
    StrHelper::printFwPerc(m_os, ownForwarder.getNodeId(), prefix, f.first, "forwperc",
        f.second);
  }
  m_os->flush();
}  

}
}
