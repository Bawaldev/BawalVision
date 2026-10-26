import test from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { createRelayServer } from '../server.js';

function listen(server) {
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve(server.address().port));
  });
}

function open(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, res) => reject(new Error(`HTTP ${res.statusCode}`)));
  });
}

function nextBinary(ws) {
  return new Promise((resolve, reject) => {
    ws.once('message', (data, isBinary) => resolve({ data, isBinary }));
    ws.once('error', reject);
  });
}

test('forwards a JPEG frame from ingest to a watcher', async (t) => {
  const relay = createRelayServer({
    ingestKey: 'ingest-secret',
    viewKey: '',
    log: () => {}
  });
  const port = await listen(relay.server);
  t.after(() => relay.close());

  const watcher = await open(`ws://127.0.0.1:${port}/watch?device=cam1`);
  const ingest = await open(`ws://127.0.0.1:${port}/ingest?device=cam1&key=ingest-secret`);
  t.after(() => {
    watcher.terminate();
    ingest.terminate();
  });

  const pending = nextBinary(watcher);
  const frame = Buffer.from([0xff, 0xd8, 0xff, 0xdb, 0x00, 0x04, 0xff, 0xd9]);
  ingest.send(frame);
  const received = await pending;

  assert.equal(received.isBinary, true);
  assert.deepEqual(Buffer.from(received.data), frame);

  const health = await fetch(`http://127.0.0.1:${port}/health`).then((res) => res.json());
  assert.equal(health.status, 'ok');
  assert.equal(health.devices.cam1.frames, 1);
  assert.equal(health.devices.cam1.watchers, 1);
  assert.equal(health.devices.cam1.online, true);
});

test('rejects ingest with a wrong key', async (t) => {
  const relay = createRelayServer({ ingestKey: 'ingest-secret', log: () => {} });
  const port = await listen(relay.server);
  t.after(() => relay.close());

  await assert.rejects(
    open(`ws://127.0.0.1:${port}/ingest?device=cam1&key=wrong`),
    /HTTP 401/
  );
});

test('ignores non-JPEG payloads', async (t) => {
  const relay = createRelayServer({ log: () => {} });
  const port = await listen(relay.server);
  t.after(() => relay.close());

  const ingest = await open(`ws://127.0.0.1:${port}/ingest?device=cam1`);
  t.after(() => ingest.terminate());
  ingest.send(Buffer.from('not-a-jpeg'));

  await new Promise((resolve) => setTimeout(resolve, 50));
  const health = await fetch(`http://127.0.0.1:${port}/health`).then((res) => res.json());
  assert.equal(health.devices.cam1.frames, 0);
});

test('serves the latest frame to a watcher that joins late', async (t) => {
  const relay = createRelayServer({ log: () => {} });
  const port = await listen(relay.server);
  t.after(() => relay.close());

  const ingest = await open(`ws://127.0.0.1:${port}/ingest?device=cam1`);
  t.after(() => ingest.terminate());
  const frame = Buffer.from([0xff, 0xd8, 0xff, 0xd9]);
  ingest.send(frame);
  await new Promise((resolve) => setTimeout(resolve, 50));

  const watcher = new WebSocket(`ws://127.0.0.1:${port}/watch?device=cam1`);
  const pending = nextBinary(watcher);
  await new Promise((resolve, reject) => {
    watcher.once('open', resolve);
    watcher.once('error', reject);
  });
  t.after(() => watcher.terminate());
  const received = await pending;
  assert.deepEqual(Buffer.from(received.data), frame);
});
