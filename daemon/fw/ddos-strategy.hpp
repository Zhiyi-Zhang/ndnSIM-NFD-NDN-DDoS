#ifndef NFD_DAEMON_FW_DDOS_STRATEGY_HPP
#define NFD_DAEMON_FW_DDOS_STRATEGY_HPP

#include <boost/random/mersenne_twister.hpp>
#include "face/face.hpp"
#include "strategy.hpp"
#include "algorithm.hpp"

namespace nfd {
namespace fw {

class DDoSStrategy : public Strategy {
public:
  DDoSStrategy(Forwarder& forwarder, const Name& name = getStrategyName());

  virtual ~DDoSStrategy() override;

  virtual void
  afterReceiveInterest(const Face& inFace, const Interest& interest,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  virtual void
  afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                   const shared_ptr<pit::Entry>& pitEntry);

  static const Name&
  getStrategyName();

protected:
  boost::random::mt19937 m_randomGenerator;

private:

  // interests/sec threshold
  double m_rateThreshold;

  // interest satisfaction ratio threshold
  double m_successRatioThreshold;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_STRATEGY_HPP