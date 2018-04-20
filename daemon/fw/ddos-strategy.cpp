#include "ddos-strategy.hpp"
#include "ddos-helper.hpp"
#include "core/logger.hpp"
#include "ns3/simulator.h"
#include <boost/random/uniform_int_distribution.hpp>
#include <ndn-cxx/lp/tags.hpp>

namespace nfd {
namespace fw {

NFD_LOG_INIT("DDoSStrategy");
NFD_REGISTER_STRATEGY(DDoSStrategy);

const static int MIN_ADDITIVE_INCREASE_STEP = 3;
const static int DEFAULT_ADDITION_TIMER = 3;
const static int DEFAULT_REVERT_TIME_COUNTER = 3;

DDoSStrategy::DDoSStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
  , m_forwarder(forwarder)
  , m_state(DDoS_NORMAL)
  , m_timer(0.1)
{
  this->setInstanceName(makeInstanceName(name, getStrategyName()));
}

DDoSStrategy::~DDoSStrategy()
{
}

void
DDoSStrategy::scheduleRevertStateEvent(ns3::Time t)
{
  NFD_LOG_TRACE("Scheduling revert state event");
  if (!m_revertStateEvent.IsRunning()) {
    m_revertStateEvent = ns3::Simulator::Schedule(t, // ns3::Seconds(m_timer),
                                        &DDoSStrategy::revertState, this);
  }
}

void
DDoSStrategy::revertState()
{
  NFD_LOG_TRACE("Reverting state");

  if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {
    applyForwardWithRateLimit();
  }

  std::list<Name> toBeDelete;
  for (auto& recordEntry : m_ddosRecords) {
    auto& record = recordEntry.second;

    // if received NACK is less than m_timer ago, continue
    if (ns3::Simulator::Now() - record->m_lastNackTimestamp < ns3::Seconds(m_timer)) {
      continue;
    }

    if (record->m_revertTimerCounter > 0) {
      record->m_revertTimerCounter -= m_timer;
    }

    if (record->m_revertTimerCounter <= 0) {

      if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {

        // init record->m_isGoodConsumer
        if (record->m_additiveIncreaseCounter == 0) {
          for (const auto& perFacePushBackEntry : record->m_pushbackWeight) {
            record->m_isGoodConsumer[perFacePushBackEntry.first] = true;
          }
        }

        // apply additive increase to the tolerance after nack's limit timer
        record->m_fakeInterestTolerance += record->m_additiveIncreaseStep;
        record->m_additiveIncreaseCounter += m_timer;

        NFD_LOG_DEBUG("Additive increase, now tolerance is " << record->m_fakeInterestTolerance);

        if (record->m_additiveIncreaseCounter == DEFAULT_ADDITION_TIMER + 1) {
          std::list<FaceId> toRemoveLimit;
          for (const auto& perFacePushBackEntry : record->m_pushbackWeight) {
            if (record->m_isGoodConsumer[perFacePushBackEntry.first]) {
              toRemoveLimit.push_back(perFacePushBackEntry.first);
            }
          }
          for (const auto& faceId : toRemoveLimit) {
            record->m_pushbackWeight.erase(faceId);
            NFD_LOG_DEBUG("Remove pushback weight record: " << faceId);
          }
          if (record->m_pushbackWeight.size() == 0) {
            toBeDelete.push_back(recordEntry.first);
          }
          else {
            record->m_additiveIncreaseCounter = 0;
          }
        }
      }
      else {
        toBeDelete.push_back(recordEntry.first);
      }
    }
  }

  // delete DDoS records that can be deleted
  for (const auto& name : toBeDelete) {
    m_ddosRecords.erase(name);
    NFD_LOG_DEBUG("Remove DDoS record: " << name);
  }

  // if no DDoS records in the list, change state to normal
  if (m_ddosRecords.size() == 0) {
    m_state = DDoS_NORMAL;
    NFD_LOG_DEBUG("Changed state to DDoS_NORMAL");
  }
  else {
    scheduleRevertStateEvent(ns3::Seconds(m_timer));
  }
}

void
DDoSStrategy::applyForwardWithRateLimit()
{
  NFD_LOG_TRACE("Applying forwarding with rate limit");

  // for each DDoS record
  for (auto& recordEntry : m_ddosRecords) {
    auto& record = recordEntry.second;
    NFD_LOG_INFO("DDOS RECORD LOOP :" << recordEntry.first);

    // for each face that has Interest buffer under the prefix
    for (auto& perFaceBufInterest : recordEntry.second->m_perFaceInterestBuffer) {

      auto& faceId = perFaceBufInterest.first;
      NFD_LOG_INFO("perFaceBufInterest LOOP " << faceId);
      int limit = 10000;

      auto interfaceWeightEntry = record->m_pushbackWeight.find(faceId);
      if (interfaceWeightEntry != record->m_pushbackWeight.end()) {
        // calculate the current rate limit of the face
        double limitDouble = interfaceWeightEntry->second * record->m_fakeInterestTolerance * m_timer;
        std::cout << limitDouble << std::endl;
        if (limitDouble > 1) {
          limit = static_cast<int>(limitDouble + 0.5);
        }
        else if (limitDouble > 0.5) {
          limit = 1;
        }
        else if (limitDouble < 0.5) {
          limit = 0;
        }
        else {
          limit = rand() % 2;
        }
        NFD_LOG_INFO("The weight is " << interfaceWeightEntry->second);
        NFD_LOG_INFO("The new limit on the face is " << limit);

        if (perFaceBufInterest.second.size() > limit) {
          record->m_isGoodConsumer[faceId] = false;
        }
      }
      else {
        // there is no more limit on the current face
        NFD_LOG_INFO("No more rate limit on the face");
      }

      for (int i = 0; i != limit; ++i) {
        if (perFaceBufInterest.second.size() > (unsigned) i) {
          auto innerIt = perFaceBufInterest.second.begin();
          std::advance(innerIt, i);

          auto interest = std::make_shared<ndn::Interest>(*innerIt);
          shared_ptr<pit::Entry> pitEntry = m_forwarder.m_pit.find(*interest);
          this->doLoadBalancing(*getFace(faceId), *interest, pitEntry);
          NFD_LOG_INFO("After loop, we sent out Interest " << interest->getName());
        }
      }
    }
    record->m_perFaceInterestBuffer.clear();
  }
}

void
DDoSStrategy::handleFakeInterestNack(const Face& inFace, const lp::Nack& nack,
                                     const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("Handle Fake Nack");
  NFD_LOG_TRACE("Node ID " << m_forwarder.getNodeId());
  NFD_LOG_TRACE("Nack tolerance " << nack.getHeader().m_tolerance);
  NFD_LOG_TRACE("Nack fake name list size " << nack.getHeader().m_fakeInterestNames.size());

  ns3::Time t1 = ns3::Seconds(m_timer);
  if (m_state == DDoS_ATTACK) {
    t1 = ns3::Simulator::GetDelayLeft(m_revertStateEvent);
    ns3::Simulator::Cancel(m_revertStateEvent);
    NFD_LOG_TRACE("Cancel revert event " << t1);
  }

  Name prefix = nack.getInterest().getName().getPrefix(nack.getHeader().m_prefixLen);
  auto& pitTable = m_forwarder.m_pit;

  // to record the Pit Entry to be removed
  std::list<shared_ptr<pit::Entry>> deleteList;

  // to record per face Interest list to be nacked
  std::map<FaceId, std::list<Name>> perFaceList;

  // to keep the corresponding DDoS record
  shared_ptr<DDoSRecord> record = insertOrUpdateRecord(nack);
  if (record == nullptr) {
    return;
  }

  // calculate DDoS record per face pushback weight
  const auto& nackNameList = nack.getHeader().m_fakeInterestNames;
  double denominator = nackNameList.size();
  for (const auto& nackName : nackNameList) { // iterate all fake interest names
    Interest interest(nackName);

    // find corresponding PIT Entry
    auto entry = pitTable.find(interest);
    if (entry != nullptr) {

      // iterate its incoming Faces and calculate pushback weight
      const auto& inRecords = entry->getInRecords();
      int inFaceNumber = inRecords.size();
      // std::cout << "inFaceNumber : " << inFaceNumber << std::endl;
      for (const auto& inRecord: inRecords) {
        FaceId faceId = inRecord.getFace().getId();
        auto innerSearch = record->m_pushbackWeight.find(faceId);
        if (innerSearch == record->m_pushbackWeight.end()) {
          record->m_pushbackWeight[faceId] = 1 / ( denominator * inFaceNumber);
        }
        else {
          record->m_pushbackWeight[faceId] += 1 / ( denominator * inFaceNumber);
        }
        perFaceList[faceId].push_back(nackName);
      }
      deleteList.push_back(entry);
    }
    else {
      continue;
    }
  }

  std::cout << "node id :" << m_forwarder.getNodeId() << std::endl
            << "receiving tolerance" << nack.getHeader().m_tolerance << std::endl;
  // pushback nacks to Interest Upstreams
  for (auto it = record->m_pushbackWeight.begin();
       it != record->m_pushbackWeight.end(); ++it) {

    lp::NackHeader newNackHeader;
    newNackHeader.m_reason = nack.getHeader().m_reason;
    newNackHeader.m_prefixLen = nack.getHeader().m_prefixLen;
    newNackHeader.m_nackId = nack.getHeader().m_nackId;
    int newTolerance = static_cast<uint64_t>(nack.getHeader().m_tolerance * it->second + 0.5);
    if (newTolerance < 1) {
      // TODO
    }
    newNackHeader.m_tolerance = newTolerance;
    newNackHeader.m_fakeInterestNames = perFaceList[it->first];

    std::cout << "\t face id: " << it->first
              << "\t weight" << it->second
              << "\t weighted tolerance: " << newTolerance << std::endl;

    Interest interest(perFaceList[it->first].front());
    auto entry = pitTable.find(interest);
    ndn::lp::Nack newNack(entry->getInterest());
    newNack.setHeader(newNackHeader);
    m_forwarder.sendDDoSNack(*getFace(it->first), newNack);

    NFD_LOG_TRACE("SendDDoSNack to downstream face " << it->first);
    NFD_LOG_TRACE("New Nack tolerance " << newNackHeader.m_tolerance);
    NFD_LOG_TRACE("New Nack fake name list " << newNackHeader.m_fakeInterestNames.size());
  }

  // delete the nacked PIT entries
  for (auto toBeDelete : deleteList) {
    m_forwarder.ddoSRemovePIT(toBeDelete);
  }

  if (m_state != DDoS_ATTACK) {
    m_state = DDoS_ATTACK;
    scheduleRevertStateEvent(ns3::Seconds(m_timer));
    NFD_LOG_DEBUG("Changed state to DDoS_ATTACK");
  }
  else {
    scheduleRevertStateEvent(t1);
    NFD_LOG_TRACE("Re-schedule the revert event " << t1);
  }
}

void
DDoSStrategy::handleValidInterestNack(const Face& inFace, const lp::Nack& nack,
                                      const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("Handle Valid Interest Nack");
  NFD_LOG_TRACE("Node ID " << m_forwarder.getNodeId());
  NFD_LOG_TRACE("Valid capacity " << nack.getHeader().m_tolerance);
  NFD_LOG_TRACE("Nack name list size (should be 0) " << nack.getHeader().m_fakeInterestNames.size());

  Name prefix = nack.getInterest().getName().getPrefix(nack.getHeader().m_prefixLen);

  // get PIT table
  auto& pitTable = m_forwarder.m_pit;
  NFD_LOG_TRACE("Current PIT Table size: " << pitTable.size());

  // to record per face Interest list to be nacked
  std::map<FaceId, Name> perFaceList;

  // to keep the corresponding DDoS record
  shared_ptr<DDoSRecord> record = insertOrUpdateRecord(nack);
  if (record == nullptr) {
    return;
  }

  int totalMatchingInterestNumber = 0;
  // calculate DDoS record per face pushback weight
  for (auto& pitEntry : pitTable) { // iterate all fake interest names

    if (prefix.isPrefixOf(pitEntry.getInterest().getName())) {
      // iterate its incoming Faces and calculate pushback weight
      const auto& inRecords = pitEntry.getInRecords();
      int inFaceNumber = inRecords.size();
      if (inFaceNumber > 0) {
        ++totalMatchingInterestNumber;
        for (const auto& inRecord: inRecords) {
          FaceId faceId = inRecord.getFace().getId();
          auto innerSearch = record->m_pushbackWeight.find(faceId);
          if (innerSearch == record->m_pushbackWeight.end()) {
            record->m_pushbackWeight[faceId] = 1 / inFaceNumber;
          }
          else {
            record->m_pushbackWeight[faceId] += 1 / inFaceNumber;
          }
          perFaceList[faceId] = pitEntry.getInterest().getName();
        }
      }
    }
    else {
      continue;
    }
  }

  std::cout << "node id :" << m_forwarder.getNodeId() << std::endl
            << "receiving tolerance" << nack.getHeader().m_tolerance << std::endl;
  // pushback nacks to Interest Upstreams
  for (auto it = record->m_pushbackWeight.begin();
       it != record->m_pushbackWeight.end(); ++it) {

    lp::NackHeader newNackHeader;
    newNackHeader.m_reason = nack.getHeader().m_reason;
    newNackHeader.m_prefixLen = nack.getHeader().m_prefixLen;
    newNackHeader.m_nackId = nack.getHeader().m_nackId;
    it->second = it->second / totalMatchingInterestNumber;
    int newTolerance = static_cast<uint64_t>(nack.getHeader().m_tolerance * it->second + 0.5);
    if (newTolerance < 1) {
      // TODO
    }
    newNackHeader.m_tolerance = newTolerance;
    newNackHeader.m_fakeInterestNames = nack.getHeader().m_fakeInterestNames;


    std::cout << "\t face id: " << it->first
              << "\t weight" << it->second
              << "\t weighted tolerance: " << newTolerance << std::endl;

    Interest interest(perFaceList[it->first]);
    auto entry = pitTable.find(interest);
    ndn::lp::Nack newNack(entry->getInterest());
    newNack.setHeader(newNackHeader);
    m_forwarder.sendDDoSNack(*getFace(it->first), newNack);

    NFD_LOG_TRACE("SendDDoSNack to downstream face " << it->first);
    NFD_LOG_TRACE("New Nack tolerance " << newNackHeader.m_tolerance);
    NFD_LOG_TRACE("New Nack fake name list (should be 0) " << newNackHeader.m_fakeInterestNames.size());
  }

  if (m_state != DDoS_ATTACK){
    m_state = DDoS_ATTACK;
    scheduleRevertStateEvent(ns3::Seconds(m_timer));
    NFD_LOG_DEBUG("Changed state to DDoS_ATTACK");
  }
}

shared_ptr<DDoSRecord>
DDoSStrategy::insertOrUpdateRecord(const lp::Nack& nack)
{
  shared_ptr<DDoSRecord> record = nullptr;
  const auto& nackReason = nack.getReason();
  Name prefix = nack.getInterest().getName().getPrefix(nack.getHeader().m_prefixLen);

  auto search = m_ddosRecords.find(prefix);
  if (search == m_ddosRecords.end()) {
    // the first nack
    // create a new DDoS record entry
    record = make_shared<DDoSRecord>();
    record->m_prefix = prefix;

    if (nackReason == lp::NackReason::DDOS_FAKE_INTEREST) {
      record->m_fakeNackCounter = 1;
      record->m_validNackCounter = 0;
      record->m_fakeInterestTolerance = nack.getHeader().m_tolerance;
    }
    if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
      record->m_fakeNackCounter = 0;
      record->m_validNackCounter = 1;
      record->m_validCapacity = nack.getHeader().m_tolerance;
    }
    // insert the new DDoS record
    m_ddosRecords[prefix] = record;
  }
  else {
    // not the first nack
    record = m_ddosRecords[prefix];


    // check if this nack was previously received
    if (nack.getHeader().m_nackId == (unsigned) record->m_nackId) {
      NS_LOG_DEBUG("This is a duplicated Nack");
      return nullptr;
    }

    if (nackReason == lp::NackReason::DDOS_FAKE_INTEREST) {
      record->m_fakeNackCounter++;
      record->m_fakeInterestTolerance = nack.getHeader().m_tolerance;
    }
    if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
      record->m_validNackCounter++;
      record->m_validCapacity = nack.getHeader().m_tolerance;
    }
    // add the counter in the record
    record->m_pushbackWeight.clear();
  }
  record->m_lastNackTimestamp = ns3::Simulator::Now();
  record->m_nackId = nack.getHeader().m_nackId;
  record->m_revertTimerCounter = DEFAULT_REVERT_TIME_COUNTER;
  record->m_additiveIncreaseCounter = 0;
  record->m_additiveIncreaseStep =  MIN_ADDITIVE_INCREASE_STEP;
  return record;
}

