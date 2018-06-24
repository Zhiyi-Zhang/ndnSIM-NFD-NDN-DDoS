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

const static double DEFAULT_REVERT_TIME_COUNTER = 3;

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

    // if received NACK is less than m`_timer ago, continue
    if (ns3::Simulator::Now() - record->m_lastNackTimestamp < ns3::Seconds(m_timer)) {
      continue;
    }

    if (record->m_fakeDDoS && record->m_revertTimerCounter > 0) {
      record->m_revertTimerCounter -= m_timer;

      if (record->m_revertTimerCounter <= 0) {
        if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {
          // remove limit for good consumers
          std::list<FaceId> toRemoveLimit;
          for (const auto& perFacePushBackEntry : record->m_pushbackWeight) {
            if (record->m_isGoodConsumer[perFacePushBackEntry.first]) {
              toRemoveLimit.push_back(perFacePushBackEntry.first);
            }
          }
          for (const auto& faceId : toRemoveLimit) {
            record->m_pushbackWeight.erase(faceId);
            NFD_LOG_DEBUG("Remove pushback weight record: " << faceId);

            ndn::Interest interest(record->m_prefix);
            ndn::lp::Nack nack(interest);
            lp::NackHeader nackHeader;
            nackHeader.m_reason = lp::NackReason::DDOS_RESET_RATE;
            nackHeader.m_prefixLen = 0;
            nackHeader.m_tolerance = 0;
            nackHeader.m_nackId = rand() % 10000;
            nack.setHeader(nackHeader);

            getFace(faceId)->sendNack(nack);
            NFD_LOG_DEBUG("Send out RESET RATE nack: " << faceId);
          }
          if (record->m_pushbackWeight.size() == 0) {
            toBeDelete.push_back(recordEntry.first);
            record->m_fakeDDoS = false;
          }
          else {
            record->m_revertTimerCounter = DEFAULT_REVERT_TIME_COUNTER;
            record->m_fakeInterestTolerance = record->m_fakeInterestTolerance / 2;
          }
        }
        else {
          toBeDelete.push_back(recordEntry.first);
          record->m_fakeDDoS = false;
        }
      }
    }

    if (record->m_validOverload && record->m_validRevertTimerCounter > 0) {

      record->m_validRevertTimerCounter -= m_timer;

      if (record->m_validRevertTimerCounter <= 0) {
        if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {

          std::list<FaceId> toRemoveLimit;
          for (const auto& perFacePushBackEntry : record->m_validPushbackWeight) {
            if (record->m_validIsGoodConsumer[perFacePushBackEntry.first]) {
              toRemoveLimit.push_back(perFacePushBackEntry.first);
            }
          }
          for (const auto& faceId : toRemoveLimit) {
            record->m_validPushbackWeight.erase(faceId);
            NFD_LOG_DEBUG("Remove pushback weight record: " << faceId);

            ndn::Interest interest(record->m_prefix);
            ndn::lp::Nack nack(interest);
            lp::NackHeader nackHeader;
            nackHeader.m_reason = lp::NackReason::DDOS_RESET_RATE;
            nackHeader.m_prefixLen = 0;
            nackHeader.m_tolerance = 0;
            nackHeader.m_nackId = rand() % 10000;
            nack.setHeader(nackHeader);

            getFace(faceId)->sendNack(nack);
            NFD_LOG_DEBUG("Send out RESET RATE nack: " << faceId);
          }
          if (record->m_validPushbackWeight.size() == 0) {
            toBeDelete.push_back(recordEntry.first);
            record->m_validOverload = false;
          }
          else {
            record->m_validRevertTimerCounter = DEFAULT_REVERT_TIME_COUNTER;
            record->m_validCapacity = record->m_validCapacity / 2;
          }
        }
        else {
          toBeDelete.push_back(recordEntry.first);
          record->m_validOverload = false;
        }
      }
    }
  }

  // delete DDoS records that can be deleted
  for (const auto& name : toBeDelete) {
    auto search = m_ddosRecords.find(name);
    if (search != m_ddosRecords.end()) {
      if (!m_ddosRecords[name]->m_validOverload && !m_ddosRecords[name]->m_fakeDDoS) {
        m_ddosRecords.erase(name);
        NFD_LOG_DEBUG("Remove DDoS record: " << name);
      }
    }
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
      int finalLimit = 10000;
      int limit = 10000;
      int validLimit = 10000;

      if (record->m_fakeDDoS) {
        auto interfaceWeightEntry = record->m_pushbackWeight.find(faceId);
        if (interfaceWeightEntry != record->m_pushbackWeight.end()) {
          // calculate the current rate limit of the face
          double limitDouble = interfaceWeightEntry->second * record->m_fakeInterestTolerance * m_timer;
          double fractpart, intpart;
          fractpart = std::modf(limitDouble , &intpart);
          int addition = ((double) rand() / (RAND_MAX)) <= fractpart ? 1:0;
          limit = static_cast<int>(intpart) + addition;

          NFD_LOG_INFO("Fake: The weight is " << interfaceWeightEntry->second);
          NFD_LOG_INFO("Fake: The new limit on the face is " << limitDouble << " " << limit);

          if (perFaceBufInterest.second.size() > limit + 1) {
            record->m_isGoodConsumer[faceId] = false;
          }
          else {
            record->m_isGoodConsumer[faceId] = true;
          }
        }
        else {
          // there is no more limit on the current face
          NFD_LOG_INFO("No more rate limit on the face");
        }
      }

      if (record->m_validOverload) {
        auto interfaceWeightEntry = record->m_validPushbackWeight.find(faceId);
        if (interfaceWeightEntry != record->m_validPushbackWeight.end()) {
          // calculate the current rate limit of the face
          double limitDouble = interfaceWeightEntry->second * record->m_validCapacity * m_timer;
          double fractpart, intpart;
          fractpart = std::modf(limitDouble , &intpart);
          int addition = ((double) rand() / (RAND_MAX)) <= fractpart ? 1:0;
          validLimit = static_cast<int>(intpart) + addition;
          NFD_LOG_INFO("Valid: The weight is " << interfaceWeightEntry->second);
          NFD_LOG_INFO("Valid: The new limit on the face is " << validLimit);

          if (perFaceBufInterest.second.size() > validLimit + 1) {
            record->m_validIsGoodConsumer[faceId] = false;
          }
          else {
            record->m_validIsGoodConsumer[faceId] = true;
          }
        }
        else {
          // there is no more limit on the current face
          NFD_LOG_INFO("No more rate limit on the face");
        }
      }
      finalLimit = std::min(limit, validLimit);
      NFD_LOG_INFO("Finally use limit " << finalLimit << " on the face " << faceId);
      for (int i = 0; i != finalLimit; ++i) {
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
  ns3::Time t1 = ns3::Seconds(m_timer);
  if (m_state == DDoS_ATTACK) {
    t1 = ns3::Simulator::GetDelayLeft(m_revertStateEvent);
    ns3::Simulator::Cancel(m_revertStateEvent);
    NFD_LOG_TRACE("Cancel revert event " << t1);
  }

  NFD_LOG_TRACE("Handle Fake Nack");
  NFD_LOG_TRACE("Node ID " << m_forwarder.getNodeId());
  NFD_LOG_TRACE("Nack tolerance " << nack.getHeader().m_tolerance);
  NFD_LOG_TRACE("Nack fake name list size " << nack.getHeader().m_fakeInterestNames.size());

  Name prefix = nack.getInterest().getName().getPrefix(nack.getHeader().m_prefixLen);

  // to record the Pit Entry to be removed
  std::list<shared_ptr<pit::Entry>> deleteList;
  // to keep the corresponding DDoS record
  shared_ptr<DDoSRecord> record = insertOrUpdateRecord(nack);
  if (record == nullptr) {
    return;
  }

  // update pushback map and pushback
  handleFakePushback(record, nack, deleteList);

  std::cout << "node id :" << m_forwarder.getNodeId() << std::endl
            << "receiving tolerance" << nack.getHeader().m_tolerance << std::endl;

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
  ns3::Time t1 = ns3::Seconds(m_timer);
  if (m_state == DDoS_ATTACK) {
    t1 = ns3::Simulator::GetDelayLeft(m_revertStateEvent);
    ns3::Simulator::Cancel(m_revertStateEvent);
    NFD_LOG_TRACE("Cancel revert event " << t1);
  }

  NFD_LOG_TRACE("Handle Valid Interest Nack");
  NFD_LOG_TRACE("Node ID " << m_forwarder.getNodeId());
  NFD_LOG_TRACE("Valid capacity " << nack.getHeader().m_tolerance);

  Name prefix = nack.getInterest().getName().getPrefix(nack.getHeader().m_prefixLen);

  // get PIT table
  auto& pitTable = m_forwarder.m_pit;

  // to record per face Interest list to be nacked
  std::map<FaceId, Name> perFaceList;

  // to keep the corresponding DDoS record
  shared_ptr<DDoSRecord> record = insertOrUpdateRecord(nack);
  if (record == nullptr) {
    return;
  }

  int totalMatchingInterestNumber = 0;
  // calculate DDoS record per face pushback weight

  std::cout << "pitTable size: " << pitTable.size() << std::endl;
  for (auto& pitEntry : pitTable) { // iterate all interest names

    if (prefix.isPrefixOf(pitEntry.getInterest().getName())) {
      // iterate its incoming Faces and calculate pushback weight
      const auto& inRecords = pitEntry.getInRecords();
      double inFaceNumber = inRecords.size();
      if (inFaceNumber > 0) {
        ++totalMatchingInterestNumber;
        for (const auto& inRecord: inRecords) {
          FaceId faceId = inRecord.getFace().getId();
          auto innerSearch = record->m_validPushbackWeight.find(faceId);
          if (innerSearch == record->m_validPushbackWeight.end()) {
            record->m_validPushbackWeight[faceId] = 1 / inFaceNumber;
          }
          else {
            record->m_validPushbackWeight[faceId] += 1 / inFaceNumber;
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
  int counter = 0;
  double weightForConsumers = 0;
  for (auto& pushbackEntry : record->m_validPushbackWeight) {

    // init isGoodConsumer map
    record->m_validIsGoodConsumer[pushbackEntry.first] = true;

    std::cout << "before division: " << pushbackEntry.second << "\t totalmatchedInterest: " << totalMatchingInterestNumber << std::endl;
    pushbackEntry.second = pushbackEntry.second / totalMatchingInterestNumber;

    if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER
        && getFace(pushbackEntry.first)->m_isConsumerFace) {
      weightForConsumers += pushbackEntry.second;
      ++counter;
    }
  }
  for (auto& pushbackEntry : record->m_validPushbackWeight) {

    if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER
        && getFace(pushbackEntry.first)->m_isConsumerFace) {
      pushbackEntry.second = weightForConsumers/counter;
    }

    lp::NackHeader newNackHeader;
    newNackHeader.m_reason = nack.getHeader().m_reason;
    newNackHeader.m_prefixLen = nack.getHeader().m_prefixLen;
    newNackHeader.m_nackId = nack.getHeader().m_nackId;
    newNackHeader.m_tolerance =  static_cast<uint64_t>(nack.getHeader().m_tolerance * pushbackEntry.second + 0.5);
    newNackHeader.m_fakeInterestNames = nack.getHeader().m_fakeInterestNames;

    std::cout << "\t face id: " << pushbackEntry.first
              << "\t weight" << pushbackEntry.second
              << "\t weighted tolerance: " << newNackHeader.m_tolerance << std::endl;

    Interest interest(perFaceList[pushbackEntry.first]);
    auto entry = pitTable.find(interest);
    ndn::lp::Nack newNack(entry->getInterest());
    newNack.setHeader(newNackHeader);
    m_forwarder.sendDDoSNack(*getFace(pushbackEntry.first), newNack);

    NFD_LOG_TRACE("SendDDoSNack to downstream face " << pushbackEntry.first);
    NFD_LOG_TRACE("New Nack tolerance " << newNackHeader.m_tolerance);
    NFD_LOG_TRACE("New Nack fake name list (should be 0) " << newNackHeader.m_fakeInterestNames.size());
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
      record->m_fakeDDoS = true;
      record->m_validOverload = false;
      record->m_fakeInterestTolerance = nack.getHeader().m_tolerance;
    }
    if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
      record->m_validOverload = true;
      record->m_fakeDDoS = false;
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
      record->m_fakeDDoS = true;
      // use moving average for now
      record->m_fakeInterestTolerance = (record->m_fakeInterestTolerance
                                         + nack.getHeader().m_tolerance) / 2;
      // another choice is to replace the old one with new one directly
      // record->m_fakeInterestTolerance = nack.getHeader().m_tolerance;
      NS_LOG_DEBUG("The new tolerance is " << record->m_fakeInterestTolerance);

      // add the counter in the record
      if (record->m_revertTimerCounter <= 0) {
        record->m_pushbackWeight.clear();
        NS_LOG_DEBUG("Clear the push back weight map");
      }
    }
    if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
      record->m_validOverload = true;
      record->m_validCapacity = (record->m_validCapacity
                                 + nack.getHeader().m_tolerance) / 2;
      record->m_validPushbackWeight.clear();
    }
  }

  if (nackReason == lp::NackReason::DDOS_FAKE_INTEREST) {
    record->m_lastNackTimestamp = ns3::Simulator::Now();
    record->m_nackId = nack.getHeader().m_nackId;
    record->m_revertTimerCounter = DEFAULT_REVERT_TIME_COUNTER;
  }
  if (nackReason == lp::NackReason::DDOS_VALID_INTEREST_OVERLOAD) {
    record->m_validLastNackTimestamp = ns3::Simulator::Now();
    record->m_validNackId = nack.getHeader().m_nackId;
    record->m_validRevertTimerCounter = DEFAULT_REVERT_TIME_COUNTER;
  }
  return record;
}

void
DDoSStrategy::handleFakePushback(shared_ptr<DDoSRecord> record, const lp::Nack& nack,
                                 std::list<shared_ptr<pit::Entry>>& deleteList)
{
  const auto& nackNameList = nack.getHeader().m_fakeInterestNames;
  double denominator = nackNameList.size();
  auto& pitTable = m_forwarder.m_pit;

  // to record per face Interest list to be nacked
  std::map<FaceId, std::list<Name>> perFaceList;
  std::map<FaceId, double> tempPushBack;

  if (record->m_pushbackWeight.size() != 0) {
    // the previous pushback weight map is still fresh

    bool hasNewFace = false;

    // first iterate all the PIT entries whose name is listed in nack
    for (const auto& nackName : nackNameList) { // iterate all fake interest names
      Interest interest(nackName);

      // find corresponding PIT Entry
      auto entry = pitTable.find(interest);
      if (entry != nullptr) {
        // iterate its incoming Faces and calculate pushback weight
        const auto& inRecords = entry->getInRecords();
        double inFaceNumber = inRecords.size();
        for (const auto& inRecord: inRecords) {
          FaceId faceId = inRecord.getFace().getId();
          auto tempSearch = tempPushBack.find(faceId);
          if (tempSearch == tempPushBack.end()) {
            tempPushBack[faceId] = 1 / ( denominator * inFaceNumber);
          }
          else {
            tempPushBack[faceId] += 1 / ( denominator * inFaceNumber);
          }
          auto innerSearch = record->m_pushbackWeight.find(faceId);
          if (innerSearch == record->m_pushbackWeight.end()) {
            hasNewFace = true;
            NS_LOG_DEBUG("The old pushback list don't have face!!!!! " << faceId);
          }
          perFaceList[faceId].push_back(nackName);
        }
        deleteList.push_back(entry);
      }
      else {
        continue;
      }
    }

    if (hasNewFace) {
      NS_LOG_DEBUG("Has new Face!!! we need to balance the pushback weight!!!!");
      // if has new face, balance the new pushback and old push back with proportion 1:1
      for (const auto& tempEntry : tempPushBack) {
        auto innerSearch = record->m_pushbackWeight.find(tempEntry.first);
        if (innerSearch == record->m_pushbackWeight.end()) {
          hasNewFace = true;
          record->m_pushbackWeight[tempEntry.first] = tempEntry.second;
        }
        else {
          record->m_pushbackWeight[tempEntry.first] += tempEntry.second;
        }
      }
      for (auto& pushBackEntry : record->m_pushbackWeight) {
        pushBackEntry.second = static_cast<int> (pushBackEntry.second / 2 + 0.5);
      }
    }
  }
  else {
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
    if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER) {
      double weightForConsumers = 0;
      int counter = 0;
      for (auto& pushBackEntry : record->m_pushbackWeight) {
        if (getFace(pushBackEntry.first)->m_isConsumerFace) {
          ++counter;
          weightForConsumers += pushBackEntry.second;
        }
      }
      for (auto& pushBackEntry : record->m_pushbackWeight) {
        if (getFace(pushBackEntry.first)->m_isConsumerFace) {
          pushBackEntry.second = weightForConsumers/counter;
        }
      }
    }
    tempPushBack = record->m_pushbackWeight;
  }

  // pushback nacks to Interest Upstreams
  for (auto pushBackItem : tempPushBack) {

    // init isGoodConsumer map
    record->m_isGoodConsumer[pushBackItem.first] = true;

    lp::NackHeader newNackHeader;
    newNackHeader.m_reason = nack.getHeader().m_reason;
    newNackHeader.m_prefixLen = nack.getHeader().m_prefixLen;
    newNackHeader.m_nackId = nack.getHeader().m_nackId;
    int newTolerance = static_cast<uint64_t>(nack.getHeader().m_tolerance * pushBackItem.second + 0.5);
    newNackHeader.m_tolerance = newTolerance;
    newNackHeader.m_fakeInterestNames = perFaceList[pushBackItem.first];

    std::cout << "\t face id: " << pushBackItem.first
              << "\t weight" << pushBackItem.second
              << "\t weighted tolerance: " << newTolerance << std::endl;

    std::cout << "name list for this face: " << perFaceList[pushBackItem.first].size() << std::endl;

    Interest interest(perFaceList[pushBackItem.first].front());
    auto entry = pitTable.find(interest);
    ndn::lp::Nack newNack(entry->getInterest());
    newNack.setHeader(newNackHeader);
    m_forwarder.sendDDoSNack(*getFace(pushBackItem.first), newNack);

    NFD_LOG_TRACE("SendDDoSNack to downstream face " << pushBackItem.first);
    NFD_LOG_TRACE("New Nack tolerance " << newNackHeader.m_tolerance);
    NFD_LOG_TRACE("New Nack fake name list " << newNackHeader.m_fakeInterestNames.size());
  }

}

/**
 * =================================================================================================
 * ============================* Functions that we don't need to care *=============================
 * =================================================================================================
 **/


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

      auto validSearch = record.second->m_pushbackWeight.find(inFace.getId());
      auto fakeSearch = record.second->m_validPushbackWeight.find(inFace.getId());

      if (m_forwarder.m_routerType == Forwarder::CONSUMER_GATEWAY_ROUTER
          && inFace.m_isConsumerFace
          && (validSearch != record.second->m_pushbackWeight.end()
              || fakeSearch != record.second->m_validPushbackWeight.end())) {
        record.second->m_perFaceInterestBuffer[inFace.getId()].push_back(interest);
        // NFD_LOG_TRACE("Interest Received with DDoS prefix: buffer Interest " << inFace.getId());
        return;
      }
    }
  }

  if (!isPrefixUnderDDoS) {
    // NFD_LOG_TRACE("Interest Received without DDoS prefix: forward");
    this->doBestRoute(inFace, interest, pitEntry);
  }
  else {
    this->doLoadBalancing(inFace, interest, pitEntry);
    // NFD_LOG_TRACE("Interest Received with DDoS prefix: load balance Interest");
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
