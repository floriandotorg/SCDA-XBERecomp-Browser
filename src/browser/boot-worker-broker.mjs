export const messageTransfers = (data, ports = []) => {
  const transfers = new Set(ports);
  const visited = new Set();
  const visit = (value) => {
    if (!value || typeof value !== 'object' || visited.has(value)) { return; }
    visited.add(value);
    if (value instanceof ArrayBuffer || value instanceof MessagePort) {
      transfers.add(value);
    } else if (ArrayBuffer.isView(value)) {
      if (value.buffer instanceof ArrayBuffer) { transfers.add(value.buffer); }
    } else if (Array.isArray(value) || Object.getPrototypeOf(value) === Object.prototype) {
      for (const entry of Object.values(value)) { visit(entry); }
    } else if (value instanceof Map) {
      for (const [key, entry] of value) { visit(key); visit(entry); }
    } else if (value instanceof Set) {
      for (const entry of value) { visit(entry); }
    }
  };
  visit(data);
  return [...transfers];
};

export const createWorkerBroker = ({ WorkerClass = Worker, hostUrl = new URL('./boot-worker-host.mjs', import.meta.url) } = {}) => {
  const records = new Set();
  let retired = false;
  const terminate = (record) => {
    if (!records.has(record)) { return; }
    record.worker.terminate();
    record.control.close();
    record.remote?.close();
    records.delete(record);
  };
  const create = (entry, options = {}, onMessage = () => {}, onError = () => {}, remote) => {
    if (retired) { throw new Error('Worker broker is retired.'); }
    const url = new URL(entry, hostUrl);
    if (url.origin !== hostUrl.origin || !url.pathname.startsWith(new URL('.', hostUrl).pathname)) {
      throw new Error('Worker entry is outside the boot artifact directory.');
    }
    const worker = new WorkerClass(hostUrl, { ...options, type: 'module' });
    const channel = new MessageChannel();
    const record = { worker, control: channel.port1, remote };
    records.add(record);
    const live = () => !retired && records.has(record);
    const fail = (error) => {
      if (!live()) { return; }
      const data = { message: error.message ?? 'Worker failed.', filename: error.filename ?? '', lineno: error.lineno ?? 0 };
      if (remote) { remote.postMessage({ type: 'error', data }); }
      else { onError(data); }
    };
    worker.addEventListener('message', (event) => {
      if (!live()) { return; }
      if (remote) { remote.postMessage({ type: 'message', data: event.data }, messageTransfers(event.data, event.ports)); }
      else { onMessage(event.data); }
    });
    worker.addEventListener('error', fail);
    worker.addEventListener('messageerror', () => fail({ message: 'Worker message could not be decoded.' }));
    channel.port1.onmessage = ({ data }) => {
      if (!live()) { data?.port?.close(); return; }
      if (data?.type !== 'create' || !data.port) { fail({ message: 'Invalid worker broker request.' }); return; }
      try { create(data.entry, data.options, undefined, undefined, data.port); }
      catch (error) { data.port.postMessage({ type: 'error', data: { message: error.message } }); }
    };
    if (remote) {
      remote.onmessage = ({ data }) => {
        if (!live()) { return; }
        if (data.type === 'terminate') { terminate(record); }
        else if (data.type === 'post') {
          try { worker.postMessage(data.data, data.transfer); }
          catch (error) { fail(error); }
        }
      };
    }
    try { worker.postMessage({ type: 'boot-worker-init', entry: url.href, control: channel.port2 }, [channel.port2]); }
    catch (error) { terminate(record); channel.port2.close(); throw error; }
    return Object.freeze({
      postMessage: (data, transfer = []) => {
        if (!live()) { throw new Error('Worker broker is retired.'); }
        worker.postMessage(data, transfer);
      },
      terminate: () => terminate(record),
    });
  };
  return Object.freeze({
    create,
    terminateAll: async () => {
      retired = true;
      const errors = [];
      for (const record of records) {
        try { terminate(record); }
        catch (error) { errors.push(error); }
      }
      if (errors.length) { throw new AggregateError(errors, 'Worker termination failed.'); }
    },
  });
};
