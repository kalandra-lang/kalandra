#include "../lib/RecordStorage.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static ValueType nominal(std::size_t id, bool array = false) {
  ValueType type;
  type.builtin = BuiltinValue::Record;
  type.record = id;
  type.array = type.tensor = array;
  if (array)
    type.shape.push_back({std::nullopt, 1});
  return type;
}
static TypedRecord record(std::initializer_list<ValueType> fields) {
  TypedRecord result;
  result.name = "Test";
  for (const auto &type : fields)
    result.fields.push_back({"field", type, {}});
  return result;
}
static RecordStoragePlan plan(const TypedModule &module) {
  auto result = planRecordStorage(module);
  require(bool(result), "valid graph rejected");
  return *result;
}
static void invalid(const TypedModule &module) {
  auto result = planRecordStorage(module);
  require(!result && result.error().code == "S068", "invalid graph admitted");
}
int main() {
  try {
    TypedModule module;
    require(plan(module).fields.empty(), "empty module");
    module.records = {record({ValueType{}})};
    require(plan(module).constructible[0], "scalar base");
    module.records = {record({nominal(0)})};
    auto self = plan(module);
    require(self.recursive[0] && !self.constructible[0] &&
                self.fields[0][0] == RecordFieldStorage::ManagedReference,
            "uninhabited self cycle");
    // A flattened List enum: tag, head, tail; Nil supplies the finite base.
    module.records = {record({ValueType{}, ValueType{}, nominal(0)})};
    module.records[0].variants = {{"Nil", {}, {}}, {"Cons", {}, {1, 2}}};
    auto list = plan(module);
    require(list.constructible[0] && list.recursive[0] &&
                list.fields[0][2] == RecordFieldStorage::ManagedReference,
            "recursive list");
    module.records[0].variants = {{"Cons", {}, {1, 2}}};
    require(!plan(module).constructible[0], "variant needs finite base");
    module.records = {record({nominal(0, true)})};
    auto array = plan(module);
    require(array.recursive[0] && array.constructible[0] &&
                array.fields[0][0] == RecordFieldStorage::CollectionDescriptor,
            "empty array base");
    module.records = {record({nominal(1, true)}), record({nominal(0)})};
    auto mixed = plan(module);
    require(mixed.component == std::vector<std::size_t>({0, 0}) && mixed.constructible[0] &&
                mixed.constructible[1] && mixed.fields[1][0] == RecordFieldStorage::Inline,
            "descriptor breaks inline cycle");
    module.records = {record({nominal(1), nominal(1)}), record({ValueType{}, nominal(0)})};
    module.records[1].variants = {{"End", {}, {}}, {"More", {}, {1}}};
    auto mutual = plan(module);
    require(mutual.constructible[0] && mutual.constructible[1] && mutual.recursive[0] &&
                mutual.fields[0][1] == RecordFieldStorage::ManagedReference,
            "mutual finite base");
    require(plan(module).component == mutual.component, "deterministic component identity");
    module.records = {record({nominal(1), nominal(2)}), record({nominal(3)}), record({nominal(3)}),
                      record({ValueType{}})};
    auto diamond = plan(module);
    require(diamond.constructible[0] && !diamond.recursive[0] &&
                diamond.fields[0][0] == RecordFieldStorage::Inline,
            "acyclic diamond");
    ValueType callable;
    callable.builtin = BuiltinValue::Function;
    auto signature = std::make_shared<FunctionSignature>();
    signature->result = nominal(0);
    callable.callable = signature;
    module.records = {record({callable})};
    require(!plan(module).recursive[0] && plan(module).constructible[0], "opaque callable handle");
    module.records = {record({nominal(1)})};
    invalid(module);
    module.records = {record({nominal(0)})};
    module.records[0].fields[0].type.record.reset();
    invalid(module);
    module.records = {record({nominal(0, true)})};
    module.records[0].fields[0].type.shape.clear();
    invalid(module);
    module.records = {record({ValueType{}})};
    module.records[0].fields[0].type.record = 0;
    invalid(module);
    module.records = {record({ValueType{}, ValueType{}})};
    module.records[0].variants = {{"Bad", {}, {1, 1}}};
    invalid(module);
    module.records[0].variants = {{"Bad", {}, {2}}};
    invalid(module);
    module.records[0].variants = {{"Bad", {}, {0}}};
    invalid(module);
    module.records[0].variants = {{"Bad", {}, {}}};
    invalid(module);
    module.records = {record({ValueType{}})};
    require(!planRecordStorage(module, 1), "work budget ignored");
    module.records[0].fields.clear();
    invalid(module);
    // Compare all three-node direct graphs against a reachability/fixed-point oracle.
    for (unsigned bits = 0; bits < 512; ++bits) {
      bool reachable[3][3] = {};
      module.records.clear();
      for (std::size_t from = 0; from < 3; ++from) {
        auto node = record({ValueType{}});
        for (std::size_t to = 0; to < 3; ++to)
          if (bits & (1U << (from * 3 + to))) {
            reachable[from][to] = true;
            node.fields.push_back({"edge", nominal(to), {}});
          }
        module.records.push_back(std::move(node));
      }
      for (std::size_t via = 0; via < 3; ++via)
        for (std::size_t from = 0; from < 3; ++from)
          for (std::size_t to = 0; to < 3; ++to)
            reachable[from][to] |= reachable[from][via] && reachable[via][to];
      bool finite[3] = {};
      for (unsigned pass = 0; pass < 3; ++pass)
        for (std::size_t from = 0; from < 3; ++from) {
          bool possible = true;
          for (std::size_t to = 0; to < 3; ++to)
            if (bits & (1U << (from * 3 + to)))
              possible &= finite[to];
          finite[from] |= possible;
        }
      auto graph = plan(module);
      for (std::size_t from = 0; from < 3; ++from) {
        auto canonical = from;
        for (std::size_t to = 0; to < 3; ++to)
          if (reachable[from][to] && reachable[to][from])
            canonical = std::min(canonical, to);
        require(graph.component[from] == canonical &&
                    graph.recursive[from] == reachable[from][from] &&
                    graph.constructible[from] == finite[from],
                "graph oracle disagreement");
        for (std::size_t field = 1; field < module.records[from].fields.size(); ++field) {
          const auto to = *module.records[from].fields[field].type.record;
          require((graph.fields[from][field] == RecordFieldStorage::ManagedReference) ==
                      (reachable[from][to] && reachable[to][from]),
                  "field oracle disagreement");
        }
      }
    }
    // Error-family forwarding must agree on physical references even when
    // boxing is required only by the original family or only by a union user.
    for (const auto &source : {"error E:\n  End\n  More:\n    next:E\nfn marker()->int ! (E | "
                               "TaskError):\n  fail E.End\nfn main()->int:\n  0\n",
                               "type Box:\n  cause:Errors\ntype Errors=A | B\nerror A:\n  Bad:\n   "
                               " box:Box\nerror B:\n  End\nfn main()->int:\n  0\n"}) {
      auto checked = check(parse(std::make_shared<SourceFile>("error-storage.k", source)));
      require(checked.ok(), "recursive error forwarding fixture rejected");
      auto storage = planRecordStorage(*checked.module);
      require(bool(storage), "recursive error forwarding plan failed");
      bool forwarded = false;
      for (std::size_t id = 0; id < checked.module->records.size(); ++id) {
        const auto &record = checked.module->records[id];
        for (std::size_t variant = 0; variant < record.errorOrigins.size(); ++variant) {
          const auto [member, original] = record.errorOrigins[variant];
          const auto &from = checked.module->records[member].variants[original].fields;
          const auto &to = record.variants[variant].fields;
          for (std::size_t n = 0; n < from.size(); ++n)
            if (record.fields[to[n]].type.record) {
              forwarded = true;
              require(storage->fields[id][to[n]] == RecordFieldStorage::ManagedReference &&
                          storage->fields[member][from[n]] == RecordFieldStorage::ManagedReference,
                      "recursive error forwarding changed managed-reference representation");
            }
        }
      }
      require(forwarded, "recursive error fixture omitted nominal forwarding");
      for (auto &record : checked.module->records)
        if (!record.errorOrigins.empty()) {
          record.errorOrigins[0].first = SIZE_MAX;
          require(!planRecordStorage(*checked.module), "invalid error storage origin admitted");
          break;
        }
    }
    // Deep chains and cycles must not use the native call stack.
    constexpr std::size_t count = 10000;
    module.records.clear();
    for (std::size_t id = 0; id < count; ++id)
      module.records.push_back(record({id + 1 < count ? nominal(id + 1) : ValueType{}}));
    require(plan(module).constructible[0], "deep finite chain");
    module.records.back().fields[0].type = nominal(0);
    auto cycle = plan(module);
    require(cycle.component.back() == 0 && !cycle.constructible[0] &&
                cycle.fields.back()[0] == RecordFieldStorage::ManagedReference,
            "deep cycle");
    auto checked = check(parse(
        std::make_shared<SourceFile>("storage.k", "type Point:\n  x:int\nfn main()->int:\n  0\n")));
    require(checked.ok() && bool(planRecordStorage(*checked.module)), "checked source planning");
    auto rejected = check(parse(std::make_shared<SourceFile>(
        "recursive.k", "enum List:\n  Cons:\n    tail:List\nfn main()->int:\n  0\n")));
    require(!rejected.ok(), "unconstructible recursive source admitted");
    require(
        std::ranges::any_of(rejected.diagnostics, [](const auto &d) { return d.code == "S068"; }),
        "recursive diagnostic changed");
    std::cout << "record storage planning passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
