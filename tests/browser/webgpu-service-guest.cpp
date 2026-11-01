#include "browser/webgpu-transport.h"
#include <emscripten.h>
#include <pthread.h>

namespace {
browser::webgpu::Transport transport;
browser::webgpu::Result result;
uint32_t unbound = 0;
}
extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t gpu_execute(uint32_t operation, uint32_t resource, const uint8_t* bytes, uint32_t size) {
  struct Request { uint32_t operation, resource; const uint8_t* bytes; uint32_t size; } request{operation, resource, bytes, size};
  pthread_t thread;
  const auto created = pthread_create(&thread, nullptr, [](void* data) -> void* {
    auto& request = *static_cast<Request*>(data);
    unbound = EM_ASM_INT({ return typeof Module.webgpuMailbox === 'undefined'; });
    result = transport.Execute(static_cast<browser::webgpu::Opcode>(request.operation), request.resource, {request.bytes, request.size});
    return nullptr;
  }, &request);
  if (created || pthread_join(thread, nullptr)) { return 1; }
  return static_cast<uint32_t>(result.status);
}
EMSCRIPTEN_KEEPALIVE uint32_t gpu_pthread_unbound() { return unbound; }
EMSCRIPTEN_KEEPALIVE const uint8_t* gpu_reply() { return result.bytes.data(); }
EMSCRIPTEN_KEEPALIVE uint32_t gpu_reply_size() { return result.bytes.size(); }
EMSCRIPTEN_KEEPALIVE uint32_t gpu_command() { return result.command; }
}
