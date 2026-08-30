// Mail script for Thunderbird/Betterbird
// This script extracts OTP codes from emails

// single entry point for the mail extension
import '../background/background.js';
import { sendToNativeHost, hostMatchesDomain, registrableDomain } from '../shared/native.js';

const OTP_CONTEXT_KEYWORDS = [
  'code', 'otp', 'one-time', 'verification', 'confirm', 'passcode',
  'authenticate', 'login', 'sign in', '2fa', 'token', 'pin',
  'expires', 'valid for', 'do not share', 'use this'
];

export const OTP_PATTERNS = [
  /\b(\d{4,8})\b/g,                         // plain digits 4–8 long
  /\b([A-Z0-9]{6,10})\b/g,                  // alphanumeric (some services)
  /\b(\d{3}[-\s]\d{3})\b/g,                 // formatted: 123 456 or 123-456
];

const OTP_SUBJECT_PATTERNS = [
  /verification/i, /your .{0,15} code/i, /one.time/i,
  /confirm.{0,10}(email|account)/i, /sign.in code/i, /login code/i,
  /\bOTP\b/i, /security code/i, /\d{4,8} is your/i
];

const POLL_INTERVAL_MS = 15000;    // 15 seconds between polls
const OTP_TTL_MS = 10 * 60 * 1000; // only look at emails from the last 10 minutes

// Use a Map instead of Set so we can prune by timestamp
const seenMessages = new Map(); // message.id -> timestamp

function markSeen(id) {
  seenMessages.set(id, Date.now());
}

function hasSeen(id) {
  return seenMessages.has(id);
}

function pruneOldSeen() {
  const cutoff = Date.now() - OTP_TTL_MS;
  for (const [id, ts] of seenMessages) {
    if (ts < cutoff) seenMessages.delete(id);
  }
}

// Run pruning every minute so the Map doesn't grow forever
setInterval(pruneOldSeen, 60_000);

// distance from `target` to the closest occurrence of `needle` in `haystack`.
function nearestDistance(haystack, needle, target) {
  let best = Infinity;
  for (let i = haystack.indexOf(needle); i !== -1; i = haystack.indexOf(needle, i + 1)) {
    const d = Math.abs(target - i);
    if (d < best) best = d;
    if (i > target) break; // occurrences are ordered; past the target we only get worse
  }
  return best;
}

export function scoreCandidate(num, surroundingText) {
  let score = 0;
  // Squash whitespace for accurate proximity and keyword matching
  const cleanText = surroundingText.replace(/\s+/g, ' ').toLowerCase();
  const lower = cleanText;
  const numIndex = lower.indexOf(num.toLowerCase());

  for (const kw of OTP_CONTEXT_KEYWORDS) {
    if (lower.indexOf(kw) === -1) continue;
    score += 10;

    // proximity bonus: keyword within 30 chars of the number
    if (numIndex !== -1 && nearestDistance(lower, kw, numIndex) <= 30) {
      score += 20;
    }
  }

  // Bonus if the match was found inside a visually prominent element
  if (surroundingText.includes('PROMINENT:')) score += 25;

  return score;
}

export function isFalsePositive(num, context) {
  if (/^(19|20)\d{2}$/.test(num)) return true;  // year
  if (num.startsWith('0') && num.length > 6) return true;  // phone-like
  if (context.includes('$') || context.includes('USD')) return true;  // price

  // Zip codes (State + 5 digits)
  if (new RegExp('\\b[A-Z]{2}\\s+' + num + '\\b').test(context)) return true;

  const lower = context.toLowerCase();
  if (lower.includes('order') || lower.includes('tracking')) return true;
  return false;
}

// Pull the registrable domain out of a From header such as
// "Amazon <noreply@amazon.com>" or a bare "no-reply@accounts.google.com".
export function extractSenderDomain(author) {
  if (!author || typeof author !== 'string') return '';
  const match = author.match(/[\w.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9.-]+/);
  if (!match) return '';
  const domain = match[0].split('@')[1];
  return domain.toLowerCase().replace(/\.+$/, '');
}

function normalizeDomainValue(d) {
  return String(d || '').toLowerCase().replace(/\.+$/, '');
}

