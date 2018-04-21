#ifndef NFD_DAEMON_FW_DDOS_RECORD_HPP
#define NFD_DAEMON_FW_DDOS_RECORD_HPP

#include "face/face.hpp"
#include "ns3/nstime.h"
#include <map>

namespace nfd {
namespace fw {

class DDoSRecord
{

public: // essential field
  Name m_prefix;

  // the step of additive increase
  int m_additiveIncreaseStep;

public: // fake interest attack
  // Which type of attack is happening
  bool m_fakeDDoS;

  // interest number per second
  int m_fakeInterestTolerance;

  ns3::Time m_lastNackTimestamp;
  int m_nackId;
  double m_revertTimerCounter;
  int m_additiveIncreaseCounter;

  // pushback weight per face
  std::map<FaceId, double> m_pushbackWeight;


public: // for valid attack
  bool m_validOverload;

  // interest number per second
  int m_validCapacity;

  int m_validNackId;
  ns3::Time m_validLastNackTimestamp;
  double m_validRevertTimerCounter;
  int m_validAdditiveIncreaseCounter;

  std::map<FaceId, double> m_validPushbackWeight;


public:
  // interest buffer per face in last check window
  std::map<FaceId, std::list<Interest>> m_perFaceInterestBuffer;

  std::map<FaceId, bool> m_isGoodConsumer;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_RECORD_HPP
