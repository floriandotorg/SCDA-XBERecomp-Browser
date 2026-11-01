import { parentPort } from 'node:worker_threads';
import { installIsoIoService } from '../../src/browser/iso-io-service.mjs';

installIsoIoService(parentPort);
