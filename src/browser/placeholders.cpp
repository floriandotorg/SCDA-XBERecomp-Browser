#include "cpu/x86_ops.h"
#include "diagnostic_service.h"

void x86_icall(X86Context&, uint8_t*, uint32_t target) {
  throw DiagnosticService{"x86_icall", target, "M2 indirect dispatch unavailable"};
}

void x86_resume(X86Context&, uint8_t*, const X86Resume& resume) {
  throw DiagnosticService{"x86_resume", resume.target, "M2 continuation dispatch unavailable"};
}

void x86_kernel(X86Context&, uint8_t*, uint32_t ordinal) {
  throw DiagnosticService{"x86_kernel", ordinal, "M3 kernel ordinal unavailable"};
}

[[noreturn]] void x86_trap(X86Context&, uint32_t address, const char* detail) {
  throw DiagnosticService{"x86_trap", address, detail};
}

void x86_priv(X86Context&, uint32_t address, const char* detail) {
  throw DiagnosticService{"x86_priv", address, detail};
}

uint64_t x86_rdtsc() {
  throw DiagnosticService{"x86_rdtsc", 0, "M2 guest clock unavailable"};
}

void x86_cpuid(X86Context&) {
  throw DiagnosticService{"x86_cpuid", 0, "M2 CPU feature policy unavailable"};
}
