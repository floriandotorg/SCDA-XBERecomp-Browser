import { BoundedIsoReader, IsoReadError } from './iso-reader.mjs';
import { ISO_MAILBOX, isoMailboxHeader, finishIsoRead } from './iso-mailbox.mjs';

export const installIsoIoService = (control) => {
  let reader;
  let generation = 0;
  let nextRequestId = 0;
  let closed = false;
  const connections = new Set();

  const disconnect = (connection) => {
    connections.delete(connection);
    const header = isoMailboxHeader(connection.mailbox);
    finishIsoRead(connection.mailbox, Atomics.load(header, 1), Atomics.load(header, 2), undefined, 'CANCELLED');
    if (connection.current) {
      reader.cancel(connection.current.internalId, connection.generation);
    }
    connection.port.close();
  };

  const receiveGuest = async (connection, message) => {
    if (!message || !Number.isInteger(message.requestId) || message.requestId < 1 || message.requestId > 0x7fffffff) {
      disconnect(connection);
      return;
    }
    const { requestId } = message;
    if (message.type === 'cancel') {
      if (connection.current?.requestId === requestId && message.generation === connection.generation) {
        reader.cancel(connection.current.internalId, connection.generation);
      }
      return;
    }
    if (message.type !== 'read') {
      disconnect(connection);
      return;
    }
    const finish = (bytes, code) => finishIsoRead(connection.mailbox, requestId, message.generation, bytes, code);
    if (!connections.has(connection) || connection.generation !== generation || message.generation !== generation) {
      finish(undefined, 'STALE');
      return;
    }
    if (connection.current) {
      finish(undefined, 'BUSY');
      return;
    }
    if (requestId <= connection.lastRequestId) {
      finish(undefined, 'REQUEST');
      return;
    }
    connection.lastRequestId = requestId;
    const header = isoMailboxHeader(connection.mailbox);
    if (Atomics.load(header, 0) !== ISO_MAILBOX.pending || Atomics.load(header, 1) !== requestId ||
        Atomics.load(header, 2) !== generation) {
      return;
    }
    if (!Number.isSafeInteger(message.length) || message.length < 0 ||
        message.length > connection.mailbox.byteLength - ISO_MAILBOX.headerBytes) {
      finish(undefined, 'RANGE');
      return;
    }
    const job = { requestId, internalId: ++nextRequestId };
    connection.current = job;
    try {
      const bytes = await reader.readAt(message.offset, message.length, job.internalId, generation);
      if (connection.generation === generation && connections.has(connection)) {
        finish(bytes);
      }
    } catch (error) {
      finish(undefined, error instanceof IsoReadError ? error.code : 'READ_FAILED');
    } finally {
      if (connection.current === job) {
        connection.current = undefined;
      }
    }
  };

  control.addEventListener('message', ({ data: message }) => {
    try {
      if (!message || closed) {
        throw new IsoReadError('CLOSED', 'ISO I/O service is closed.');
      }
      switch (message.type) {
        case 'mount': {
          if (reader) {
            throw new IsoReadError('FILE', 'Stop the I/O service before replacing its selected File.');
          }
          const mounted = new BoundedIsoReader(message.file);
          mounted.beginGeneration(message.generation);
          reader = mounted;
          generation = message.generation;
          break;
        }
        case 'generation': {
          if (!reader) {
            throw new IsoReadError('FILE', 'Select a local ISO before creating a runtime.');
          }
          reader.beginGeneration(message.generation);
          generation = message.generation;
          nextRequestId = 0;
          for (const connection of connections) {
            disconnect(connection);
          }
          break;
        }
        case 'connect': {
          if (!reader || message.generation !== generation) {
            throw new IsoReadError('STALE', 'ISO bridge belongs to an inactive runtime.');
          }
          if (connections.size >= 2) {
            throw new IsoReadError('BUSY', 'ISO guest connection budget is exhausted.');
          }
          isoMailboxHeader(message.mailbox);
          if (!message.port || typeof message.port.start !== 'function' || typeof message.port.close !== 'function') {
            throw new IsoReadError('REQUEST', 'A guest completion MessagePort is required.');
          }
          const connection = { port: message.port, mailbox: message.mailbox, generation, current: undefined, lastRequestId: 0 };
          connections.add(connection);
          connection.port.addEventListener('message', ({ data }) => { void receiveGuest(connection, data); });
          connection.port.addEventListener('messageerror', () => disconnect(connection));
          connection.port.start();
          break;
        }
        case 'stats': {
          break;
        }
        case 'close': {
          closed = true;
          reader?.close();
          for (const connection of connections) {
            disconnect(connection);
          }
          break;
        }
        default: {
          throw new IsoReadError('REQUEST', 'Unknown ISO control request.');
        }
      }
      control.postMessage({ id: message.id, ok: true, stats: reader?.stats });
    } catch (error) {
      message?.port?.close();
      control.postMessage({ id: message?.id, ok: false, error: {
        code: error instanceof IsoReadError ? error.code : 'REQUEST',
        message: error instanceof IsoReadError ? error.message : 'Invalid ISO control request.',
      } });
    }
  });
  control.start?.();
};
