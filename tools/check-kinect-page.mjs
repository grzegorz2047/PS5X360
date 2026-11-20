// SPDX-License-Identifier: MIT
// Fix 56 (tools/source-1.0.2/fixes/56-kinect-pairing.patch): the phone camera page the console
// serves (port/phone_camera_page.hpp) and its pairing state (pairing.mjs, in the same header).
//   node tools/check-kinect-page.mjs <assembled tree>
//  (a) pairing.mjs: the console's answers (status, frames), the two-second tick, the camera's own
//      start, a browser that wants a tap, and view(): for every pairing and camera state the banner
//      has a text in every language, Start and Stop never show together, the key field only when a
//      key is needed, Disconnect only with something to disconnect; the TV's QR flow end to end
//      (connecting, waiting for the game, paired, camera asked by itself, one tap);
//  (b) the page: English, Portuguese, Spanish and Polish with the same texts; it imports
//      pairing.mjs; the marker the server replaces is there once; it no longer stops on
//      enabled:false; the phone layout (viewport, no sideways scrolling, 48 px tap targets); its
//      script parses;
//  (c) the :8360/camera certificate page (port/phone_camera_console.hpp) has Polish with the same
//      texts as English.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const tree = process.argv[2] || '.';
let failures = 0;
const check = (what, fn) => {
  try { fn(); } catch (e) { failures++; console.log('FAIL: ' + what + ': ' + e.message); }
};
const between = (text, open, close) => {
  const a = text.indexOf(open), b = text.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'no ' + open);
  return text.slice(a + open.length, b);
};
const header = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const page = between(header, 'R"KINECTPAGE(', ')KINECTPAGE"');
const pairingSource = between(header, 'R"PAIRINGJS(', ')PAIRINGJS"');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'check-kinect-page-'));
fs.writeFileSync(path.join(work, 'pairing.mjs'), pairingSource);
const P = await import(pathToFileURL(path.join(work, 'pairing.mjs')).href);
const TEXT = eval('(' + between(page, 'const TEXT = ', ';\nconst lang = ') + ')');

// (a) The state.
check('status answers', () => {
  assert.equal(P.afterStatus('connecting', 403, null), 'wrong_key');
  assert.equal(P.afterStatus('paired', 0, null), 'lost');
  assert.equal(P.afterStatus('waiting_game', 502, null), 'lost');
  assert.equal(P.afterStatus('need_key', 0, null), 'need_key');
  assert.equal(P.afterStatus('disconnected', 0, null), 'disconnected');
  assert.equal(P.afterStatus('connecting', 200, {enabled: true, game_running: true, waiting_for: ''}), 'paired');
  assert.equal(P.afterStatus('connecting', 200, {enabled: false, game_running: false, waiting_for: 'game'}), 'waiting_game');
  assert.equal(P.afterStatus('waiting_game', 200, {enabled: false, game_running: true, waiting_for: 'setting'}), 'waiting_setting');
  // A console without the new fields.
  assert.equal(P.afterStatus('connecting', 200, {enabled: false}), 'waiting_setting');
  assert.equal(P.afterStatus('connecting', 200, {enabled: false, game_running: false}), 'waiting_game');
});
check('frame answers', () => {
  assert.equal(P.afterFrame('paired', 409, 'disabled'), 'waiting_setting');
  assert.equal(P.afterFrame('paired', 403, 'key'), 'wrong_key');
  assert.equal(P.afterFrame('paired', 0, ''), 'lost');
  assert.equal(P.afterFrame('paired', 502, ''), 'lost');
  assert.equal(P.afterFrame('paired', 400, 'invalid motion frame'), 'paired');
});
check('the tick', () => {
  for (const s of ['connecting', 'lost', 'waiting_game', 'waiting_setting']) assert.ok(P.shouldPoll(s), s);
  for (const s of ['need_key', 'wrong_key', 'disconnected', 'paired']) assert.ok(!P.shouldPoll(s), s);
  assert.equal(P.POLL_MS, 2000);
});
check('the camera by itself', () => {
  assert.ok(P.shouldAutoStart({pairing: 'paired', camera: 'off'}));
  assert.ok(!P.shouldAutoStart({pairing: 'paired', camera: 'off', userStopped: true}), 'not after Stop');
  assert.ok(!P.shouldAutoStart({pairing: 'paired', camera: 'off', autoTried: true}), 'once');
  assert.ok(!P.shouldAutoStart({pairing: 'paired', camera: 'on'}));
  assert.ok(!P.shouldAutoStart({pairing: 'waiting_game', camera: 'off'}));
  assert.equal(P.afterCameraError('NotAllowedError', false), 'needs_tap');
  assert.equal(P.afterCameraError('NotAllowedError', true), 'error');
  assert.equal(P.afterCameraError('NotFoundError', false), 'error');
});
const pairings = ['need_key', 'connecting', 'wrong_key', 'waiting_game', 'waiting_setting', 'paired', 'lost', 'disconnected'];
const cameras = ['off', 'loading', 'needs_tap', 'on', 'error'];
check('every view', () => {
  for (const pairing of pairings) for (const camera of cameras) for (const body of [false, true]) {
    const v = P.view({pairing, camera, body, sending: 12, cameraError: 'x'});
    const where = pairing + '/' + camera;
    for (const code of ['en', 'pt', 'es', 'pl']) assert.ok(TEXT[code][v.step], where + ': ' + v.step + ' in ' + code);
    assert.ok(['ok', 'wait', 'warn', 'error'].includes(v.tone), where);
    assert.ok(!(v.show.start && v.show.stop), where + ': Start and Stop together');
    assert.equal(v.show.key, v.show.connect, where);
    assert.equal(v.show.key, ['need_key', 'wrong_key', 'disconnected'].includes(pairing), where + ': key field');
    assert.equal(v.show.stop, camera === 'on' || camera === 'loading', where + ': Stop');
    if (v.show.focus || v.show.recenter) assert.equal(camera, 'on', where);
    if (v.show.disconnect) assert.ok(!['need_key', 'wrong_key', 'disconnected'].includes(pairing), where);
  }
  assert.equal(P.view({pairing: 'paired', camera: 'on', body: true, sending: 28}).step, 'step_sending');
  assert.deepEqual(P.view({pairing: 'paired', camera: 'on', body: true, sending: 28}).args, [28]);
  assert.equal(P.view({pairing: 'paired', camera: 'on', body: false}).step, 'step_no_body');
  assert.equal(P.view({pairing: 'waiting_game', camera: 'off'}).step, 'wait_game');
  assert.equal(P.view({pairing: 'waiting_setting', camera: 'off'}).step, 'wait_setting');
  assert.equal(P.view({pairing: 'lost', camera: 'on'}).step, 'reconnecting');
});
check('the QR flow', () => {
  const s = {pairing: 'connecting', camera: 'off'};
  s.pairing = P.afterStatus(s.pairing, 200, {enabled: false, game_running: false, waiting_for: 'game'});
  assert.equal(P.view(s).step, 'wait_game');
  assert.ok(P.shouldPoll(s.pairing) && !P.shouldAutoStart(s));
  s.pairing = P.afterStatus(s.pairing, 0, null);  // the console restarts into the game
  assert.equal(s.pairing, 'lost');
  s.pairing = P.afterStatus(s.pairing, 200, {enabled: true, game_running: true, waiting_for: ''});
  assert.equal(s.pairing, 'paired');
  assert.ok(P.shouldAutoStart(s));
  s.autoTried = true;
  s.camera = P.afterCameraError('NotAllowedError', false);
  const v = P.view(s);
  assert.equal(v.step, 'step_tap');
  assert.deepEqual(Object.entries(v.show).filter(([, on]) => on).map(([k]) => k).sort(), ['disconnect', 'start']);
});

