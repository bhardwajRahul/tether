import { describe, it, expect, vi, beforeEach } from 'vitest';

global.messenger = {
  messages: {
    query: vi.fn().mockResolvedValue({ messages: [], id: null }),
    getFull: vi.fn().mockResolvedValue({ parts: [] }),
    listInlineTextParts: vi.fn().mockResolvedValue([]),
    continueList: vi.fn().mockResolvedValue(null),
    onNewMailReceived: {
      addListener: vi.fn()
    }
  },
  messengerUtilities: {
    convertToPlainText: vi.fn().mockResolvedValue('')
  },
  folders: {
    onFolderInfoChanged: {
      addListener: vi.fn()
    }
  },
  messageDisplay: {
    onMessageDisplayed: {
      addListener: vi.fn()
    }
  },
  runtime: {
    connectNative: vi.fn().mockReturnValue({
      postMessage: vi.fn(),
      onMessage: { addListener: vi.fn() },
      onDisconnect: { addListener: vi.fn() }
    })
  }
};

const nativePostMessage = vi.fn();

global.browser = {
  runtime: {
    connectNative: vi.fn().mockReturnValue({
      postMessage: nativePostMessage,
      onMessage: { addListener: vi.fn() },
      onDisconnect: { addListener: vi.fn() }
    })
  }
};

import { scoreCandidate, isFalsePositive, OTP_PATTERNS, processMessage, extractSenderDomain, resolveSenderDomain, canonicalizeSenderDomain } from '../src/mail/extractor.js';

function extractOtpCandidates(text) {
  const candidates = [];
  for (const pattern of OTP_PATTERNS) {
    const regex = new RegExp(pattern.source, pattern.flags);
    let match;
    while ((match = regex.exec(text)) !== null) {
      const num = match[1] || match[0];
      if (/^[A-Za-z]+$/.test(num)) continue;

      const startIndex = Math.max(0, match.index - 50);
      const endIndex = Math.min(text.length, match.index + match[0].length + 50);
      const context = text.substring(startIndex, endIndex);

      if (isFalsePositive(num, context)) continue;

      const score = scoreCandidate(num, context);
      candidates.push({ num, score });
    }
  }
  return candidates.sort((a, b) => b.score - a.score);
}

describe('scoreCandidate (from extractor.js)', () => {
  it('scores high when keyword is near the number', () => {
    const score = scoreCandidate('123456', 'Your verification code is 123456');
    expect(score).toBeGreaterThan(0);
  });

  it('scores higher with proximity bonus', () => {
    const farAway = scoreCandidate('123456', 'your code is 123456 at the very end of this extremely long message that contains many words and little else');
    const close = scoreCandidate('123456', 'code: 123456 use this');
    expect(close).toBeGreaterThan(farAway);
  });

  it('gives bonus for prominent elements', () => {
    const normal = scoreCandidate('123456', 'your code is 123456');
    const prominent = scoreCandidate('123456', 'PROMINENT: code is 123456');
    expect(prominent).toBeGreaterThan(normal);
  });

  it('scores multiple keyword matches', () => {
    const single = scoreCandidate('123456', 'code 123456');
    const multiple = scoreCandidate('123456', 'verification code 123456');
    expect(multiple).toBeGreaterThan(single);
  });
});

describe('isFalsePositive (from extractor.js)', () => {
  it('detects years as false positives', () => {
    expect(isFalsePositive('2024', 'year 2024')).toBe(true);
    expect(isFalsePositive('2025', 'in 2025')).toBe(true);
  });

  it('detects phone-like numbers as false positives', () => {
    expect(isFalsePositive('0123456', 'phone: 0123456')).toBe(true);
  });

  it('detects prices as false positives', () => {
    expect(isFalsePositive('999', 'cost: $999 USD')).toBe(true);
    expect(isFalsePositive('50', 'price: $50')).toBe(true);
  });

  it('detects order/tracking as false positives', () => {
    expect(isFalsePositive('123456', 'order #123456 tracking')).toBe(true);
  });

  it('accepts legitimate OTP codes', () => {
    expect(isFalsePositive('171792', 'your verification code is 171792')).toBe(false);
    expect(isFalsePositive('123456', 'confirm code 123456 expires in 5 min')).toBe(false);
  });

  it('rejects zip codes as false positives', () => {
    expect(isFalsePositive('95110', 'Adobe, 345 Park Avenue, San Jose, CA 95110 USA')).toBe(true);
    expect(isFalsePositive('10001', 'NY 10001')).toBe(true);
  });
});

