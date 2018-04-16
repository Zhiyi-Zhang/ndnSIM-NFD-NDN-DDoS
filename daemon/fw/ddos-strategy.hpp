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

  void
  scheduleApplyRateAndForwardEvent();

  void
  applyRateAndForward();

  void
  scheduleRevertStateEvent();

  void
  revertState();

  friend ProcessNackTraits<DDoSStrategy>;

protected:
  ns3::EventId m_applyRateAndForwardEvent; ///< @brief EventId of apply rate and forward event
  bool m_noApplyRateEventRunsYet;

  ns3::EventId m_revertStateEvent; ///< @brief EventId of revert state event
  uint64_t m_timer;

private:
  // forwarder
  Forwarder& m_forwarder;

  // the state of the state machine
  DDoSState m_state;

  // additive increase constant
  double m_additiveIncrease = 1;

  // multiplicate decrease constant
  double m_multiplicativeDecrease = 2;

  DDoSRecords m_ddosRecords;

  double m_checkWindow = 1;

  // last NACK count seen for each prefix
  std::map<Name, int> lastNackCountSeen;

};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_STRATEGY_HPP
