import { test } from 'node:test';
import assert from 'node:assert/strict';
import { backendPath } from './backendPath.ts';

test('maps UI routes onto backend routes', () => {
  assert.equal(backendPath(['health']), '/health');
  assert.equal(backendPath(['buckets']), '/');
  assert.equal(backendPath(['buckets', 'b']), '/b');
  assert.equal(backendPath(['list', 'b']), '/b');
  assert.equal(backendPath(['objects', 'b', 'a', 'c.txt']), '/b/a/c.txt');
});

test('rejects unknown or incomplete routes', () => {
  assert.equal(backendPath([]), null);
  assert.equal(backendPath(['nope']), null);
  assert.equal(backendPath(['objects', 'b']), null);
  assert.equal(backendPath(['buckets', 'b', 'x']), null);
});
