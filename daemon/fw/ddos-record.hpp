#ifndef NFD_DAEMON_FW_DDOS_RECORD_HPP
#define NFD_DAEMON_FW_DDOS_RECORD_HPP

#include "face/face.hpp"
#include <map>

namespace nfd {
namespace fw {

class DDoSRecord
{
public: // essential field
  Name m_prefix;

  // count how many DDoS records has been received after the first one
  int m_fakeNackCounter;
  int m_validNackCounter;

  // interest number per second
  // should be reset when new nack arrives
  int m_fakeInterestTolerance;

public: // for consumer router's use ONLY

  // used by revert Event
  int m_revertTimerCounter;

  // interest buffer per face in last check window
  std::map<FaceId, std::list<Interest>> m_perFaceInterestBuffer;

  // has rate limiting started?
  bool m_rateLimiting;

  // if the counter == 3, meaning for 3 checks their is no new nack comes in
  // then we can remove the record
  // should be reset when new nack arrives
  int m_additiveIncreaseCounter;

  // the step of additive step, default to be tolerance / 3 + 1
  int m_additiveIncreaseStep;

public: // for push back

  // pushback weight per face
  std::map<FaceId, double> m_pushbackWeight;

  // marked interest number per face under a m_prefix after m_firstNackTimeStamp
  std::map<FaceId, int> m_markedInterestPerFace;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_RECORD_HPP
