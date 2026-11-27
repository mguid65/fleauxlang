// libFuzzer harness for the bytecode loader's trust boundary: any bytes as a .fleaux.bc file, read, verified as the
// loader verifies a file before linking it, and disassembled when it verifies.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <sstream>
#include <vector>

#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/bytecode/verifier.hpp"
#include "fleaux/common/fnv1a.hpp"

namespace {

// Where a file's checksum is, and the first byte it covers.
constexpr std::size_t kChecksumOffset = 8U;
constexpr std::size_t kPayloadOffset = 16U;

}  // namespace

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) -> int {
  std::vector<std::uint8_t> bytes(data, data + size);
  // A file is refused when its checksum does not match, which a mutated input almost never does. The checksum is no
  // defense, since anyone can write a valid one, so the harness writes it, and the fuzzer reaches what follows.
  if (bytes.size() >= kPayloadOffset) {
    const std::uint64_t checksum = fleaux::common::fnv1a(std::span<const std::uint8_t>{bytes}.subspan(kPayloadOffset));
    std::memcpy(bytes.data() + kChecksumOffset, &checksum, sizeof checksum);
  }
  const auto module = fleaux::bytecode::deserialize_module(bytes);
  if (!module.has_value()) {
    return 0;
  }
  if (!fleaux::bytecode::verify_module_operands(*module) || !fleaux::bytecode::verify_module_builtin_callsites(*module) ||
      !fleaux::bytecode::verify_module_locations(*module)) {
    return 0;
  }
  std::ostringstream disassembly;
  (void)fleaux::bytecode::disassemble_module(*module, disassembly);
  return 0;
}
