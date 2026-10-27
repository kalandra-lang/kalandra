"""C++ checks shared by the typed and strict-loop ABI regression matrices."""
def pair_checks(first, second):
    return r'''
#include <type_traits>
#if !defined(SKIP_PAIR)
struct KalandraOrdinaryPair {std::uint64_t bits,code;};
extern "C" KalandraOrdinaryPair _mlir_ciface_kalandra_test_entry_ordinary_pair_v1(FIRST,SECOND,kalandra::runtime::OrdinaryFault*);
#endif
#if defined(TEST_DIRECT)
extern "C" T _mlir_ciface_kalandra_test_entry_ordinary_direct_v1(FIRST,SECOND);
#endif
template<class V> V pairValue(std::uint64_t bits) {
 if constexpr(std::is_floating_point_v<V>) {
  using U=std::conditional_t<sizeof(V)==4,std::uint32_t,std::uint64_t>;
  return std::bit_cast<V>(U(bits));
 } else if constexpr(std::is_same_v<V,bool>) {
  if(bits>1)std::abort();return bool(bits);
 } else {
  using U=std::make_unsigned_t<V>;return std::bit_cast<V>(U(bits));
 }
}
void checkPair(FIRST a,SECOND b,T reference,kalandra::runtime::Invocation &context) {
#if !defined(SKIP_PAIR)
 kalandra::runtime::OrdinaryFault fault;std::memset(&fault,0xa5,sizeof(fault));
 auto result=_mlir_ciface_kalandra_test_entry_ordinary_pair_v1(a,b,&fault);
 if(result.code!=context.status())std::abort();
 if(result.code) {
  if(fault.code!=result.code || fault.line!=context.line() || fault.column!=context.column())std::abort();
 } else {auto value=pairValue<T>(result.bits);if(std::memcmp(&value,&reference,sizeof(T)))std::abort();
 }
#endif
#if defined(TEST_DIRECT)
 if(!context.status()){auto direct=_mlir_ciface_kalandra_test_entry_ordinary_direct_v1(a,b);if(std::memcmp(&direct,&reference,sizeof(T)))std::abort();}
#endif
}
'''.replace('FIRST',first).replace('SECOND',second)
