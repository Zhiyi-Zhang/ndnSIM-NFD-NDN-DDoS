#ifndef NFD_DAEMON_FW_PUSHBACK_STRATEGY_HPP
#define NFD_DAEMON_FW_PUSHBACK_STRATEGY_HPP

#include "strategy.hpp"
#include "mt-forwarding-info.hpp"
#include <boost/thread/pthread/mutex.hpp>
#include <fstream>

namespace nfd {
namespace fw {

/** \brief Pushback Strategy
 * 
 */
class PushbackStrategy: public Strategy
{
public:

  PushbackStrategy(Forwarder& forwarder, const Name& name = getStrategyName());

  static const Name&
  getStrategyName();

  virtual
  ~PushbackStrategy();

  virtual void
  afterReceiveInterest(const Face& inFace, const Interest& interest,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  virtual void
  beforeSatisfyInterest(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& inFace, const Data& data) override;

  virtual void
  beforeExpirePendingInterest(const shared_ptr<pit::Entry>& pitEntry) override;

  /**
   * Sends out probed interest packets with a new nonce.
   * Currently all paths except the working path are probed whenever probingDue() returns true.
   */
  void
  probeInterests(const FaceId outFaceId, const fib::NextHopList& nexthops,
      shared_ptr<pit::Entry> pitEntry);

public:
  static const Name STRATEGY_NAME;

private:

  static void
  writeFwPercMap(Forwarder& ownForwarder, MtForwardingInfo* measurementInfo);

  void
  initializeForwMap(MtForwardingInfo* measurementInfo,
      std::vector<fib::NextHop> nexthops);

private:

  /* variables for RTT measurement */

  std::map<int, int> m_rttFaceMap; // <face id, sum of rtt values>
  std::map<int, int> m_rttCountFaceMap; // <face id, count of rtt values>

  // calculate average RTT for given face for lastNRTT values
  // hence, average RTT will be compared at every lastNRTT interest-data pair
  int m_lastNRTT = 100;

  std::map<int, double> m_avgRTTMap; // <face id, last 100 RTT value average>

  std::map<int, std::map<std::string, ndn::time::steady_clock::TimePoint>> m_interestEntryTime; // <face id, <interest, time>  
           

  static shared_ptr<std::ofstream> m_os;
  static boost::mutex m_mutex;

  Forwarder& m_ownForwarder;
  time::steady_clock::TimePoint m_lastFWRatioUpdate;
  time::steady_clock::TimePoint m_lastFWWrite;

  bool INIT_SHORTEST_PATH;
  double PROBING_PERCENTAGE;
  double CHANGE_PER_MARK;

//  bool USE_HIGH_CONG_THRESH;

  shared_ptr<MtForwardingInfo> sharedInfo;

  const time::steady_clock::duration TIME_BETWEEN_FW_UPDATE;
  const time::steady_clock::duration TIME_BETWEEN_FW_WRITE;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_PUSHBACK_STRATEGY_HPP
