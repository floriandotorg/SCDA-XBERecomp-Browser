// The disc copied into the origin private file system, under scda-disc/, so later starts need neither the original
// file nor permission to read it again. disc-id.json names the copied file; it reads 'null' while a copy is under way,
// so an interrupted copy is never offered.
const identityOf = file => JSON.stringify({ name: file.name, size: file.size, modified: file.lastModified });
const missing = error => error?.name === 'NotFoundError';

const readCopy = async root => {
  try {
    const identity = await (await (await root.getFileHandle('disc-id.json')).getFile()).text();
    const file = await (await root.getFileHandle('disc.iso')).getFile();
    return { identity, file };
  } catch (error) {
    if (missing(error)) { return undefined; }
    throw error;
  }
};
const writeText = async (handle, text) => {
  const writer = await handle.createWritable();
  await writer.write(text);
  await writer.close();
};

export const prepareDisc = async (root, file, { estimate, progress = () => {} }) => {
  const identity = identityOf(file);
  const copy = await readCopy(root);
  if (copy?.identity === identity && copy.file.size === file.size) { return copy.file; }
  if (Number.isFinite(estimate?.quota) && Number.isFinite(estimate?.usage) && estimate.quota - estimate.usage < file.size) {
    throw new Error('Not enough browser storage to keep a copy of the disc.');
  }
  const metadata = await root.getFileHandle('disc-id.json', { create: true });
  await writeText(metadata, 'null');
  const handle = await root.getFileHandle('disc.iso', { create: true });
  let copied = 0;
  await file.stream().pipeThrough(new TransformStream({
    transform: (chunk, controller) => {
      copied += chunk.byteLength;
      progress(copied, file.size);
      controller.enqueue(chunk);
    },
  })).pipeTo(await handle.createWritable());
  await writeText(metadata, identity);
  return handle.getFile();
};

export const cachedDisc = async root => {
  const copy = await readCopy(root);
  if (!copy) { return null; }
  try {
    const identity = JSON.parse(copy.identity);
    return identity && Number.isSafeInteger(identity.size) && identity.size > 0 && identity.size === copy.file.size ? copy.file : null;
  } catch (error) {
    if (error instanceof SyntaxError) { return null; }
    throw error;
  }
};

export const createDiscCache = ({ storage = globalThis.navigator?.storage } = {}) => {
  const directory = async () => (await storage.getDirectory()).getDirectoryHandle('scda-disc', { create: true });
  return Object.freeze({
    cached: async () => storage?.getDirectory ? cachedDisc(await directory()) : null,
    store: async (file, progress) => {
      if (!storage?.getDirectory) { return file; }
      void storage.persist?.().catch(() => {});
      return prepareDisc(await directory(), file, { estimate: await storage.estimate?.().catch(() => undefined), progress });
    },
  });
};
