#pragma once
#include <algorithm>
#include <climits>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace ycsbc_ub {
// Controller passes an ordered list, which may contain gaps in CPU numbering.
inline std::vector<int> parse_worker_cpus(const char* text, int count, int first) {
  if(count < 1 || count > 192) throw std::runtime_error("Invalid UB worker CPU count");
  std::vector<int> cpus;
  if(text && *text) {
    const std::string list(text);
    size_t start=0;
    while(start < list.size()) {
      const size_t end=list.find(',',start);
      const std::string token=list.substr(start,end==std::string::npos ? end : end-start);
      if(token.empty() || token.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("Invalid YCSB_UB_CPUS list");
      cpus.push_back(std::stoi(token));
      if(end==std::string::npos) break;
      start=end+1;
      if(start==list.size()) throw std::runtime_error("Invalid trailing comma in YCSB_UB_CPUS");
    }
  } else {
    // Compatibility for older controllers using a consecutive CPU range.
    if(first < 0 || first > INT_MAX-count) throw std::runtime_error("Invalid UB start CPU");
    for(int i=0;i<count;++i) cpus.push_back(first+i);
  }
  if(cpus.size()!=static_cast<size_t>(count))
    throw std::runtime_error("YCSB_UB_CPUS must contain one CPU per local worker");
  for(size_t i=0;i<cpus.size();++i) {
    if(cpus[i]<0 || cpus[i]>=CPU_SETSIZE ||
       std::find(cpus.begin(),cpus.begin()+i,cpus[i])!=cpus.begin()+i)
      throw std::runtime_error("UB worker CPU IDs must be distinct and fit cpu_set_t");
  }
  return cpus;
}
}
