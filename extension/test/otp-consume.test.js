// Regression tests for the OTP double-consumption bug.
//
// Symptom: A.com autofills correctly and the user logs in; later B.com is opened
// and gets A.com's code injected. Cause: consumeOtp() was sent from the content
// script, but filling dispatches input/change, the site auto-submits, and the
// navigation destroys the content script before the message is delivered. The
// daemon then kept serving that code for the rest of its 5 minute TTL.
//
// The fix moves the consume into the background worker, driven by the tab's reply.

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { JSDOM } from 'jsdom';
import * as fs from 'fs';
import * as path from 'path';

const loadFixture = (filename) => {
  const html = fs.readFileSync(path.join(__dirname, 'fixtures', filename), 'utf8');
  return new JSDOM(html).window.document;
};

// --- native.js harness ------------------------------------------------------
// Captures the daemon->extension listener and the messages sent back to the daemon.
let messageListener = null;
let sentToDaemon = [];
let tabResponses = {}; // tabId -> { filled } handed back to sendMessage's callback
let messagedTabs = [];

const mockPort = {
  postMessage: vi.fn((msg) => sentToDaemon.push(msg)),
  onMessage: { addListener: vi.fn((cb) => { messageListener = cb; }) },
  onDisconnect: { addListener: vi.fn() }
};

function installChrome(tabs) {
  global.browser = undefined;
  global.chrome = {
    runtime: {
      connectNative: vi.fn().mockReturnValue(mockPort),
      lastError: null
    },
    tabs: {
      query: vi.fn((_filter, cb) => cb(tabs)),
      sendMessage: vi.fn((id, msg, cb) => {
        messagedTabs.push({ id, msg });
        if (cb) cb(tabResponses[id]);
      })
    }
  };
}

beforeEach(() => {
  messageListener = null;
  sentToDaemon = [];
  messagedTabs = [];
  tabResponses = {};
  mockPort.postMessage.mockClear();
  installChrome([{ id: 1, lastAccessed: 100 }]);
});

const { connectToNativeHost } = await import('../src/shared/native.js');
const { handleFillOtp, _resetFilledOtps } = await import('../src/content/autofill.js');

describe('background consumes the OTP on behalf of the filling tab', () => {
  it('sends consume_otp with the otp_id when a tab reports it filled', () => {
    tabResponses[1] = { filled: true };
    connectToNativeHost();

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 7 });

    // This is the whole bug: the content script cannot be relied on to send this,
    // because filling the field navigates the page away.
    expect(sentToDaemon).toContainEqual({ command: 'consume_otp', otp_id: 7 });
  });

  it('does not consume when no tab could fill the code', () => {
    tabResponses[1] = { filled: false };
    connectToNativeHost();

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 7 });

    expect(sentToDaemon).not.toContainEqual(
      expect.objectContaining({ command: 'consume_otp' })
    );
  });

  it('stops offering the code once a tab takes it', () => {
    installChrome([
      { id: 1, lastAccessed: 100 },
      { id: 2, lastAccessed: 300 }, // most recently used
      { id: 3, lastAccessed: 200 }
    ]);
    tabResponses[2] = { filled: true };
    connectToNativeHost();

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 9 });

    // Most-recently-used first, and nothing after the tab that filled it. The old
    // code blasted every tab, so every open OTP page got the same code.
    expect(messagedTabs.map(t => t.id)).toEqual([2]);
  });

  it('falls through to the next tab when the first cannot fill', () => {
    installChrome([
      { id: 1, lastAccessed: 300 },
      { id: 2, lastAccessed: 200 },
      { id: 3, lastAccessed: 100 }
    ]);
    tabResponses[1] = { filled: false }; // no OTP field here
    tabResponses[2] = { filled: true };
    connectToNativeHost();

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 11 });

    // Unfocused tabs are still reachable - this is what commit 5bad703 fixed and
    // it must keep working.
    expect(messagedTabs.map(t => t.id)).toEqual([1, 2]);
    expect(sentToDaemon).toContainEqual({ command: 'consume_otp', otp_id: 11 });
  });

  it('forwards otp_id to the content script', () => {
    tabResponses[1] = { filled: true };
    connectToNativeHost();

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 42 });

    expect(messagedTabs[0].msg).toEqual({ action: 'fill_otp', otp: '123456', otp_id: 42 });
  });

  it('still ignores an otp_available with an empty code', () => {
    connectToNativeHost();
    messageListener({ command: 'otp_available', otp: '', otp_id: 3 });
    expect(messagedTabs).toHaveLength(0);
  });
});

