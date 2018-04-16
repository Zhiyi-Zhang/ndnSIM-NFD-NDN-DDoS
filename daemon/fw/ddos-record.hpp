#ifndef NFD_DAEMON_FW_DDOS_RECORD_HPP
#define NFD_DAEMON_FW_DDOS_RECORD_HPP

#include "face/face.hpp"
#include <map>

namespace nfd {
namespace fw {

class DDoSRecord
{
public:
  enum DDoSType {
    VALID,
    FAKE,
    MIXED
  };

public: // required field
  Name m_prefix;

  DDoSType m_type;

  // count how many DDoS records has been received after the first one
  int m_fakeNackCounter;
  int m_validNackCounter;

  // has rate limiting started?
  bool m_rateLimiting;

  // used by revert Event
  int m_revertTimerCounter;


public: // expectations

  // interest number per second
  int m_fakeInterestTolerance;
  int m_lastAllowedInterestCount;


public: // counters

  // interest buffer per face in last check window
  std::map<FaceId, std::list<Name>> m_perFaceInterestBuffer;

  // pushback weight per face
  std::map<FaceId, double> m_pushbackWeight;

  // marked interest number per face under a m_prefix after m_firstNackTimeStamp
  std::map<FaceId, int> m_markedInterestPerFace;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_RECORD_HPP
