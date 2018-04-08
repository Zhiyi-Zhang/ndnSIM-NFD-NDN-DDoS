#ifndef NFD_DAEMON_FW_MT_FORWARDING_INFO_HPP
#define NFD_DAEMON_FW_MT_FORWARDING_INFO_HPP

#include "../face/face.hpp"
#include "strategy-info.hpp"

namespace nfd {
namespace fw {

/**
 * Measurement information that can be saved and retrieved per-name-prefix.
 */
class MtForwardingInfo : public StrategyInfo
{
public:

  static constexpr int
  getTypeId()
  {
    return 1013;
  }

  MtForwardingInfo()
      : m_ownPrefix("null")
  {
  }

  double
  getRttAvg(FaceId face)
  {
    if (m_RttAvgMap.find(face) == m_RttAvgMap.end()) {
      std::cout << time::steady_clock::now().time_since_epoch().count() / 1000 * 1000
          << " ms, couldn't find face " << face << "\n";
    }
    assert(m_RttAvgMap.find(face) != m_RttAvgMap.end());
    double RttAvg = m_RttAvgMap.at(face);

    return RttAvg;
  }

  void
  setRttAvg(FaceId faceId, double avg)
  {
    m_RttAvgMap[faceId] = avg;
  }

  const std::map<FaceId, double>
  getRttAvgMap() const
  {
    return m_RttAvgMap;
  }

  int
  getFaceCount()
  {
    std::vector<FaceId> faceIdList;
    for (auto faceInfo : m_RttAvgMap) {
      faceIdList.push_back(faceInfo.first);
    }
    return faceIdList.size();
  }

  void
  setPrefix(std::string prefix)
  {
    m_ownPrefix = prefix;
  }

  std::string
  getPrefix() const
  {
    return m_ownPrefix;
  }

private:
  std::string m_ownPrefix;
  std::map<FaceId, double> m_RttAvgMap;

};

}
}

#endif

