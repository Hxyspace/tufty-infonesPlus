#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace tufty {
// Emulator-thread-only allocation records; no metadata in PSRAM. Free gaps
// coalesce implicitly, and failed loads can release in any order.
class Arena {
 public:
  Arena(void *data,std::size_t bytes):base_(static_cast<std::byte *>(data)),bytes_(bytes){}
  void *allocate(std::size_t n){
    constexpr auto alignment=alignof(std::max_align_t);
    if(!n || n>bytes_ || used_==spans_.size())return nullptr;
    if(n>SIZE_MAX-(alignment-1))return nullptr;
    n=(n+alignment-1)&~(alignment-1);
    std::size_t at=0,index=0;
    for(;index<used_;++index){
      if(n<=spans_[index].offset-at)break;
      at=spans_[index].offset+spans_[index].size;
    }
    if(n>bytes_-at)return nullptr;
    for(auto j=used_;j>index;--j)spans_[j]=spans_[j-1];
    spans_[index]={at,n};++used_;used_bytes_+=n;
    if(used_bytes_>peak_bytes_)peak_bytes_=used_bytes_;
    return base_+at;
  }
  bool contains(const void *p)const{
    const auto at=reinterpret_cast<std::uintptr_t>(p),base=reinterpret_cast<std::uintptr_t>(base_);
    return at>=base && at-base<bytes_;
  }
  void release(void *p){
    for(std::size_t i=0;i<used_;++i)if(base_+spans_[i].offset==p){
      used_bytes_-=spans_[i].size;
      for(auto j=i+1;j<used_;++j)spans_[j-1]=spans_[j];
      --used_;return;
    }
  }
  std::size_t used_bytes()const{return used_bytes_;}
  std::size_t peak_bytes()const{return peak_bytes_;}
 private:
  struct Span {std::size_t offset,size;};
  std::byte *base_;std::size_t bytes_,used_=0,used_bytes_=0,peak_bytes_=0;
  std::array<Span,64> spans_{};
};
}
