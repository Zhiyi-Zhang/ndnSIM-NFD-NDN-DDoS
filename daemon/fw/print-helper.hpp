#ifndef NFD_DAEMON_FW_PRINT_HELPER_HPP
#define NFD_DAEMON_FW_PRINT_HELPER_HPP

namespace nfd {
namespace fw {

class PrintHelper
{
static void
  printFIB(Fib& fib)
  {
    Fib::const_iterator it = fib.begin();
    while (it != fib.end()) {
      std::cout << it->getPrefix() << ": ";
      for (auto b : it->getNextHops()) {
        std::cout << b.getFace().getLocalUri() << b.getFace().getId() << ", local:" << ", ";
      }
      std::cout << "\n";
      it++;
    }
  }

  static void
  printFIBEntry(const shared_ptr<fib::Entry> entry)
  {
    for (auto f : entry->getNextHops()) {
      std::cout << f.getFace().getLocalUri() << f.getFace().getId() << ", ";
    }
    std::cout << "\n";
  }

  static void
  printFwPerc(shared_ptr<std::ostream> os,
              uint32_t nodeId,
              std::string prefix,
              FaceId faceId,
              std::string type,
              double fwPerc)
  {
    *os << ns3::Simulator::Now().ToDouble(ns3::Time::S)
        << "\t" << nodeId
        << "\t" << prefix
        << "\t" << faceId
        << "\t" << type
        << "\t" << fwPerc << "\n";
    // os->flush();
  }
}

} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_PRINT_HELPER_HPP