// DKIM-Signature headers are `;`-separated tag=value pairs; `d=` is the signing
// domain. An email may carry several signatures (one per hop).
function dkimDomains(headers) {
  const raw = headers?.['dkim-signature'] || [];
  const domains = [];
  for (const value of raw) {
    for (const part of String(value).split(';')) {
      const m = part.trim().match(/^d\s*=\s*(?:"([^"]+)"|([^;\s]+))/i);
      if (m) {
        const d = normalizeDomainValue(m[1] || m[2] || '');
        if (d) domains.push(d);
      }
    }
  }
  return domains;
}

function returnPathDomain(headers) {
  const raw = headers?.['return-path'] || [];
  return extractSenderDomain(raw[0]);
}

// Some services send transactional mail (including OTPs) from a registrable
// domain that differs from the site the user logs in on. Map those sender
// domains to the login domain so the correlation still matches.
const SENDER_DOMAIN_ALIASES = new Map([
  // Facebook sends security/verification mail from facebookmail.com.
  ['facebookmail.com', 'facebook.com'],
]);

export function canonicalizeSenderDomain(domain) {
  const rd = registrableDomain(domain);
  return SENDER_DOMAIN_ALIASES.get(rd) || rd;
}

// Prefer the DKIM signing domain, then the Return-Path, then the From header.
// DKIM and Return-Path are validated by the receiving server, so they are
// harder to spoof than a free-form From display name.
export function resolveSenderDomain(message, headers) {
  const from = extractSenderDomain(message?.author);
  const dkim = dkimDomains(headers);
  let domain;
  if (dkim.length > 0) {
    domain = dkim.find((d) => hostMatchesDomain(d, from)) || dkim[dkim.length - 1];
  } else {
    domain = returnPathDomain(headers) || from;
  }
  return canonicalizeSenderDomain(domain);
}

// Fetch the raw headers and derive the best sender domain. getFull() is the
// only API that exposes headers; it is only called once an OTP candidate has
// been found, so ordinary (non-OTP) mail never pays for it.
async function getSenderDomain(message) {
  try {
    const full = await messenger.messages.getFull(message.id);
    return resolveSenderDomain(message, full.headers);
  } catch (e) {
    return extractSenderDomain(message.author);
  }
}

// ---------------------------------------------------------------------------
// FALLBACK text extraction: manually walk getFull() parts.
// Recovery path for messages whose MIME tree makes listInlineTextParts() throw.
// For HTML parts we use DOMParser to tag prominent elements for the scorer,
// then grab the full body plain text.
// ---------------------------------------------------------------------------
function extractTextFromParts(parts) {
  let text = "";
  if (!parts) return text;

  for (const part of parts) {
    if (part.contentType === "text/plain" && part.body) {
      text += part.body + "\n";

    } else if (part.contentType === "text/html" && part.body) {
      try {
        const parser = new DOMParser();
        const doc = parser.parseFromString(part.body, 'text/html');

        // Remove noise nodes before any extraction
        doc.querySelectorAll('style, script, footer, nav').forEach(el => el.remove());

        // Tag visually prominent elements so the scorer can give them a bonus
        const prominentParts = [];
        doc.querySelectorAll(
          'strong, b, h1, h2, h3, .otp, .code, [class*="otp"], [class*="code"], [class*="verif"]'
        ).forEach(el => {
          const t = el.textContent.trim();
          if (t) prominentParts.push('PROMINENT:' + t);
        });

        // Also tag elements that likely contain only the code
        doc.querySelectorAll('td, th, span, div, p, font').forEach(el => {
          const t = el.textContent.trim();
          if (/^\d{4,8}$/.test(t) || /^[A-Z0-9]{6,10}$/.test(t)) {
            prominentParts.push('PROMINENT:' + t);
          }
        });

        const bodyPlain = doc.body?.textContent || '';
        if (prominentParts.length > 0) {
          // Pad prominent parts to avoid false proximity
          text += prominentParts.join('\n' + ' '.repeat(50) + '\n') + '\n' + ' '.repeat(50) + '\n';
        }
        text += bodyPlain + '\n';
      } catch (e) {
        console.error("DOMParser error", e);
      }

    } else if (part.parts) {
      text += extractTextFromParts(part.parts);
    }
  }

  return text;
}

