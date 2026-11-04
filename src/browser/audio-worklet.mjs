import { AudioQueue, AUDIO_RATE } from './audio-transport.mjs';

class MissionAudio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    if (sampleRate !== AUDIO_RATE) { throw new Error('Mission audio requires a 48000 Hz context.'); }
    this.queue = new AudioQueue(options.processorOptions);
  }

  process(inputs, outputs) {
    if (outputs[0]?.length === 2) { this.queue.render(outputs[0][0], outputs[0][1]); }
    return true;
  }
}

registerProcessor('mission-audio', MissionAudio);