void
DDoSStrategy::handleHintChangeNack(const Face& inFace, const lp::Nack& nack,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
   if (m_forwarder.m_routerType == Forwarder::PRODUCER_GATEWAY_ROUTER){
    // forward the nack to all the incoming interfaces
    sendNacks(pitEntry, nack.getHeader());
    m_forwarder.ddoSRemovePIT(pitEntry);

    int prefixLen = nack.getHeader().m_prefixLen;
    Name prefix = nack.getInterest().getName().getPrefix(prefixLen);
    if(nack.getHeader().m_fakeInterestNames.size() > 0){
      Name new_name = nack.getHeader().m_fakeInterestNames.front();
      Fib& fib = m_forwarder.getFib();
      fib.erase(prefix);
      std::pair<fib::Entry*, bool> insert_return = fib.insert(new_name);
      if(!std::get<1>(insert_return)){
        NFD_LOG_TRACE("Entry already exists-----ERROR!!!");
      }
    }
    else{
         NFD_LOG_TRACE("No Name found for Name Change");
    }
  }
  else if (m_forwarder.m_routerType == Forwarder::NORMAL_ROUTER) {
    // forward the nack to all the incoming interfaces
    sendNacks(pitEntry, nack.getHeader());
  }
  else {
    // forward the nack only to good consumers
    int prefixLen = nack.getHeader().m_prefixLen;
    Name prefix = nack.getInterest().getName().getPrefix(prefixLen);
    auto search = m_ddosRecords.find(prefix);
    if (search == m_ddosRecords.end()) {
      sendNacks(pitEntry, nack.getHeader());
    }
    else {
      auto& recordEntry = m_ddosRecords[prefix];
      std::unordered_set<const Face*> downstreams;
      std::transform(pitEntry->in_begin(), pitEntry->in_end(), std::inserter(downstreams, downstreams.end()),
                     [] (const pit::InRecord& inR) { return &inR.getFace(); });
      for (const Face* downstream : downstreams) {
        if (recordEntry->m_markedInterestPerFace[downstream->getId()] > 0) {
          continue;
        }
        this->sendNack(pitEntry, *downstream, nack.getHeader());
      }
    }
  }
}

