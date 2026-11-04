#include "browser/shader-inventory.h"
#include <cstdio>
int main() {
  browser::ShaderInventory inventory;
  gpu::State state;
  state.vs = std::make_shared<gpu::VertexShader>();
  state.vs_handle = 1;
  state.vs->program = {0, (9u << 21) | (3u << 25), 0, 0, 0, (1u << 21), 0, 1, 0, (13u << 21), 0, 1};
  state.ps = std::make_shared<gpu::PixelShader>();
  state.rs[gpu::kRsPsTextureModes] = 1 | (4u << 5);
  state.textures[0] = 0x10000;
  inventory.Observe(state, false);
  inventory.Texture(0x0e);
  inventory.Texture(0x2e);
  if (inventory.macMask != ((1u << 9) | (1u << 1)) || inventory.iluMask != ((1u << 3) | 1) ||
      inventory.textureModes != (1 | (1u << 1) | (1u << 4)) || inventory.paths != 12 ||
      inventory.formats[0] != (1u << 14) || inventory.formats[1] != (1u << 14)) { return 1; }
  gpu::TexInfo textures[4]{};
  textures[0].kind = gpu::kTexCube;
  browser::ShaderInventory cube;
  cube.Observe(state, false, textures);
  if (cube.textureModes != (1 | (1u << 3) | (1u << 4))) { return 3; }
  state.vs.reset(); state.ps.reset(); state.vs_handle = 0x44;
  inventory.Observe(state, true);
  if (inventory.paths != 13) { return 2; }
  std::puts("shader-inventory-contracts-ok");
}
