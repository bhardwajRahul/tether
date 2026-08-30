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

const { connectToNativeHost, registerOtpRequest, _resetOtpRequests, hostMatchesDomain, registrableDomain } =
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

  it('matches an apex page against a subdomain sender', () => {
    expect(hostMatchesDomain('google.com', 'accounts.google.com')).toBe(true);
  });

  it('matches ccTLD subdomains', () => {
    expect(hostMatchesDomain('www.amazon.co.uk', 'amazon.co.uk')).toBe(true);
  });

  it('rejects a different registrable domain under the same ccTLD', () => {
    expect(hostMatchesDomain('evil.co.uk', 'amazon.co.uk')).toBe(false);
  });

  it('rejects suffix spoofs on ccTLD domains', () => {
    expect(hostMatchesDomain('amazon.co.uk.evil.com', 'amazon.co.uk')).toBe(false);
  });

  it('rejects different registrants under a multi-part ccTLD', () => {
    expect(hostMatchesDomain('evil.com.cy', 'bank.com.cy')).toBe(false);
  });

  it('rejects different sites on a private suffix', () => {
    expect(hostMatchesDomain('foo.github.io', 'bar.github.io')).toBe(false);
  });
});

describe('registrableDomain', () => {
  it('returns eTLD+1 for common domains', () => {
    expect(registrableDomain('www.amazon.com')).toBe('amazon.com');
    expect(registrableDomain('accounts.google.com')).toBe('google.com');
    expect(registrableDomain('www.amazon.co.uk')).toBe('amazon.co.uk');
    expect(registrableDomain('amazon.com.evil.com')).toBe('evil.com');
  });

  it('leaves IP literals alone', () => {
    expect(registrableDomain('192.168.1.1')).toBe('192.168.1.1');
  });

  it('handles wildcard and exception suffixes', () => {
    expect(registrableDomain('test.ck')).toBe('');
    expect(registrableDomain('b.test.ck')).toBe('b.test.ck');
    expect(registrableDomain('www.ck')).toBe('www.ck');
    expect(registrableDomain('www.www.ck')).toBe('www.ck');
  });

  it('distinguishes registrants on multi-part ccTLDs', () => {
    expect(registrableDomain('evil.com.cy')).toBe('evil.com.cy');
    expect(registrableDomain('bank.com.cy')).toBe('bank.com.cy');
  });

  it('distinguishes sites on private suffixes', () => {
    expect(registrableDomain('foo.github.io')).toBe('foo.github.io');
    expect(registrableDomain('bar.github.io')).toBe('bar.github.io');
  });

  it('returns empty for bare public suffixes', () => {
    expect(registrableDomain('com')).toBe('');
    expect(registrableDomain('biz')).toBe('');
  });
});
