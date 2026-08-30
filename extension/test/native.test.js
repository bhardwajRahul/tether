import { describe, it, expect, vi, beforeEach } from 'vitest';

// Capture the onMessage listener registered by connectToNativeHost so we can
// simulate the daemon pushing events to the native host.
let messageListener = null;

const mockPort = {
  postMessage: vi.fn(),
  onMessage: { addListener: vi.fn((cb) => { messageListener = cb; }) },
  onDisconnect: { addListener: vi.fn() }
};

// By default the tab reports that it filled the field, so delivery stops there.
function installChrome(tabs) {
  global.browser = undefined;
  global.chrome = {
    runtime: {
      connectNative: vi.fn().mockReturnValue(mockPort),
      lastError: null
    },
    tabs: {
      query: vi.fn((_filter, cb) => cb(tabs)),
      sendMessage: vi.fn((_id, _msg, cb) => { if (cb) cb({ filled: true }); })
    }
  };
}

beforeEach(() => {
  messageListener = null;
  mockPort.postMessage.mockClear();
  installChrome([{ id: 1 }, { id: 2 }, { id: 3 }]);
});

const { connectToNativeHost, registerOtpRequest, _resetOtpRequests, hostMatchesDomain } =
  await import('../src/shared/native.js');

describe('native host receive/routing', () => {
  beforeEach(() => _resetOtpRequests());

  it('delivers only to tabs that requested an OTP', () => {
    connectToNativeHost();
    registerOtpRequest(2, 'login.example.com');
    expect(messageListener).toBeTypeOf('function');

    messageListener({ command: 'otp_available', otp: '123456' });

    expect(chrome.tabs.query).toHaveBeenCalledWith({}, expect.any(Function));
    expect(chrome.tabs.sendMessage).toHaveBeenCalledTimes(1);
    const [tabId, msg] = chrome.tabs.sendMessage.mock.calls[0];
    expect(tabId).toBe(2);
    expect(msg).toMatchObject({ action: 'fill_otp', otp: '123456' });
  });

  it('prefers the active tab among requesters', () => {
    installChrome([
      { id: 1, active: false },
      { id: 2, active: true },
      { id: 3, active: false }
    ]);
    connectToNativeHost();
    registerOtpRequest(1, 'a.example.com');
    registerOtpRequest(2, 'b.example.com');
    registerOtpRequest(3, 'c.example.com');

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 9 });

    const tabIds = chrome.tabs.sendMessage.mock.calls.map((c) => c[0]);
    expect(tabIds[0]).toBe(2);
    expect(tabIds).not.toContain(1);
    expect(tabIds).not.toContain(3);
  });

  it('does not deliver to any tab when none requested', () => {
    connectToNativeHost();
    messageListener({ command: 'otp_available', otp: '123456' });
    expect(chrome.tabs.sendMessage).not.toHaveBeenCalled();
  });

  it('only delivers a pinned OTP to a matching requesting domain', () => {
    connectToNativeHost();
    registerOtpRequest(1, 'evil.com');
    registerOtpRequest(2, 'www.amazon.com');

    messageListener({
      command: 'otp_available',
      otp: '123456',
      otp_id: 9,
      sender_domain: 'amazon.com'
    });

    const tabIds = chrome.tabs.sendMessage.mock.calls.map((c) => c[0]);
    expect(tabIds).toEqual([2]);
    expect(tabIds).not.toContain(1);
  });

  it('delivers an unpinned OTP to any requesting domain', () => {
    connectToNativeHost();
    registerOtpRequest(1, 'evil.com');

    messageListener({ command: 'otp_available', otp: '123456', otp_id: 9 });

    const tabIds = chrome.tabs.sendMessage.mock.calls.map((c) => c[0]);
    expect(tabIds).toEqual([1]);
  });

  it('does not deliver a pinned OTP when no requesting tab matches', () => {
    connectToNativeHost();
    registerOtpRequest(1, 'evil.com');

    messageListener({
      command: 'otp_available',
      otp: '123456',
      otp_id: 9,
      sender_domain: 'amazon.com'
    });

    expect(chrome.tabs.sendMessage).not.toHaveBeenCalled();
  });

  it('ignores an otp_available event with an empty code', () => {
    connectToNativeHost();
    registerOtpRequest(1, 'a.example.com');
    messageListener({ command: 'otp_available', otp: '' });
    expect(chrome.tabs.sendMessage).not.toHaveBeenCalled();
  });

  it('ignores malformed native messages', () => {
    connectToNativeHost();
    registerOtpRequest(1, 'a.example.com');
    messageListener(null);
    messageListener('garbage');
    messageListener({ command: 'something_else', otp: '123456' });
    expect(chrome.tabs.sendMessage).not.toHaveBeenCalled();
  });
});

describe('hostMatchesDomain', () => {
  it('matches exact and subdomain hosts', () => {
    expect(hostMatchesDomain('amazon.com', 'amazon.com')).toBe(true);
    expect(hostMatchesDomain('www.amazon.com', 'amazon.com')).toBe(true);
    expect(hostMatchesDomain('login.amazon.com', 'amazon.com')).toBe(true);
  });

  it('rejects suffix spoofs', () => {
    expect(hostMatchesDomain('amazon.com.evil.com', 'amazon.com')).toBe(false);
    expect(hostMatchesDomain('evilamazon.com', 'amazon.com')).toBe(false);
    expect(hostMatchesDomain('notamazon.com', 'amazon.com')).toBe(false);
  });

  it('treats an empty domain as unpinned', () => {
    expect(hostMatchesDomain('anything.example.com', '')).toBe(true);
    expect(hostMatchesDomain('', '')).toBe(true);
  });

  it('normalizes case and trailing dots', () => {
    expect(hostMatchesDomain('www.amazon.com', 'Amazon.COM.')).toBe(true);
  });
});
