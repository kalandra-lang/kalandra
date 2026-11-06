#include "DeviceLiveness.h"
#include <algorithm>
#include <limits>

namespace kalandra {
std::expected<std::vector<std::vector<continuation::Slot>>, std::string>
deviceLiveRoots(const continuation::Plan &plan, std::span<const continuation::Slot> tracked,
                std::uint64_t maxBytes, std::uint64_t maxWork, bool callFrames) try {
  using C = continuation::Code;
  const auto count = plan.code.size(), slots = plan.slots.size();
  if (count > std::numeric_limits<std::uint32_t>::max())
    return std::unexpected("device root map exceeds the instruction ABI");
  if (tracked.size() > slots)
    return std::unexpected("device root slot count exceeds the frame");
  const std::uint64_t words = (tracked.size() + 63) / 64;
  // Charge graph/vector tables, bitsets, work queues and the root-slot index
  // before allocating them. Runtime arrays are unaffected by this compiler cap.
  struct Edge {
    std::uint32_t target;
    std::size_t definition;
  };
  const std::uint64_t row =
      4 * sizeof(std::vector<std::uint64_t>) + 2 * sizeof(Edge) + 8 * sizeof(std::uint32_t) + 1;
  if (slots > maxBytes / sizeof(std::size_t))
    return std::unexpected("device root analysis exceeds its memory budget");
  std::uint64_t retained = slots * sizeof(std::size_t);
  if (count > (maxBytes - retained) / row)
    return std::unexpected("device root analysis exceeds its memory budget");
  retained += count * row;
  if (words > (maxBytes - retained) / 8 / (std::uint64_t(count) + 1))
    return std::unexpected("device root analysis exceeds its memory budget");
  retained += (std::uint64_t(count) + 1) * words * 8;
  if (slots > maxWork)
    return std::unexpected("device root analysis exceeds its work budget");
  maxWork -= slots;
  std::vector<std::size_t> indices(slots, SIZE_MAX);
  for (std::size_t i = 0; i < tracked.size(); ++i) {
    if (tracked[i] >= slots || indices[tracked[i]] != SIZE_MAX)
      return std::unexpected("invalid or duplicate device root slot");
    indices[tracked[i]] = i;
  }
  const auto spend = [&](std::uint64_t work) {
    if (work > maxWork)
      return false;
    maxWork -= work;
    return true;
  };
  std::vector<std::vector<std::uint32_t>> predecessors(count);
  std::vector<std::vector<Edge>> successors(count);
  for (std::size_t pc = 0; pc < count; ++pc) {
    if (!spend(1 + plan.code[pc].args.size()))
      return std::unexpected("device root analysis exceeds its work budget");
    const auto &op = plan.code[pc];
    bool produces = false;
    switch (op.code) {
    case C::SynchronousCall:
      if (!callFrames || pc + 1 >= count)
        return std::unexpected("device root analysis requires a resumable call continuation");
      produces = true;
      break;
    case C::FunctionReference:
    case C::CallableTarget:
    case C::CallableCaptures:
      if (!callFrames)
        return std::unexpected("device root analysis requires a callable continuation");
      produces = true;
      break;
    case C::InvalidCallable:
      if (!callFrames)
        return std::unexpected("device root analysis requires a callable continuation");
      break;
    case C::Literal:
    case C::Unary:
    case C::Binary:
    case C::Kernel:
    case C::Copy:
    case C::Field:
    case C::CollectInit:
    case C::CollectAppend:
    case C::MapInit:
    case C::MapCheck:
    case C::MapLoad:
    case C::MapStore:
    case C::MapAdvance:
      produces = true;
      break;
    case C::Label:
    case C::Branch:
    case C::Jump:
    case C::Return:
    case C::Case:
    case C::CollectFinish:
      break;
    case C::Try:
    case C::Catch:
    case C::Fail:
      break;
    default:
      return std::unexpected("device root analysis requires explicit control-flow effects");
    }
    if (produces && op.output >= slots)
      return std::unexpected("device root definition is out of range");
    for (auto slot : op.args)
      if (slot >= slots)
        return std::unexpected("device root operand is out of range");
    const auto edge = [&](std::size_t target, std::size_t output = SIZE_MAX) -> bool {
      if (target >= count || (output != SIZE_MAX && output >= slots))
        return false;
      successors[pc].push_back({static_cast<std::uint32_t>(target),
                                output == SIZE_MAX ? SIZE_MAX : indices[output]});
      predecessors[target].push_back(pc);
      return true;
    };
    // The success and failure successors define different slots. A previous
    // value in either destination may still be live on the opposite edge.
    if (op.code == C::Try || op.code == C::Fail || op.code == C::Catch) {
      const bool catches = op.code == C::Catch;
      if (op.args.size() != (catches ? 2 : 1) || op.target == SIZE_MAX ||
          (op.code != C::Fail && op.output >= slots) ||
          !edge(op.jump, catches ? op.args[1] : op.target) ||
          (op.code != C::Fail && !edge(catches ? op.target : pc + 1, op.output)))
        return std::unexpected("invalid device error control flow");
      continue;
    }
    if (op.code == C::Jump || op.code == C::Branch || op.code == C::Case) {
      if (!(op.code == C::Case && op.jump == SIZE_MAX) && !edge(op.jump))
        return std::unexpected("device root branch target is out of range");
    }
    if (op.code != C::Jump && op.code != C::Return && op.code != C::InvalidCallable && pc + 1 < count)
      edge(pc + 1, produces ? op.output : SIZE_MAX);
  }
  std::vector<std::vector<std::uint64_t>> live(count, std::vector<std::uint64_t>(words));
  std::vector<std::uint32_t> pending(count);
  std::vector<bool> queued(count, true);
  for (std::size_t i = 0; i < count; ++i)
    pending[i] = count - i - 1;
  std::size_t head = 0, tail = 0, pendingCount = count;
  std::vector<std::uint64_t> next(words);
  while (pendingCount) {
    const auto pc = pending[head];
    head = (head + 1) % count;
    --pendingCount;
    queued[pc] = false;
    if (!spend(1 + words * (2 + successors[pc].size()) + plan.code[pc].args.size()))
      return std::unexpected("device root analysis exceeds its work budget");
    std::fill(next.begin(), next.end(), 0);
    for (auto successor : successors[pc])
      for (std::size_t word = 0; word < words; ++word) {
        auto incoming = live[successor.target][word];
        if (successor.definition != SIZE_MAX && successor.definition / 64 == word)
          incoming &= ~(UINT64_C(1) << (successor.definition % 64));
        next[word] |= incoming;
      }
    const auto &op = plan.code[pc];
    for (auto arg : std::span(op.args).first(op.code == C::Catch ? 1 : op.args.size())) {
      const auto root = indices[arg];
      if (root != SIZE_MAX)
        next[root / 64] |= UINT64_C(1) << (root % 64);
    }
    if (plan.code[pc].code == C::CollectAppend || plan.code[pc].code == C::CollectFinish) {
      const auto slot = plan.code[pc].output;
      if (slot >= slots)
        return std::unexpected("device collection root is out of range");
      const auto root = indices[slot];
      if (root != SIZE_MAX)
        next[root / 64] |= UINT64_C(1) << (root % 64);
    }
    if (live[pc] == next)
      continue;
    live[pc] = next;
    for (auto predecessor : predecessors[pc])
      if (!queued[predecessor]) {
        pending[tail] = predecessor;
        tail = (tail + 1) % count;
        ++pendingCount;
        queued[predecessor] = true;
      }
  }
  std::vector<std::vector<continuation::Slot>> result(count);
  for (std::size_t pc = 0; pc < count; ++pc) {
    if (!spend(tracked.size()) || !spend(tracked.size()))
      return std::unexpected("device root analysis exceeds its work budget");
    std::size_t rootCount = 0;
    for (std::size_t i = 0; i < tracked.size(); ++i)
      rootCount += bool(live[pc][i / 64] & (UINT64_C(1) << (i % 64)));
    if (rootCount > (maxBytes - retained) / sizeof(continuation::Slot))
      return std::unexpected("device root maps exceed their memory budget");
    retained += rootCount * sizeof(continuation::Slot);
    result[pc].reserve(rootCount);
    for (std::size_t i = 0; i < tracked.size(); ++i)
      if (live[pc][i / 64] & (UINT64_C(1) << (i % 64))) {
        result[pc].push_back(tracked[i]);
      }
  }
  return result;
} catch (const std::bad_alloc &) {
  return std::unexpected("device root analysis allocation failed");
}
std::expected<std::vector<continuation::Slot>, std::string>
deviceSuspendedCallRoots(const continuation::Plan &plan, std::size_t call,
                         std::span<const std::vector<continuation::Slot>> live) try {
  if (live.size() != plan.code.size() || call >= live.size() ||
      plan.code[call].code != continuation::Code::SynchronousCall || call + 1 == live.size())
    return std::unexpected("invalid device suspended call root map");
  std::vector<continuation::Slot> roots;
  for (const auto slot : live[call + 1])
    if (slot != plan.code[call].output)
      roots.push_back(slot);
  return roots;
} catch (const std::bad_alloc &) {
  return std::unexpected("device suspended call root map allocation failed");
}
} // namespace kalandra
