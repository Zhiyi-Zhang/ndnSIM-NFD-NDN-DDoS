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

public:
  Name m_prefix;

  DDoSType m_type;

  // count how many DDoS records has been received after the first one
  int m_counter;

  // the timestamp when the first nack comes
  int m_firstNackTimeStamp;

public: // expectations

  // interest number per second
  int m_expectedSendingRate;

  std::map<FaceId, double> m_pushbackWeight;

public: // counters
  // marked interest number per face under a m_prefix after m_firstNackTimeStamp
  std::map<FaceId, int> m_markedInterestPerFace;
};

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_RECORD_HPP