describe('OTP pattern matching', () => {
  it('extracts plain digit codes', () => {
    const text = 'Your code is 171792';
    const matches = text.match(/\b(\d{4,8})\b/);
    expect(matches).toContain('171792');
  });

  it('extracts formatted codes', () => {
    const text = 'code: 123 456';
    const regex = new RegExp(OTP_PATTERNS[2].source, OTP_PATTERNS[2].flags);
    const match = regex.exec(text);
    expect(match).toBeTruthy();
  });

  it('extracts alphanumeric codes', () => {
    const text = 'token ABC123DEF';
    const regex = new RegExp(OTP_PATTERNS[1].source, OTP_PATTERNS[1].flags);
    const matches = text.match(regex);
    expect(matches).toBeTruthy();
  });
});

describe('extractSenderDomain', () => {
  it('extracts the domain from a display-name header', () => {
    expect(extractSenderDomain('Amazon <noreply@amazon.com>')).toBe('amazon.com');
    expect(extractSenderDomain('Google <no-reply@accounts.google.com>')).toBe('accounts.google.com');
  });

  it('extracts the domain from a bare address', () => {
    expect(extractSenderDomain('noreply@amazon.com')).toBe('amazon.com');
  });

  it('returns empty string for missing or malformed input', () => {
    expect(extractSenderDomain('')).toBe('');
    expect(extractSenderDomain(null)).toBe('');
    expect(extractSenderDomain('not an email')).toBe('');
  });

  it('lowercases and strips trailing dots', () => {
    expect(extractSenderDomain('NoReply@Amazon.COM.')).toBe('amazon.com');
  });
});

describe('resolveSenderDomain', () => {
  it('uses the DKIM domain the receiving server validated', () => {
    expect(resolveSenderDomain(
      { author: 'Amazon <no-reply@example.com>' },
      { 'authentication-results': ['mx.example.com; dkim=pass header.d=amazon.com header.i=@amazon.com'] }
    )).toBe('amazon.com');
  });

  it('falls back to header.i when header.d is absent', () => {
    expect(resolveSenderDomain(
      { author: 'Amazon <no-reply@example.com>' },
      { 'authentication-results': ['example.com; dkim=pass header.i=@amazon.com header.s=mail'] }
    )).toBe('amazon.com');
  });

  it('ignores a failed DKIM result and leaves the OTP unpinned', () => {
    expect(resolveSenderDomain(
      { author: 'Amazon <noreply@amazon.com>' },
      { 'authentication-results': ['example.com; dkim=fail header.d=amazon.com'] }
    )).toBe('');
  });

  it('leaves the OTP unpinned when Authentication-Results is missing', () => {
    expect(resolveSenderDomain({ author: 'Amazon <noreply@amazon.com>' }, {})).toBe('');
  });

  it('prefers the validated domain aligned with From among multiple passes', () => {
    expect(resolveSenderDomain(
      { author: 'Facebook <security@facebookmail.com>' },
      { 'authentication-results': ['example.com; dkim=pass header.d=sendgrid.net; dkim=pass header.d=facebookmail.com'] }
    )).toBe('facebook.com');
  });

  it('maps a validated sender domain through the alias table', () => {
    expect(resolveSenderDomain(
      { author: 'Facebook <security@facebookmail.com>' },
      { 'authentication-results': ['example.com; dkim=pass header.d=facebookmail.com'] }
    )).toBe('facebook.com');
  });
});

