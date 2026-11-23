#pragma once
#include <expected>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <utility>

namespace kalandra::runtime::bundle {
enum class Backend { Cuda, Vulkan };
constexpr std::string_view name(Backend backend) {
  return backend == Backend::Cuda ? "cuda" : "vulkan";
}

// Own the admitted context itself. A successful probe never opens a second
// context for execution, and a failed first use is just as final as a success.
template <class Context, class Error> class Selection {
public:
  using Result = std::expected<Context, Error>;
  template <class Override, class Probe>
  const Result &get(Override &&override, Probe &&probe) noexcept {
    std::call_once(once_, [&] {
      const std::string_view overrideName = override();
      for (auto backend : {Backend::Cuda, Backend::Vulkan}) {
        if (!overrideName.empty() && overrideName != name(backend))
          continue;
        result_ = probe(backend);
        if (result_ ||
            (result_.error() != Error::Unavailable && result_.error() != Error::Unsupported))
          return;
      }
      if (!overrideName.empty() && overrideName != "cuda" && overrideName != "vulkan")
        result_ = std::unexpected(Error::Invalid);
    });
    return result_;
  }

private:
  std::once_flag once_;
  Result result_{std::unexpected(Error::Unavailable)};
};
} // namespace kalandra::runtime::bundle
