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

public: // fake interest attack
  // Which type of attack is happening
  bool m_fakeDDoS;

  // interest number per second
  double m_fakeInterestTolerance;

  ns3::Time m_lastNackTimestamp;
  int m_nackId;
  double m_revertTimerCounter;

  // pushback weight per face
  std::map<FaceId, double> m_pushbackWeight;
  std::map<FaceId, bool> m_isGoodConsumer;


public: // for valid attack
  bool m_validOverload;

  // interest number per second
  double m_validCapacity;

  int m_validNackId;
  ns3::Time m_validLastNackTimestamp;
  double m_validRevertTimerCounter;

  std::map<FaceId, double> m_validPushbackWeight;
  std::map<FaceId, bool> m_validIsGoodConsumer;


public:
  // interest buffer per face in last check window
  std::map<FaceId, std::list<Interest>> m_perFaceInterestBuffer;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_RECORD_HPP
