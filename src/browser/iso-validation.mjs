const maxImageBytes = 64 * 2 ** 20;
const maxRequestBytes = 2 ** 20;

export class IsoValidationError extends Error {
  constructor(code, message, identifiers = {}) {
    super(message);
    this.name = 'IsoValidationError';
    this.code = code;
    Object.assign(this, identifiers);
  }
}

const fail = (code, message, identifiers) => { throw new IsoValidationError(code, message, identifiers); };
const checkpoint = (signal) => {
  if (signal?.aborted) { fail('CANCELLED', 'Disc validation was cancelled.'); }
};
const manifestError = () => fail('MANIFEST', 'This build has no supported disc identity and story manifest.');
const storyPath = (path) => {
  if (typeof path !== 'string' || path.length > 240 || /[^\x20-\x7e]|:/.test(path)) { manifestError(); }
  const normalized = path.replaceAll('\\', '/').toLowerCase();
  if (normalized.split('/').some((part) => !part || part === '.' || part === '..') || normalized.endsWith('.xbe')) {
    manifestError();
  }
  return normalized;
};
const snapshotManifest = (manifest) => {
  if (manifest?.schema !== 1 || manifest.image !== 'default.xbe' ||
      typeof manifest.xbeSha256 !== 'string' || !/^[a-f0-9]{64}$/.test(manifest.xbeSha256) ||
      !Number.isSafeInteger(manifest.xbeSize) || manifest.xbeSize < 1 || manifest.xbeSize > maxImageBytes ||
      !Array.isArray(manifest.requiredStoryFiles) || manifest.requiredStoryFiles.length < 1 ||
      manifest.requiredStoryFiles.length > 256) {
    manifestError();
  }
  const seen = new Set();
  const story = Array.from(manifest.requiredStoryFiles, (file) => {
    const path = storyPath(file?.path);
    if (!Number.isSafeInteger(file.minBytes) || file.minBytes < 1 || file.minBytes > 0xffffffff || seen.has(path)) {
      manifestError();
    }
    seen.add(path);
    return { path, minBytes: file.minBytes };
  });
  return { size: manifest.xbeSize, sha256: manifest.xbeSha256, story };
};
const regularFile = (entry) => entry?.dir === false && Number.isSafeInteger(entry.size) &&
  entry.size >= 0 && entry.size <= 0xffffffff;

export const validateDiscImage = async ({ disc, manifest, signal }) => {
  checkpoint(signal);
  const identity = snapshotManifest(manifest);
  if (!globalThis.crypto?.subtle) { fail('CAPABILITY', 'Local executable hashing requires a secure browser context.'); }
  const access = async (path, operation) => {
    checkpoint(signal);
    let result;
    try {
      result = await operation();
    } catch {
      checkpoint(signal);
      fail('READ_FAILED', `Could not read ${path} from the selected disc.`, { path });
    }
    checkpoint(signal);
    return result;
  };
  const image = await access('default.xbe', () => disc.find('default.xbe'));
  if (!regularFile(image)) { fail('MISSING_IMAGE', 'The disc has no story executable.'); }
  if (image.size !== identity.size) { fail('IMAGE_SIZE', 'The executable size does not match this build.'); }
  for (const file of identity.story) {
    const entry = await access(file.path, () => disc.find(file.path));
    if (!regularFile(entry) || entry.size < file.minBytes) {
      fail('MISSING_STORY', `Required story file is missing or truncated: ${file.path}.`, { path: file.path });
    }
  }
  const bytes = new Uint8Array(identity.size);
  for (let n = 0; n < bytes.length; n += maxRequestBytes) {
    const length = Math.min(maxRequestBytes, bytes.length - n);
    const part = await access('default.xbe', () => disc.read('default.xbe', n, length));
    if (!(part instanceof Uint8Array) || part.length !== length) {
      fail('SHORT_READ', 'The selected executable could not be read completely.');
    }
    bytes.set(part, n);
  }
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  checkpoint(signal);
  const sha256 = [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
  if (sha256 !== identity.sha256) {
    fail('IMAGE_HASH', 'The executable version does not match this build.', { expected: identity.sha256, observed: sha256 });
  }
  return { bytes, sha256, storyFiles: identity.story.length };
};
