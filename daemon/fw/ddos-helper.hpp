#ifndef NFD_DAEMON_FW_DDOS_HELPER_HPP
#define NFD_DAEMON_FW_DDOS_HELPER_HPP

#include "../../../../core/model/ptr.h"
#include "algorithm.hpp"

namespace nfd {
namespace fw {

using ndn::Name;

class DDoSHelper
{
public: // DDoS Related

  // @return the number of pending interests with the same prefix of each incoming face
  // @param the Interest name
  // @param prefix length (component number). Default: FIB entry size or + 2?
  // @note help to calculate the traffic volume and volume change
  static std::map<FaceId, uint32_t>
  getInterestNumberWithPrefix(Forwarder& forwarder,
                              const Name& interestName,
                              const int length)
  {
    std::map<FaceId, uint32_t> result;
    Name prefix = interestName.getPrefix(length);
    uint32_t counter = 0;

    const auto& pitTable = forwarder.m_pit;
    for (const auto& entry : pitTable) {
      if (prefix.isPrefixOf(entry.getInterest().getName())) {
        for (const auto& record : entry.getInRecords()) {
          auto search = result.find(record.getFace().getId());
          if(search != result.end()) {
            result[record.getFace().getId()]++;
          }
          else {
            result[record.getFace().getId()] = 1;
          }
        }
      }
    }
    return result;
  }

  // @return the current PIT table usage rate
  static double
  getCurrentPitTableUsage(Forwarder& forwarder, int maxSize = 300)
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
    // TODO
    return 0.0;
  }

  static std::map<FaceId, double>
  getFacePrefixSuccessRatio(const Name& interestName,
                            const int length)
  {
    // TODO
    std::map<FaceId, double> result;
    return result;
  }

  // @return whether is under DDoS or not, decided by decision machine
  // @param TODO
  static bool
  isUnderDDoS()
  {
    // TODO
    return false;
  }

public: // general APIs

  static uint32_t
  getPitTableSize(Forwarder& forwarder)
  {
    const auto& pitTable = forwarder.m_pit;
    return pitTable.size();
  }

  static uint32_t
  getTime()
  {
    return ndn::time::steady_clock::now().time_since_epoch().count() / 1000000;
  }
};


} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_DDOS_HELPER_HPP
