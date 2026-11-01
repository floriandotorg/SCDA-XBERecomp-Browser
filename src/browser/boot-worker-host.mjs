const pending = [];
let initialized = false;
let ready = false;

const installWorkerProxy = (control) => {
  globalThis.Worker = class extends EventTarget {
    onmessage = null;
    onerror = null;
    #port;
    #terminated = false;
    constructor(entry, options = {}) {
      super();
      const channel = new MessageChannel();
      this.#port = channel.port1;
      channel.port1.onmessage = ({ data, ports }) => {
        if (this.#terminated) { return; }
        if (data.type === 'message') {
          const event = new MessageEvent('message', { data: data.data, ports });
          this.dispatchEvent(event);
          this.onmessage?.(event);
        } else if (data.type === 'error') {
          const event = new ErrorEvent('error', data.data);
          this.dispatchEvent(event);
          this.onerror?.(event);
        }
      };
      control.postMessage({ type: 'create', entry: String(entry), options, port: channel.port2 }, [channel.port2]);
    }
    postMessage = (data, transfer = []) => {
      if (this.#terminated) { return; }
      const list = Array.isArray(transfer) ? transfer : transfer.transfer ?? [];
      this.#port.postMessage({ type: 'post', data, transfer: list }, list);
    };
    terminate = () => {
      if (this.#terminated) { return; }
      this.#terminated = true;
      this.#port.postMessage({ type: 'terminate' });
      this.#port.close();
    };
  };
};

const receive = (event) => {
  if (ready) { return; }
  event.stopImmediatePropagation();
  if (initialized) { pending.push(event); return; }
  initialized = true;
  if (event.data?.type !== 'boot-worker-init' || !event.data.control) {
    throw new Error('Worker host requires a parent owned broker.');
  }
  installWorkerProxy(event.data.control);
  import(event.data.entry).then(() => {
    ready = true;
    removeEventListener('message', receive);
    for (const queued of pending) {
      dispatchEvent(new MessageEvent('message', { data: queued.data, ports: queued.ports }));
    }
    pending.length = 0;
  }).catch((error) => { setTimeout(() => { throw error; }, 0); });
};

addEventListener('message', receive);
