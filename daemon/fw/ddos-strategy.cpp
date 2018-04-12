#include "ddos-strategy.hpp"
#include "ddos-helper.hpp"
#include <boost/random/uniform_int_distribution.hpp>
#include <ndn-cxx/util/random.hpp>
#include "core/logger.hpp"

NFD_LOG_INIT("DDoSStrategy");

namespace nfd {
namespace fw {

DDoSStrategy::DDoSStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
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
  NFD_LOG_TRACE("afterReceiveNack");
  lp::NackReason nackReason = nack.getReason();

  // check if NACK is received beacuse of DDoS
  if (nackReason == lp::NackReason::FAKE_INTEREST_OVERLOAD
      || nackReason == lp::NackReason::VALID_INTEREST_OVERLOAD) {

    std::cout << nackReason << std::endl;
    // first delete the tmp PIT entry
    if (!pitEntry->hasInRecords()) {
      this->rejectPendingInterest(pitEntry);
    }

    // Step 1: Check success ratio of that prefix per interface;find faces that violates the threshold
    // Step 2: Check the
    // Step 2: For interfaces that has abrupt traffic increase of such prefixes, rate limit
    // Step 3: If already rate limiting and receive NACK again, pushback to downstream routers

    // When traffic is below thresholds and no NACK received, stop rate limit

  }
  else {
    this->processNack(inFace, nack, pitEntry);
  }
}

void
DDoSStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                   const shared_ptr<pit::Entry>& pitEntry)
{
  if (hasPendingOutRecords(*pitEntry)) {
    // not a new Interest, don't forward
    return;
  }

  if (m_state == DDoS_NORMAL) {
    this->doBestRoute(inFace, interest, pitEntry);
  }
  else if (m_state == DDoS_CONGESTION || m_state == DDoS_ATTACK) {
    this->doLoadBalancing(inFace, interest, pitEntry);
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