describe('handleFillOtp', () => {
  beforeEach(() => _resetFilledOtps());

  it('fills a single field and reports filled', () => {
    const doc = loadFixture('std-autocomplete.html');
    const result = handleFillOtp(doc, { otp: '123456', otp_id: 1 });

    expect(result).toEqual({ filled: true });
    expect(doc.querySelector('input').value).toBe('123456');
  });

  it('fills split fields character by character', () => {
    const doc = loadFixture('split-otp.html');
    const result = handleFillOtp(doc, { otp: '123456', otp_id: 1 });

    expect(result).toEqual({ filled: true });
    const inputs = [...doc.querySelectorAll('input')];
    expect(inputs.slice(0, 6).map(i => i.value).join('')).toBe('123456');
  });

  it('refuses to fill the same otp_id twice', () => {
    const doc = loadFixture('std-autocomplete.html');
    expect(handleFillOtp(doc, { otp: '123456', otp_id: 5 }).filled).toBe(true);

    // The daemon replays the code on a service-worker restart (net.cpp subscribe
    // handler). Refilling causes event storms on controlled React inputs.
    doc.querySelector('input').value = '';
    expect(handleFillOtp(doc, { otp: '123456', otp_id: 5 }).filled).toBe(false);
    expect(doc.querySelector('input').value).toBe('');
  });

  it('overwrites a stale value when a NEW code arrives', () => {
    const doc = loadFixture('std-autocomplete.html');
    handleFillOtp(doc, { otp: '111111', otp_id: 1 });
    expect(doc.querySelector('input').value).toBe('111111');

    // Guards the behaviour introduced by 5bad703: a fresh code must replace a stale
    // field value without the user clearing it first.
    const result = handleFillOtp(doc, { otp: '222222', otp_id: 2 });
    expect(result).toEqual({ filled: true });
    expect(doc.querySelector('input').value).toBe('222222');
  });

  it('reports not-filled when the code is already present', () => {
    const doc = loadFixture('std-autocomplete.html');
    doc.querySelector('input').value = '123456';

    expect(handleFillOtp(doc, { otp: '123456', otp_id: 1 }).filled).toBe(false);
  });

  it('reports not-filled when the page has no OTP fields', () => {
    const doc = new JSDOM('<input type="password" name="pw">').window.document;
    expect(handleFillOtp(doc, { otp: '123456', otp_id: 1 }).filled).toBe(false);
  });

  it('ignores an empty or missing code', () => {
    const doc = loadFixture('std-autocomplete.html');
    expect(handleFillOtp(doc, { otp: '', otp_id: 1 }).filled).toBe(false);
    expect(handleFillOtp(doc, {}).filled).toBe(false);
  });

  it('dispatches input and change so framework-controlled inputs update', () => {
    const doc = loadFixture('std-autocomplete.html');
    const input = doc.querySelector('input');
    const seen = [];
    input.addEventListener('input', () => seen.push('input'));
    input.addEventListener('change', () => seen.push('change'));

    handleFillOtp(doc, { otp: '123456', otp_id: 1 });

    expect(seen).toEqual(['input', 'change']);
  });

  it('invokes onFilled so the caller can stop its poll loop', () => {
    const doc = loadFixture('std-autocomplete.html');
    const onFilled = vi.fn();

    handleFillOtp(doc, { otp: '123456', otp_id: 8 }, { onFilled });

    expect(onFilled).toHaveBeenCalledWith(8);
  });
});
