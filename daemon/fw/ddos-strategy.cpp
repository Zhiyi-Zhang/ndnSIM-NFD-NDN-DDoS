#include "ddos-strategy.hpp"
#include "ddos-helper.hpp"
#include <boost/random/uniform_int_distribution.hpp>
#include "core/logger.hpp"
#include "ns3/simulator.h"

namespace nfd {
namespace fw {

NFD_LOG_INIT("DDoSStrategy");
NFD_REGISTER_STRATEGY(DDoSStrategy);

DDoSStrategy::DDoSStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
  , m_noApplyRateEventRunsYet(true)
  , m_timer(0)
  , m_forwarder(forwarder)
  , m_state(DDoS_NORMAL)
{
  this->setInstanceName(makeInstanceName(name, getStrategyName()));
}

DDoSStrategy::~DDoSStrategy()
{
}

static bool
canForwardToNextHop(const Face& inFace,
                    shared_ptr<pit::Entry> pitEntry,
                    const fib::NextHop& nexthop)
{
  return !wouldViolateScope(inFace, pitEntry->getInterest(), nexthop.getFace()) &&
    canForwardToLegacy(*pitEntry, nexthop.getFace());
}

static bool
hasFaceForForwarding(const Face& inFace,
                     const fib::NextHopList& nexthops,
                     const shared_ptr<pit::Entry>& pitEntry)
{
  return std::find_if(nexthops.begin(), nexthops.end(),
                      bind(&canForwardToNextHop, cref(inFace), pitEntry, _1))
    != nexthops.end();
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
    NFD_LOG_TRACE("After handleFakeInterestNack " << nackReason);
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
DDoSStrategy::scheduleRevertStateEvent()
{
  NFD_LOG_TRACE("Scheduling revert state event");
  if (!m_revertStateEvent.IsRunning()) {    
    m_revertStateEvent = ns3::Simulator::Schedule(ns3::Seconds(m_timer),
                                        &DDoSStrategy::revertState, this);
  } else {
    ns3::Time t = ns3::Simulator::GetDelayLeft(m_revertStateEvent);

    // if the delay is lesser than new NACK timer
    if (t < ns3::Seconds(m_timer)){
      // cancel old scheduled event and start new
      ns3::Simulator::Cancel(m_revertStateEvent);
      m_revertStateEvent = ns3::Simulator::Schedule(ns3::Seconds(m_timer),
                                          &DDoSStrategy::revertState, this);
    }
  }
}

void DDoSStrategy::revertState()
{
  NFD_LOG_TRACE("Reverting state");
  if (m_state == DDoS_ATTACK) {
    m_state = DDoS_NORMAL;
    m_ddosRecords.clear();
    m_noApplyRateEventRunsYet = true;
    NFD_LOG_DEBUG("Changed state to DDoS_NORMAL");
  }
}

void
DDoSStrategy::scheduleApplyRateAndForwardEvent()
{
  NFD_LOG_TRACE("Scheduling apply rate and forward event");
  if (!m_applyRateAndForwardEvent.IsRunning()) {
      m_applyRateAndForwardEvent = ns3::Simulator::Schedule(ns3::Seconds(m_checkWindow),
                                                  &DDoSStrategy::applyRateAndForward, this);
  }
}

void
DDoSStrategy::applyRateAndForward()
{
  NFD_LOG_TRACE("Applying rate and forwarding");

  // for each DDoS record
  for (auto& recordEntry : m_ddosRecords) {
    auto& record = recordEntry.second;
    const auto& prefix = record->m_prefix;

    for (auto& perFaceBufInterest : recordEntry.second->m_perFaceInterestBuffer) {
      auto& faceId = perFaceBufInterest.first;
      auto interfaceWeight = record->m_pushbackWeight.find(faceId);

      if (interfaceWeight != record->m_pushbackWeight.end()) {

        // calculate number of allowed interest for this face
        int allowedInterests = 10000;

        // not rate limiting on this prefix yet
        if (!record->m_rateLimiting) {
          NFD_LOG_DEBUG("Not yet rate limiting");
          allowedInterests = record->m_fakeInterestTolerance * (interfaceWeight->second);
          record->m_rateLimiting = true;
          lastNackCountSeen[prefix] = record->m_fakeNackCounter;
        }
        // if no new NACK has been received
        else if (lastNackCountSeen[prefix] == record->m_fakeNackCounter) {
          NFD_LOG_DEBUG("Rate limiting but no new NACK received");
          allowedInterests = record->m_lastAllowedInterestCount + m_additiveIncrease;
        }
        // if new NACK has been received
        else if (lastNackCountSeen[prefix] < record->m_fakeNackCounter) {
          NFD_LOG_DEBUG("Rate limiting and new NACK received");
          allowedInterests = record->m_lastAllowedInterestCount / m_multiplicativeDecrease;
          lastNackCountSeen[prefix]++;
        }

        // update m_fakeInterestTolerance
        record->m_lastAllowedInterestCount = allowedInterests;
        NFD_LOG_INFO("Applying rate " << allowedInterests);

        // forward those number of allowed interests
        for (int i = 0; i != allowedInterests; ++i) {
          if (perFaceBufInterest.second.size() > (unsigned) i) {
            auto innerIt = perFaceBufInterest.second.begin();
            std::advance(innerIt, i);

            Name interest_copy(*innerIt);
            auto interest = std::make_shared<ndn::Interest>(interest_copy);
            shared_ptr<pit::Entry> pitEntry = m_forwarder.m_pit.find(*interest);
            this->doLoadBalancing(*getFace(faceId), *interest, pitEntry);
            NFD_LOG_INFO("After loop, we sent out Interest " << interest->getName());
          }
        }
      }
    }

    record->m_perFaceInterestBuffer.clear();
  }

  if (m_state == DDoS_ATTACK) {
    scheduleApplyRateAndForwardEvent();
  }
}

void
DDoSStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("After Receive Interest");
  if (hasPendingOutRecords(*pitEntry)) {
    // not a new Interest, don't forward
    return;
  }

  bool isPrefixUnderDDoS = false;
  for (auto& record : m_ddosRecords) {
    if (record.first.isPrefixOf(interest.getName())) {
      isPrefixUnderDDoS = true;
      record.second->m_perFaceInterestBuffer[inFace.getId()].push_back(interest.getName());
    }
  }

  if (!isPrefixUnderDDoS) {
    NFD_LOG_TRACE("Interest Received without DDoS prefix: forward");
    this->doBestRoute(inFace, interest, pitEntry);
  }
  else if (isPrefixUnderDDoS) {
    NFD_LOG_TRACE("Interest Received with DDoS prefix: buffer Interest");
  }
}

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
DDoSStrategy::handleFakeInterestNack(const Face& inFace, const lp::Nack& nack,
                                     const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("Handle Nack");
  NFD_LOG_TRACE("Nack tolerance " << nack.getHeader().m_fakeTolerance);
  NFD_LOG_TRACE("Nack fake name list " << nack.getHeader().m_fakeInterestNames.size());

  if (m_noApplyRateEventRunsYet && m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {
    scheduleApplyRateAndForwardEvent();
    m_noApplyRateEventRunsYet = false;
  }

  // no matter the prefix, change state to DDoS_ATTACK
  m_state = DDoS_ATTACK;
  NFD_LOG_DEBUG("Changed state to DDoS_ATTACK");

  // change the state to DDoS_NORMAL as per NACK timer
  m_timer = nack.getHeader().m_timer;
  scheduleRevertStateEvent();

  // first delete the tmp PIT entry
  if (!pitEntry->hasInRecords()) {
    this->rejectPendingInterest(pitEntry);
  }
  int prefixLen = nack.getHeader().m_prefixLen;
  Name prefix = nack.getInterest().getName().getPrefix(prefixLen);

  auto& pitTable = m_forwarder.m_pit;
  NFD_LOG_TRACE("Current PIT Table size: " << pitTable.size());
  std::list<shared_ptr<pit::Entry>> deleteList;
  auto search = m_ddosRecords.find(prefix);
  if (search == m_ddosRecords.end()) { // the first nack

    // create DDoS record entry
    auto record = make_shared<DDoSRecord>();
    record->m_prefix = prefix;
    record->m_type = DDoSRecord::FAKE;
    record->m_fakeNackCounter = 1;
    record->m_validNackCounter = 0;
    record->m_rateLimiting = false;
    record->m_lastAllowedInterestCount = 0;
    record->m_fakeInterestTolerance = nack.getHeader().m_fakeTolerance;

    std::map<FaceId, std::list<Name>> perFaceList;

    // calculate DDoS record per face pushback weight
    const auto& nackNameList = nack.getHeader().m_fakeInterestNames;
    double denominator = nackNameList.size();
    auto& pitTable = m_forwarder.m_pit;
    NFD_LOG_TRACE("Current PIT Table size: " << pitTable.size());
    for (const auto& nackName : nackNameList) { // iterate all fake interest names
      Interest interest(nackName);

      // find corresponding PIT Entry
      auto entry = pitTable.find(interest);
      if (entry != nullptr) {

        // iterate its incoming Faces and calculate pushback weight
        const auto& inRecords = entry->getInRecords();
        int inFaceNumber = inRecords.size();
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
    m_ddosRecords[prefix] = record;

    // pushback nacks
    for (auto it = record->m_pushbackWeight.begin();
         it != record->m_pushbackWeight.end(); ++it) {
      ndn::lp::Nack newNack(nack.getInterest());
      lp::NackHeader newNackHeader;
      newNackHeader.m_reason = nack.getHeader().m_reason;
      newNackHeader.m_prefixLen = nack.getHeader().m_prefixLen;
      newNackHeader.m_fakeTolerance = static_cast<uint64_t>(nack.getHeader().m_fakeTolerance * it->second);
      newNackHeader.m_fakeInterestNames = perFaceList[it->first];
      newNack.setHeader(newNackHeader);
      m_forwarder.sendDDoSNack(*getFace(it->first), newNack);

      NFD_LOG_TRACE("SendDDoSNack to downstream");
      NFD_LOG_TRACE("New Nack tolerance " << newNackHeader.m_fakeTolerance);
      NFD_LOG_TRACE("New Nack fake name list " << newNackHeader.m_fakeInterestNames.size());
    }
  }
  else {
    // not the first nack
    // TODO
  }

  for (auto toBeDelete : deleteList) {
    m_forwarder.ddoSRemovePIT(toBeDelete);
  }
}

void
DDoSStrategy::handleValidInterestNack(const Face& inFace, const lp::Nack& nack,
                                      const shared_ptr<pit::Entry>& pitEntry)
{
  // first delete the tmp PIT entry
    if (!pitEntry->hasInRecords()) {
      this->rejectPendingInterest(pitEntry);
    }
}

void
DDoSStrategy::handleHintChangeNack(const Face& inFace, const lp::Nack& nack,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
  if (m_forwarder.m_routerType == Forwarder::PRODUCER_GATEWAY_ROUTER ||
      m_forwarder.m_routerType == Forwarder::NORMAL_ROUTER) {
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

void
DDoSStrategy::doLoadBalancing(const Face& inFace, const Interest& interest,
                              const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_TRACE("InterestForwarding: do load balancing");

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
  NFD_LOG_TRACE("InterestForwarding: do best route");

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