/**
 * =================================================================================================
 * ============================* Functions that we don't need to care *=============================
 * =================================================================================================
 **/

void
DDoSStrategy::beforeExpirePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
  // TODO
}

void
DDoSStrategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                                    const Face& inFace, const Data& data)
{
  // TODO
}

const Name&
DDoSStrategy::getStrategyName()
{
  static Name strategyName("ndn:/localhost/nfd/strategy/ddos/%FD%01");
  return strategyName;
}

void
DDoSStrategy::afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                               const shared_ptr<pit::Entry>& pitEntry)
{
  const auto& nackReason = nack.getReason();
  NFD_LOG_TRACE("AfterReceiveNack " << nackReason);

  // check if NACK is received beacuse of DDoS
  if (nackReason == lp::NackReason::DDOS_FAKE_INTEREST) {
    this->handleFakeInterestNack(inFace, nack, pitEntry);
  }
  else if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
    this->handleValidInterestNack(inFace, nack, pitEntry);
  }
  else if (nackReason == lp::NackReason::DDOS_HINT_CHANGE_NOTICE) {
    this->handleHintChangeNack(inFace, nack, pitEntry);
  }
  else {
    this->processNack(inFace, nack, pitEntry);
  }
}

void
DDoSStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
  // NFD_LOG_TRACE("After Receive Interest");
  if (hasPendingOutRecords(*pitEntry)) {
    // not a new Interest, don't forward
    return;
  }


  bool isPrefixUnderDDoS = false;
  for (auto& record : m_ddosRecords) {
    if (record.first.isPrefixOf(interest.getName())) {
      isPrefixUnderDDoS = true;
      if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER && inFace.m_isConsumerFace) {
        record.second->m_perFaceInterestBuffer[inFace.getId()].push_back(interest);
        NFD_LOG_TRACE("Interest Received with DDoS prefix: buffer Interest");
        return;
      }
    }
  }

  if (!isPrefixUnderDDoS) {
    NFD_LOG_TRACE("Interest Received without DDoS prefix: forward");
    this->doBestRoute(inFace, interest, pitEntry);
  }
  else {
    if (m_forwarder.m_routerType != Forwarder::CONSUMER_GATEWAY_ROUTER) {
      this->doLoadBalancing(inFace, interest, pitEntry);
      NFD_LOG_TRACE("Interest Received with DDoS prefix: load balance Interest");
    }
  }

}