// ---------------------------------------------------------------------------
// PRIMARY text extraction. Both APIs set the manifest's strict_min_version:
//
//   messenger.messages.listInlineTextParts(id)            - TB 128+
//     - gives us clean content without manually walking the MIME tree
//
//   messenger.messengerUtilities.convertToPlainText(html) - TB 137+
//     - Thunderbird's own HTML-to-plaintext converter; handles tables,
//       encoded entities, and nested elements far better than a hand-rolled
//       DOMParser tag-strip. Borrowed from thunderbird-vericode's approach.
//
// We still run the DOMParser tagging pass on the raw HTML before
// converting, so the scorer's +25 bonus is preserved on this code path too.
//
// extractTextFromParts() catches runtime failures, not missing APIs.
// ---------------------------------------------------------------------------
async function getEmailText(messageId) {
  try {
    const parts = await messenger.messages.listInlineTextParts(messageId);
    let text = '';

    for (const part of parts) {
      if (part.contentType === 'text/plain') {
        text += part.content + '\n';

      } else if (part.contentType === 'text/html') {
        // tagging pass on the raw HTML first, before conversion
        // strips all tags. This preserves our scoring bonus.
        try {
          const parser = new DOMParser();
          const doc = parser.parseFromString(part.content, 'text/html');
          doc.querySelectorAll('style, script, footer, nav').forEach(el => el.remove());

          const prominentParts = [];
          doc.querySelectorAll(
            'strong, b, h1, h2, h3, .otp, .code, [class*="otp"], [class*="code"], [class*="verif"]'
          ).forEach(el => {
            const t = el.textContent.trim();
            if (t) prominentParts.push('PROMINENT:' + t);
          });

          // Also tag elements that likely contain only the code
          doc.querySelectorAll('td, th, span, div, p, font').forEach(el => {
            const t = el.textContent.trim();
            if (/^\d{4,8}$/.test(t) || /^[A-Z0-9]{6,10}$/.test(t)) {
              prominentParts.push('PROMINENT:' + t);
            }
          });

          if (prominentParts.length > 0) {
            // Pad prominent parts to avoid false proximity
            text += prominentParts.join('\n' + ' '.repeat(50) + '\n') + '\n' + ' '.repeat(50) + '\n';
          }
        } catch (e) {
          // tagging failed, continue without the bonus
        }

        // Use Thunderbird's built-in converter for the full body plain text.
        // This handles encoded entities, table layouts, and nested elements
        // far better than a manual tag-strip.
        const plain = await messenger.messengerUtilities.convertToPlainText(part.content);
        text += plain + '\n';
      }
    }

    return text;
  } catch (e) {
    // strict_min_version guarantees both APIs exist, so this is a message
    // they choked on. Retry by walking the MIME tree ourselves.
    console.log("listInlineTextParts failed, falling back to getFull():", e.message);
    const full = await messenger.messages.getFull(messageId);
    return extractTextFromParts(full.parts);
  }
}

export async function processMessage(message) {
  console.log("Processing message");

  const bodyText = await getEmailText(message.id);

  // Subject is a strong prior — if it matches, we trust body extractions more
  const subjectMatches = OTP_SUBJECT_PATTERNS.some(p => p.test(message.subject));

  const candidates = [];

  for (const pattern of OTP_PATTERNS) {
    const regex = new RegExp(pattern.source, pattern.flags);
    let match;

    while ((match = regex.exec(bodyText)) !== null) {
      const num = match[1] || match[0];

      // Skip purely alphabetical matches
      if (/^[A-Za-z]+$/.test(num)) continue;

      const startIndex = Math.max(0, match.index - 50);
      const endIndex = Math.min(bodyText.length, match.index + match[0].length + 50);
      const context = bodyText.substring(startIndex, endIndex);

      if (isFalsePositive(num, context)) continue;

      let score = scoreCandidate(num, context);
      if (subjectMatches) score += 30;

      candidates.push({ num, score });
    }
  }

  if (candidates.length > 0) {
    candidates.sort((a, b) => b.score - a.score);
    const best = candidates[0];

    if (best.score > 0) {
      console.log("Found OTP candidate in email with score:", best.score);
      const senderDomain = await getSenderDomain(message);
      sendToNativeHost({
        command: "new_otp",
        otp: best.num.replace(/[-\s]/g, ''),
        source: message.subject,
        sender_domain: senderDomain
      });
    }
  }
}

