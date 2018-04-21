#ifndef NFD_DAEMON_FW_DDOS_STRATEGY_HPP
#define NFD_DAEMON_FW_DDOS_STRATEGY_HPP

#include "ddos-record.hpp"
#include "face/face.hpp"
#include "strategy.hpp"
#include "process-nack-traits.hpp"
#include "algorithm.hpp"
#include <boost/random/mersenne_twister.hpp>

namespace nfd {
namespace fw {

typedef std::map<Name, shared_ptr<DDoSRecord>> DDoSRecords;

class DDoSStrategy : public Strategy
                   , public ProcessNackTraits<DDoSStrategy>
{
public:
  enum DDoSState
  {
    DDoS_NORMAL = 0,       /* no congestion and no ddos attack */
    DDoS_CONGESTION = 1,     /* under congestion state */
    DDoS_ATTACK = 2,       /* under ddos attack */
    // add more if needed
  };

public:
  DDoSStrategy(Forwarder& forwarder, const Name& name = getStrategyName());

  virtual
  ~DDoSStrategy() override;

  static const Name&
  getStrategyName();

  /**
   * @brief decide how to forward the Interest
   * Checking the state machine state: m_state
   * During congestion control or DDoS, this function will do load balancing forwarding
   * Otherwise, the strategy will do best route forwarding
   */
  virtual void
  afterReceiveInterest(const Face& inFace, const Interest& interest,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  /**
   * @brief handle nack
   * Nack carries DDoS feedback in our case.
   * Check the nack type/reason and react (e.g. nack new Interests, remove PIT)
   */
  virtual void
  afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                   const shared_ptr<pit::Entry>& pitEntry) override;

  /**
   * @brief handle Data packets
   */
  virtual void
  beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& inFace, const Data& data) override;

  /**
   * @brief used for calculate success ratio
   */
  virtual void
  beforeExpirePendingInterest(const shared_ptr<pit::Entry>& pitEntry) override;

protected:
  boost::random::mt19937 m_randomGenerator;

private:
  void
  handleFakeInterestNack(const Face& inFace, const lp::Nack& nack,
                         const shared_ptr<pit::Entry>& pitEntry);

  void
  handleValidInterestNack(const Face& inFace, const lp::Nack& nack,
                          const shared_ptr<pit::Entry>& pitEntry);

  void
  handleHintChangeNack(const Face& inFace, const lp::Nack& nack,
                       const shared_ptr<pit::Entry>& pitEntry);

  void
  doBestRoute(const Face& inFace, const Interest& interest,
              const shared_ptr<pit::Entry>& pitEntry);

  void
  doLoadBalancing(const Face& inFace, const Interest& interest,
                  const shared_ptr<pit::Entry>& pitEntry);

  shared_ptr<DDoSRecord>
  insertOrUpdateRecord(const lp::Nack& nack);

  void
  handleFakePushback(shared_ptr<DDoSRecord> record, const lp::Nack& nack,
                     std::list<shared_ptr<pit::Entry>>& deleteList);

  void
  applyForwardWithRateLimit();

  void
  scheduleRevertStateEvent(ns3::Time t);

  void
  revertState();

  friend ProcessNackTraits<DDoSStrategy>;

protected:

  ns3::EventId m_revertStateEvent; ///< @brief EventId of revert state event

private:
  // forwarder
  Forwarder& m_forwarder;

  DDoSState m_state;

  DDoSRecords m_ddosRecords;

  double m_timer;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_STRATEGY_HPP