static bool
canForwardToNextHop(const Face& inFace, shared_ptr<pit::Entry> pitEntry, const fib::NextHop& nexthop)
{
  return !wouldViolateScope(inFace, pitEntry->getInterest(), nexthop.getFace()) &&
    canForwardToLegacy(*pitEntry, nexthop.getFace());
}

static bool
hasFaceForForwarding(const Face& inFace, const fib::NextHopList& nexthops,
                     const shared_ptr<pit::Entry>& pitEntry)
{
  return std::find_if(nexthops.begin(), nexthops.end(),
                      bind(&canForwardToNextHop, cref(inFace), pitEntry, _1)) != nexthops.end();
}

void
DDoSStrategy::doLoadBalancing(const Face& inFace, const Interest& interest,
                              const shared_ptr<pit::Entry>& pitEntry)
{
  // NFD_LOG_TRACE("InterestForwarding: do load balancing");

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  // Ensure there is at least 1 Face is available for forwarding
  if (!hasFaceForForwarding(inFace, nexthops, pitEntry)) {
    this->rejectPendingInterest(pitEntry);
    return;
  }

  fib::NextHopList::const_iterator selected;
  do {
    boost::random::uniform_int_distribution<> dist(0, nexthops.size() - 1);
    const size_t randomIndex = dist(m_randomGenerator);

    uint64_t currentIndex = 0;

    for (selected = nexthops.begin(); selected != nexthops.end() && currentIndex != randomIndex;
         ++selected, ++currentIndex) {
    }
  } while (!canForwardToNextHop(inFace, pitEntry, *selected));
  this->sendInterest(pitEntry, selected->getFace(), interest);
}

void
DDoSStrategy::doBestRoute(const Face& inFace, const Interest& interest,
                          const shared_ptr<pit::Entry>& pitEntry)
{
  // NFD_LOG_TRACE("InterestForwarding: do best route");

  if (hasPendingOutRecords(*pitEntry)) {
    // not a new Interest, don't forward
    return;
  }

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {
    Face& outFace = it->getFace();
    if (!wouldViolateScope(inFace, interest, outFace) &&
        canForwardToLegacy(*pitEntry, outFace)) {
      this->sendInterest(pitEntry, outFace, interest);
      return;
    }
  }

  this->rejectPendingInterest(pitEntry);
}

} // namespace fw
} // namespace nfd
