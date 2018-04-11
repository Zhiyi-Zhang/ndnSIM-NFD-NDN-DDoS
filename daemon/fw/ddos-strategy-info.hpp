#ifndef NFD_DAEMON_DDOS_STRATEGY_INFO_HPP
#define NFD_DAEMON_DDOS_STRATEGY_INFO_HPP

#include "../face/face.hpp"
#include "strategy-info.hpp"

namespace nfd {
namespace fw {

/**
 * Measurement information that can be saved and retrieved per-name-prefix.
 */
class DDoSStrategyInfo : public StrategyInfo
{
public:
  static constexpr int
  getTypeId()
  {
    return 1013;
  }

  DDoSStrategyInfo()
      : m_ownPrefix("null")
  {
  }


public:
  std::string m_ownPrefix;

  // PIT expiration
  std::list<uint32_t/* timestamp */, pit::Entry> m_expiration;

  // Data
  std::list<uint32_t/* timestamp */, pit::Entry> m_data;

  // Nack
  std::list<uint32_t/* timestamp */, pit::Entry> m_nack;
};

}  //fw
}  //nfd

#endif // NFD_DAEMON_DDOS_STRATEGY_INFO_HPP
