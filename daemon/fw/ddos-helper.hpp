#ifndef NFD_DAEMON_FW_DDOS_HELPER_HPP
#define NFD_DAEMON_FW_DDOS_HELPER_HPP

#include "../../../utils/ndn-ns3-packet-tag.hpp"
#include "../../../utils/ndn-ns3-cc-tag.hpp"
#include "../../../../core/model/ptr.h"
#include "mt-forwarding-info.hpp"
#include "algorithm.hpp"

namespace nfd {
namespace fw {

using ndn::Name;

class DdosHelper
{
public:
  // @return the number of pending interests with the same prefix
  // @param the Interest name
  // @param prefix length (component number). Default: FIB entry size or + 2?
  // @note help to calculate the traffic change
  static uint32_t
  getInterestNumberWithPrefix(Forwarder& forwarder,
                              const Name& interestName,
                              const int length)
  {
    Name prefix = interestName.getPrefix(length);
    uint32_t counter = 0;

    const auto& pitTable = forwarder.m_pit;
    for (const auto& entry : pitTable) {
      if (prefix.isPrefixOf(entry.getInterest().getName())) {
        // if the prefix matches the Interest, add the number of inRecords
        counter += entry.getInRecords().size();
      }
    }
    return counter;
  }

  // @return the current PIT table usage rate
  static double
  getCurrentPitTableUsage(Forwarder& forwarder, int maxSize)
  {
    int pitSize = getPitTableSize(forwarder);
    return pitSize/maxSize;
  }

  // @return the current prefix success ratio: suc/all in 1 second
  // @param TODO
  // @param the Interest name
  // @param prefix length (component number). Default: FIB entry size
  static double
  getPrefixSuccessRatio(const Name& interestName,
                        const int length)
  {
    return 0.0;
  }

  // @return whether is under DDoS or not, decided by decision machine
  // @param TODO
  static bool
  isUnderDDoS()
  {
    return false;
  }

public:

  static uint32_t
  getPitTableSize(Forwarder& forwarder)
  {
    const auto& pitTable = forwarder.m_pit;
    return pitTable.size();
  }
};


} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_HELPER_HPP