if (typeof messenger !== 'undefined') {
  console.log("Tether Mail Extractor loaded in Thunderbird/Betterbird");

  // ---------------------------------------------------------------------------
  // PRIMARY: Poll via messages.query() — the only reliable method for IMAP.
  //
  // onNewMailReceived only fires for POP3/local delivery, never for IMAP.
  // onFolderInfoChanged fires for metadata changes and is unreliable for timing.
  // Polling with a fromDate filter is what production Thunderbird extensions use.
  // ---------------------------------------------------------------------------
  async function pollForNewOtpEmails() {
    const since = new Date(Date.now() - OTP_TTL_MS);

    try {
      let page = await messenger.messages.query({ fromDate: since });

      while (page && Array.isArray(page.messages)) {
        for (const message of page.messages) {
          if (hasSeen(message.id)) continue;
          markSeen(message.id);

          // Pre-filter by subject or sender before fetching the full body,
          // so we don't pay the getEmailText() cost on every single email.
          const subjectMatches = OTP_SUBJECT_PATTERNS.some(p => p.test(message.subject));
          const fromMatches = /noreply|no-reply|security|verify|auth|account/i.test(message.author);

          if (subjectMatches || fromMatches) {
            processMessage(message);
          }
        }

        // messages.query() returns paginated results — walk all pages
        page = page.id ? await messenger.messages.continueList(page.id) : null;
      }
    } catch (e) {
      console.error("pollForNewOtpEmails error:", e);
    }
  }

  // Run immediately on load, then on a regular interval
  pollForNewOtpEmails();
  setInterval(pollForNewOtpEmails, POLL_INTERVAL_MS);

  // ---------------------------------------------------------------------------
  // SECONDARY: onNewMailReceived — fires instantly for POP3 and local delivery.
  // Messages are handed to us directly here so we process them inline rather
  // than re-querying. hasSeen() prevents the poller from double-processing them.
  // ---------------------------------------------------------------------------
  if (messenger.messages?.onNewMailReceived) {
    messenger.messages.onNewMailReceived.addListener(async (folder, messages) => {
      console.log("onNewMailReceived fired for folder:", folder.name);
      for (const message of messages.messages) {
        if (hasSeen(message.id)) continue;
        markSeen(message.id);
        const subjectMatches = OTP_SUBJECT_PATTERNS.some(p => p.test(message.subject));
        const fromMatches = /noreply|no-reply|security|verify|auth|account/i.test(message.author);
        if (subjectMatches || fromMatches) processMessage(message);
      }
    });
  }

  // ---------------------------------------------------------------------------
  // TERTIARY: onFolderInfoChanged — used only as a hint to poll early for IMAP.
  // This event doesn't give us the messages directly, so we just trigger a poll
  // immediately rather than waiting for the next interval. hasSeen() ensures
  // nothing gets processed twice.
  // ---------------------------------------------------------------------------
  if (messenger.folders?.onFolderInfoChanged) {
    messenger.folders.onFolderInfoChanged.addListener((folder, folderInfo) => {
      if (folder.type === 'inbox' || folder.type === 'other') {
        console.log("onFolderInfoChanged hint — polling now for folder:", folder.name);
        pollForNewOtpEmails();
      }
    });
  }

  // ---------------------------------------------------------------------------
  // QUATERNARY: onMessageDisplayed — fires instantly when the user opens an
  // email manually. Zero-latency fallback that works for all account types.
  // ---------------------------------------------------------------------------
  messenger.messageDisplay.onMessageDisplayed.addListener(async (tabId, message) => {
    if (hasSeen(message.id)) return;
    markSeen(message.id);
    processMessage(message);
  });
}
