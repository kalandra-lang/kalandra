#include <cstdint>
#include <numeric>
#include <vector>
using I=std::int64_t;
#if defined(ARRAY_WIDTH)
static double arrayRounds(std::vector<I> values,I rounds) {
  if(!rounds) {I sum=0;for(auto value:values)sum+=value;return double(sum);}
  std::vector<I> next(values.size());
  for(std::size_t i=0;i<values.size();++i)next[i]=(values[i]*13+7)%1000003;
  return arrayRounds(std::move(next),rounds-1);
}
extern "C" double broad_entry(I seed,I rounds) {
  std::vector<I> values{ARRAY_VALUES};
  return arrayRounds(std::move(values),rounds);
}
#elif defined(AFFINE) || defined(POLYNOMIAL)
static double recurrence(double x,double offset,I rounds) {
  if(!rounds)return x;
#if defined(AFFINE)
  return recurrence(x*0.5+offset,offset,rounds-1);
#else
  return recurrence(x*1.015625+offset,offset,rounds-1);
#endif
}
extern "C" double broad_entry(I seed,I rounds) {
#if defined(AFFINE)
  return recurrence(double(seed)*0.25,double(seed)*0.0625,rounds);
#else
  return recurrence(double(seed)*0.015625,double(seed)*0.00390625,rounds);
#endif
}
#elif defined(EUCLID) || defined(EUCLID_MIXED)
static I gcd(I a,I b) {if(!b)return a;return gcd(b,a%b);}
static double recurrence(I x,I seed,I rounds) {
  if(!rounds)return double(x);
#if defined(EUCLID_MIXED)
  I next=(x*48271+gcd(x*13+7,seed*7+11)+seed)%2147483647;
#else
  I next=gcd(x*13+7,seed*7+11)+seed;
#endif
  return recurrence(next,seed,rounds-1);
}
extern "C" double broad_entry(I seed,I rounds) {return recurrence(seed,seed,rounds);}
#endif