describe('canonicalizeSenderDomain', () => {
  it('maps facebookmail.com to facebook.com', () => {
    expect(canonicalizeSenderDomain('facebookmail.com')).toBe('facebook.com');
  });

  it('maps a subdomain of a known sender domain', () => {
    expect(canonicalizeSenderDomain('mail.facebookmail.com')).toBe('facebook.com');
  });

  it('returns the registrable domain for unrelated domains', () => {
    expect(canonicalizeSenderDomain('amazon.com')).toBe('amazon.com');
    expect(canonicalizeSenderDomain('www.amazon.co.uk')).toBe('amazon.co.uk');
  });

  it('returns empty for empty input', () => {
    expect(canonicalizeSenderDomain('')).toBe('');
  });
});

describe('processMessage sends the sender domain', () => {
  beforeEach(() => {
    nativePostMessage.mockClear();
  });

  it('leaves sender_domain empty when the server did not validate DKIM', async () => {
    global.messenger.messages.listInlineTextParts.mockResolvedValueOnce([
      { contentType: 'text/plain', content: 'Your verification code is 123456' }
    ]);

    await processMessage({
      id: 42,
      subject: 'Your verification code',
      author: 'Amazon <noreply@amazon.com>'
    });

    expect(nativePostMessage).toHaveBeenCalledWith(
      expect.objectContaining({
        command: 'new_otp',
        otp: '123456',
        sender_domain: ''
      })
    );
  });

  it('uses the validated DKIM domain when available', async () => {
    global.messenger.messages.listInlineTextParts.mockResolvedValueOnce([
      { contentType: 'text/plain', content: 'Your verification code is 123456' }
    ]);
    global.messenger.messages.getFull.mockResolvedValueOnce({
      headers: { 'authentication-results': ['example.com; dkim=pass header.d=amazon.com'] },
      parts: []
    });

    await processMessage({
      id: 43,
      subject: 'Your verification code',
      author: 'Amazon <no-reply@example.com>'
    });

    expect(nativePostMessage).toHaveBeenCalledWith(
      expect.objectContaining({
        command: 'new_otp',
        otp: '123456',
        sender_domain: 'amazon.com'
      })
    );
  });
});

describe('Real email fixtures', () => {
  it('extracts OTP from MyBank email', () => {
    const text = `PREVENT FRAUD: DO NOT SHARE THIS CODE. MyBank will never contact you asking for it. Only use verification code: 171792 in Online Banking or the MyBank App.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    const best = candidates[0];
    expect(best.num).toBe('171792');
    expect(best.score).toBeGreaterThan(0);
  });

  it('extracts OTP from Google auth email', () => {
    const text = `Google: Your verification code is 123456

This code is valid for the next 10 minutes. Don't share this with anyone.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num).toBe('123456');
  });

  it('extracts OTP from Amazon formatted code', () => {
    const text = `Your Amazon sign-in verification code is 123 456

This code can be used to confirm your identity.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num.replace(/[-\s]/g, '')).toBe('123456');
  });

  it('extracts OTP from bank email with strong tag', () => {
    const text = `PROMINENT:987654 Your verification code is 987654 expires in 5 minutes. Do not share this code.`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num).toBe('987654');
  });

  it('rejects order confirmation as false positive', () => {
    const text = `Thank you for your order! Order number: 123456789 Total: $99.99`;

    const candidates = extractOtpCandidates(text);

    expect(candidates.length).toBe(0);
  });

  it('extracts correct OTP from complex HTML email (Adobe)', () => {
    const fs = require('fs');
    const path = require('path');

    // naive plain text conversion here
    const emlPath = path.join(__dirname, 'fixtures/adobe.eml');
    const emailText = fs.readFileSync(emlPath, 'utf-8');

    const candidates = extractOtpCandidates(emailText);

    // Zip code 95110 should be rejected by isFalsePositive
    const zipCodeCandidate = candidates.find(c => c.num === '95110');
    expect(zipCodeCandidate).toBeUndefined();
    
    expect(candidates.length).toBeGreaterThan(0);
    expect(candidates[0].num).toBe('861017');
  });
});
