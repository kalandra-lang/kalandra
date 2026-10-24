#include "../runtime/StringArray.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" const void *record_array_box(const StringArrayDescriptor *, Invocation *);
extern "C" const void *tree_array_box(const StringArrayDescriptor *, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Cell {
  std::int64_t tag;
  StringDescriptor text;
  StringArrayDescriptor values;
};
struct Tree {
  StringDescriptor name;
  StringArrayDescriptor children;
};
static_assert(sizeof(Cell) == 72 && sizeof(Tree) == 64);
int main() {
  try {
    for (unsigned mode = 0; mode < 16; ++mode) {
      Invocation context, foreign;
      StringDescriptor text;
      kalandra_string_literal_v1(&context, &text, "é", 2);
      auto *numbers = static_cast<std::int64_t *>(context.allocateRecord(16));
      auto *cells = static_cast<Cell *>(context.allocateRecord(4 * sizeof(Cell)));
      require(numbers && cells && !context.status(), "record array fixture allocation failed");
      numbers[0] = 20;
      numbers[1] = 22;
      const StringArrayDescriptor values{numbers, numbers, 0, 2, 1};
      const std::array<Cell, 4> source{{{1, {reinterpret_cast<void *>(0x1234), nullptr, -1}, {}},
                                        {2, text, {}},
                                        {2, text, {}},
                                        {3, {}, values}}};
      std::memcpy(cells, source.data(), sizeof(source));
      auto array = context.lease(cells), chars = context.lease(text.allocated),
           numeric = context.lease(numbers);
      StringArrayDescriptor input{cells, cells, 0, 4, 1};
      switch (mode) {
      case 1:
        input = {};
        break;
      case 2:
        cells[2].tag = 0;
        break;
      case 3:
        cells[2].tag = 4;
        break;
      case 4:
        cells[2].text.byteLength = 100;
        break;
      case 5:
        kalandra_string_literal_v1(&foreign, &cells[2].text, "foreign", 7);
        break;
      case 6:
        input.stride = 2;
        break;
      case 7:
        input.allocated = input.data = reinterpret_cast<void *>(0x1230);
        break;
      default:
        break;
      }
      // Table, four element owners, array owner, outer owner: seven publication allocations.
      if (mode >= 8)
        context.failAllocationAt(context.allocations() + mode - 7);
      auto *root = record_array_box(&input, &context);
      const bool invalid = mode >= 2 && mode <= 7;
      const bool failure = mode >= 8 && mode <= 14;
      if (invalid || failure) {
        require(!root && context.status() == (failure ? 2 : 3), "bad record array was published");
        context.settle();
      } else {
        require(root && !context.status(), "record array publication failed");
        auto retained = context.lease(root);
        require(bool(retained), "record array root lease missing");
        context.settle();
        const bool owned = mode != 1;
        require(array->referenceCount() == (owned ? 2 : 1) &&
                    chars->referenceCount() == (owned ? 3 : 1) &&
                    numeric->referenceCount() == (owned ? 2 : 1),
                "transitive record array edges lost");
        if (owned) {
          auto owner = retained->dependency(0);
          require(bool(owner), "record array owner missing");
          auto first = owner->dependency(2), second = owner->dependency(3);
          require(first && second, "record array element owners missing");
          auto a = first->dependency(0), b = second->dependency(0);
          require(a && b && a->bytes().data() == b->bytes().data() && a->bytes().size() == 2 &&
                      !std::memcmp(a->bytes().data(), "é", 2),
                  "duplicate String bytes lost");
        }
        retained.reset();
      }
      require(array->referenceCount() == 1 && chars->referenceCount() == 1 &&
                  numeric->referenceCount() == 1 && !context.live() &&
                  context.allocations() == context.releases() && !context.callDepth(),
              "record array cleanup leaked");
    }
    for (unsigned depth : {8u, 160u}) {
      Invocation context;
      StringArrayDescriptor children;
      for (unsigned i = 0; i < depth; ++i) {
        auto *cell = static_cast<Tree *>(context.allocateRecord(sizeof(Tree)));
        require(cell, "Tree fixture allocation failed");
        const Tree value{{}, children};
        std::memcpy(cell, &value, sizeof(value));
        children = {cell, cell, 0, 1, 1};
      }
      auto *root = tree_array_box(&children, &context);
      require(root && !context.status(), "valid deep array retained a logical call cap");
      require(!context.callDepth(), "array callbacks leaked call slots");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "deep array cleanup leaked");
    }
    {
      Invocation context;
      require(!kalandra_recursive_record_array_dependency_v1(&context, nullptr, 0, 0, nullptr, 0) &&
                  !context.status(),
              "inactive record array metadata inspected");
      require(!kalandra_recursive_record_array_dependency_v1(&context, nullptr, 0, 0, nullptr, 1) &&
                  context.status() == 3,
              "null active record array admitted");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation context;
      const auto count = mode == 0 ? Invocation::maxRecursiveDependencies : 1;
      auto *cells = context.allocateRecord(count * sizeof(Cell));
      require(cells, "record array boundary fixture allocation failed");
      StringArrayDescriptor input{cells, cells, 0, static_cast<std::int64_t>(count), 1};
      KalandraRecursiveRecordCell callback = [](const void *, Invocation *) -> const void * {
        return nullptr;
      };
      if (mode == 2)
        callback = [](const void *, Invocation *context) -> const void * {
          context->fault(4);
          return nullptr;
        };
      if (mode == 3)
        callback = [](const void *, Invocation *context) -> const void * {
          context->expectedTaskError(1, 4, 0, 0);
          return nullptr;
        };
      require(!kalandra_recursive_record_array_dependency_v1(&context, &input, sizeof(Cell),
                                                             alignof(Cell), callback, 1),
              "failed callback or over-budget record array accepted");
      require(mode == 3 ? context.hasExpectedError() && !context.status()
                        : context.status() == (mode == 0   ? 2
                                               : mode == 1 ? 3
                                                           : 4),
              "record array callback fault/resource boundary changed");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "record array callback failure leaked temporary storage");
    }
    std::cout << "recursive record array bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
