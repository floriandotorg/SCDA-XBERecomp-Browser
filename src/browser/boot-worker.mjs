import create from './story_boot.mjs';
import manifest from './boot-manifest.mjs';
import { createBootSession } from './boot-session.mjs';

self.onmessage = createBootSession({
  create, manifest,
  postMessage: (message) => self.postMessage(message),
  onProgress: (message) => self.postMessage(message),
});
