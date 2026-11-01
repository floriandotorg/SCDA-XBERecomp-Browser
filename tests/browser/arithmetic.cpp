#include "arithmetic.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

const auto check = [](bool valid, const ArithmeticFixture& fixture, uint32_t value, unsigned count, const char* flag) {
  if (!valid) {
    std::fprintf(stderr, "%s%u value=%08X count=%u %s\n", fixture.operation, fixture.width, value, count, flag);
    throw std::runtime_error("emitted arithmetic differs from instruction oracle");
  }
};

int main() {
  try {
    size_t cases = 0;
    const std::array<uint32_t, 12> values = {0, 1, 2, 0x7F, 0x80, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xA53C96E1};
    for (const auto& fixture : ArithmeticFixtures()) {
      const unsigned width = fixture.width;
      const uint64_t mask = (uint64_t(1) << width) - 1;
      const std::string_view operation = fixture.operation;
      for (uint32_t value : values) {
        for (unsigned n = 0; n < 256; ++n) {
          for (unsigned flags = 0; flags < 4; ++flags) {
            const unsigned inputCount = fixture.count < 0 ? n : unsigned(fixture.count);
            const unsigned count = inputCount & 31;
            const uint64_t input = (value >> fixture.shift) & mask;
            const unsigned initialCarry = flags & 1;
            const unsigned initialOverflow = flags >> 1;
            X86Context context{};
            context.eax = context.edx = 0xC35A96E1;
            context.ecx = n;
            context.*fixture.destination = value;
            context.cf = initialCarry;
            context.of = initialOverflow;
            context.sf = 1;
            context.zf = 1;
            context.pf = 1;
            fixture.function(context, nullptr);
            uint64_t expected = input;
            unsigned carry = initialCarry;
            unsigned overflow = initialOverflow;
            const bool shift = operation == "shl" || operation == "shr" || operation == "sar" || operation == "shld" || operation == "shrd";
            unsigned effective = count;
            const bool arithmetic = operation == "add" || operation == "adc" || operation == "sub" || operation == "sbb" || operation == "neg" || operation == "inc" || operation == "dec";
            if (arithmetic) {
              const uint64_t operand = inputCount & mask;
              const uint64_t sign = uint64_t(1) << (width - 1);
              const auto signedValue = [&](uint64_t value) { return int64_t(value) - ((value & sign) ? int64_t(mask + 1) : 0); };
              int64_t signedResult = 0;
              if (operation == "add" || operation == "adc") {
                const unsigned addCarry = operation == "adc" ? initialCarry : 0;
                const uint64_t result = input + operand + addCarry;
                expected = result & mask;
                carry = result > mask;
                signedResult = signedValue(input) + signedValue(operand) + addCarry;
              } else if (operation == "sub" || operation == "sbb") {
                const unsigned borrow = operation == "sbb" ? initialCarry : 0;
                expected = (input - operand - borrow) & mask;
                carry = input < operand + borrow;
                signedResult = signedValue(input) - signedValue(operand) - borrow;
              } else if (operation == "neg") {
                expected = (0 - input) & mask;
                carry = input != 0;
                signedResult = -signedValue(input);
              } else {
                const int delta = operation == "inc" ? 1 : -1;
                expected = uint64_t(int64_t(input) + delta) & mask;
                signedResult = signedValue(input) + delta;
              }
              overflow = signedResult < -int64_t(sign) || signedResult > int64_t(sign) - 1;
            } else if (count) {
              if (operation == "shl") {
                expected = (input << count) & mask;
                if (count <= width) { carry = (input >> (width - count)) & 1; }
                overflow = unsigned(expected >> (width - 1)) ^ carry;
              } else if (operation == "shr" || operation == "sar") {
                if (operation == "sar") {
                  const int64_t signedValue = int64_t(input) - ((input >> (width - 1)) ? int64_t(uint64_t(1) << width) : 0);
                  expected = uint64_t(signedValue >> count) & mask;
                } else { expected = input >> count; }
                if (count <= width) { carry = (input >> (count - 1)) & 1; }
                overflow = operation == "sar" ? 0 : unsigned(input >> (width - 1));
              } else if (operation == "shld" || operation == "shrd") {
                const uint64_t source = 0xC35A96E1;
                if (operation == "shld") {
                  expected = ((input << count) | (source >> (32 - count))) & mask;
                  carry = (input >> (32 - count)) & 1;
                } else {
                  expected = ((input >> count) | (source << (32 - count))) & mask;
                  carry = (input >> (count - 1)) & 1;
                }
                overflow = unsigned((expected ^ input) >> 31);
              } else {
                const bool throughCarry = operation == "rcl" || operation == "rcr";
                const bool left = operation == "rol" || operation == "rcl";
                const unsigned ringWidth = width + unsigned(throughCarry);
                const uint64_t ringMask = (uint64_t(1) << ringWidth) - 1;
                const uint64_t ring = input | (throughCarry ? uint64_t(initialCarry) << width : 0);
                effective = count % ringWidth;
                uint64_t rotated = ring;
                if (effective) {
                  rotated = left ? ((ring << effective) | (ring >> (ringWidth - effective))) & ringMask
                                 : ((ring >> effective) | (ring << (ringWidth - effective))) & ringMask;
                }
                expected = rotated & mask;
                if (throughCarry) { carry = unsigned(rotated >> width); }
                else { carry = left ? unsigned(expected & 1) : unsigned(expected >> (width - 1)); }
                overflow = left ? unsigned(expected >> (width - 1)) ^ carry
                                : unsigned((expected >> (width - 1)) ^ (expected >> (width - 2))) & 1;
              }
            }
            const uint32_t destinationMask = uint32_t(mask << fixture.shift);
            const uint32_t expectedRegister = (value & ~destinationMask) | uint32_t(expected << fixture.shift);
            check(context.*fixture.destination == expectedRegister, fixture, value, inputCount, "register width and result");
            if (!shift || count < width) { check(context.cf == carry, fixture, value, inputCount, "carry"); }
            if (arithmetic || !count || count == 1 || ((operation == "rcl" || operation == "rcr") && !effective)) {
              const unsigned expectedOverflow = count && !effective ? initialOverflow : overflow;
              check(context.of == expectedOverflow, fixture, value, inputCount, "overflow");
            }
            if (arithmetic || (shift && count)) {
              unsigned parity = 1;
              for (unsigned bit = 0; bit < 8; ++bit) { parity ^= unsigned(expected >> bit) & 1; }
              check(context.zf == (expected == 0) && context.sf == (expected >> (width - 1)) && context.pf == parity, fixture, value, inputCount, "sign zero parity");
            } else {
              check(context.zf == 1 && context.sf == 1 && context.pf == 1, fixture, value, inputCount, "preserved sign zero parity");
            }
            ++cases;
          }
          if (fixture.count >= 0) { break; }
        }
      }
    }
    std::printf("arithmetic-contracts-ok fixtures=%zu cases=%zu\n", ArithmeticFixtures().size(), cases);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Arithmetic fixture: %s\n", error.what());
    return 1;
  }
}