// (b) The page.
check('languages', () => {
  assert.deepEqual(Object.keys(TEXT).sort(), ['en', 'es', 'pl', 'pt']);
  const keys = Object.keys(TEXT.en).sort();
  for (const code of ['pt', 'es', 'pl']) assert.deepEqual(Object.keys(TEXT[code]).sort(), keys, code);
  for (const key of ['wait_game', 'wait_setting', 'step_key', 'step_tap', 'step_sending']) assert.ok(TEXT.pl[key], key);
});
check('the page', () => {
  assert.ok(page.includes("from './pairing.mjs?v=56';"), 'imports pairing.mjs');
  assert.equal(page.split('<meta name="ps5x360-host" content="relay">').length, 2, 'the marker the server replaces');
  assert.ok(!page.includes("throw Error(T('enable_motion'))"), 'no stop on enabled:false');
  assert.ok(page.includes('<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">'));
  assert.ok(page.includes('overflow-x:hidden') && page.includes('min-height:48px'), 'phone layout');
  assert.ok(page.includes('id="banner"') && page.includes('aria-live="polite"'), 'one banner');
  const script = between(page, '<script type="module">', '</script>');
  fs.writeFileSync(path.join(work, 'page.mjs'), script);
  const parsed = spawnSync(process.execPath, ['--check', path.join(work, 'page.mjs')], {encoding: 'utf8'});
  assert.equal(parsed.status, 0, parsed.stderr);
});

// (c) The certificate page.
check('the certificate page', () => {
  const landing = fs.readFileSync(path.join(tree, 'port/phone_camera_console.hpp'), 'utf8');
  const W = eval('(' + between(landing, 'const W=', '\nconst w=W[').trim().replace(/;$/, '') + ')');
  assert.deepEqual(Object.keys(W).sort(), ['en', 'es', 'pl', 'pt']);
  for (const code of ['pt', 'es', 'pl']) assert.deepEqual(Object.keys(W[code]).sort(), Object.keys(W.en).sort(), code);
});

fs.rmSync(work, {recursive: true, force: true});
if (failures) { console.log(`check-kinect-page: ${failures} failure(s)`); process.exit(1); }
console.log('check-kinect-page: ok');
