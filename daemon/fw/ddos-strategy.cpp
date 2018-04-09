/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2018,  Regents of the University of California
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ddos-strategy.hpp"
#include "algorithm.hpp"
#include "ddos-helper.hpp"
#include "core/logger.hpp"

namespace nfd {
namespace fw {

NFD_LOG_INIT("DDoS-Strategy");
NFD_REGISTER_STRATEGY(DDoSStrategy);

const time::milliseconds DDoSStrategy::RETX_SUPPRESSION_INITIAL(10);
const time::milliseconds DDoSStrategy::RETX_SUPPRESSION_MAX(250);

DDoSStrategy::DDoSStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
  , m_retxSuppression(RETX_SUPPRESSION_INITIAL,
                      RetxSuppressionExponential::DEFAULT_MULTIPLIER,
                      RETX_SUPPRESSION_MAX)
  , sharedInfo(make_shared<MtForwardingInfo>())

{
  ParsedInstanceName parsed = parseInstanceName(name);
  if (!parsed.parameters.empty()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument("DDoSStrategy does not accept parameters"));
  }
  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument("DDoSStrategy does not support version "
                                                + to_string(*parsed.version)));
  }
  this->setInstanceName(makeInstanceName(name, getStrategyName()));
}

const Name&
DDoSStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/ddos/%FD%05");
  return strategyName;
}

/** \brief determines whether a NextHop is eligible
 *  \param inFace incoming face of current Interest
 *  \param interest incoming Interest
 *  \param nexthop next hop
 *  \param pitEntry PIT entry
 *  \param wantUnused if true, NextHop must not have unexpired out-record
 *  \param now time::steady_clock::now(), ignored if !wantUnused
 */
static inline bool
isNextHopEligible(const Face& inFace, const Interest& interest,
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
    // nexthop must not have unexpired out-record
    pit::OutRecordCollection::iterator outRecord = pitEntry->getOutRecord(outFace);
    if (outRecord != pitEntry->out_end() && outRecord->getExpiry() > now) {
      return false;
    }
  }

  return true;
}

/** \brief pick an eligible NextHop with earliest out-record
 *  \note It is assumed that every nexthop has an out-record.
 */
static inline fib::NextHopList::const_iterator
findEligibleNextHopWithEarliestOutRecord(const Face& inFace, const Interest& interest,
                                         const fib::NextHopList& nexthops,
                                         const shared_ptr<pit::Entry>& pitEntry)
{
  fib::NextHopList::const_iterator found = nexthops.end();
  time::steady_clock::TimePoint earliestRenewed = time::steady_clock::TimePoint::max();
  for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) {
    if (!isNextHopEligible(inFace, interest, *it, pitEntry))
      continue;
    pit::OutRecordCollection::iterator outRecord = pitEntry->getOutRecord(it->getFace());
    BOOST_ASSERT(outRecord != pitEntry->out_end());
    if (outRecord->getLastRenewed() < earliestRenewed) {
      found = it;
      earliestRenewed = outRecord->getLastRenewed();
    }
  }
  return found;
}

void
DDoSStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
  RetxSuppressionResult suppression = m_retxSuppression.decidePerPitEntry(*pitEntry);
  if (suppression == RetxSuppressionResult::SUPPRESS) {
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << " suppressed");
    return;
  }

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();
  fib::NextHopList::const_iterator it = nexthops.end();

  if (suppression == RetxSuppressionResult::NEW) {

    Name miPrefix;
    MtForwardingInfo *mi = nullptr;

    std::tie(miPrefix, mi) = this->findPrefixMeasurements(fibEntry);

    // has measurements for forwarding prefix?
    if (mi == nullptr)
    {
      
      // create measurement table entries for this forwarding prefix
      MtForwardingInfo *mi = this->addPrefixMeasurements(fibEntry);
      if (mi == nullptr){
        std::cout << "it is null" << std::endl;
      }

    }


    // forward to nexthop with lowest cost except downstream
    it = std::find_if(nexthops.begin(), nexthops.end(),
                      bind(&isNextHopEligible, cref(inFace), interest, _1, pitEntry,
                           false, time::steady_clock::TimePoint::min()));

    if (it == nexthops.end()) {
      NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " noNextHop");

      lp::NackHeader nackHeader;
      nackHeader.setReason(lp::NackReason::NO_ROUTE);
      this->sendNack(pitEntry, inFace, nackHeader);

      this->rejectPendingInterest(pitEntry);
      return;
    }

    Face& outFace = it->getFace();
    this->sendInterest(pitEntry, outFace, interest);
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << " newPitEntry-to=" << outFace.getId());
    return;
  }

  // find an unused upstream with lowest cost except downstream
  it = std::find_if(nexthops.begin(), nexthops.end(),
                    bind(&isNextHopEligible, cref(inFace), interest, _1, pitEntry,
                         true, time::steady_clock::now()));
  if (it != nexthops.end()) {
    Face& outFace = it->getFace();
    this->sendInterest(pitEntry, outFace, interest);
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << " retransmit-unused-to=" << outFace.getId());
    return;
  }

  // find an eligible upstream that is used earliest
  it = findEligibleNextHopWithEarliestOutRecord(inFace, interest, nexthops, pitEntry);
  if (it == nexthops.end()) {
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " retransmitNoNextHop");
  }
  else {
    Face& outFace = it->getFace();
    this->sendInterest(pitEntry, outFace, interest);
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << " retransmit-retry-to=" << outFace.getId());
  }
}

void
DDoSStrategy::afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                               const shared_ptr<pit::Entry>& pitEntry)
{
  // TODO
}

void
DDoSStrategy::beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& inFace, const Data& data)
{
  // TODO
}

void
DDoSStrategy::beforeExpirePendingInterest(const shared_ptr<pit::Entry>& pitEntry)
{
  //TODO
}

std::tuple<Name, MtForwardingInfo*>
DDoSStrategy::findPrefixMeasurements(const fib::Entry& fibEntry)
{
  measurements::Entry* me = this->getMeasurements().get(fibEntry);
  if (me == nullptr) {
    return std::make_tuple(Name(), nullptr);
  }

  MtForwardingInfo* mi = me->getStrategyInfo<MtForwardingInfo>();

  // after runtime strategy change, it's possible that me exists but mi doesn't exist;
  // this case needs another longest prefix match until mi is found
  BOOST_ASSERT(mi != nullptr);

  return std::make_tuple(me->getName(), mi);
}

MtForwardingInfo*
DDoSStrategy::addPrefixMeasurements(const fib::Entry& fibEntry)
{
  measurements::Entry *me = nullptr;
  me = this->getMeasurements().get(fibEntry);
  BOOST_ASSERT(me != nullptr);
  
  // set lifetime for measurement entry
  // TODO: Reset to right value based on "decision machine"
  static const time::nanoseconds ME_LIFETIME = time::seconds(8);
  this->getMeasurements().extendLifetime(*me, ME_LIFETIME);

  return me->insertStrategyInfo<MtForwardingInfo>().first;
}

} // namespace fw
} // namespace nfd
