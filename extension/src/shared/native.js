// Native messaging connection logic
let port = null;

// Tabs that have asked for an OTP. Delivery is scoped to these tabs only so a
// code meant for the site the user is logging into can't be harvested by some
// other tab the user happens to have open.
// tabId -> { host, ts }
const requestingTabs = new Map();
const REQUEST_TTL_MS = 3 * 60 * 1000; // an OTP flow runs ~2 minutes

export function registerOtpRequest(tabId, host) {
  if (typeof tabId !== 'number') return;
  requestingTabs.set(tabId, { host: String(host || ''), ts: Date.now() });
}

export function clearOtpRequest(tabId) {
  requestingTabs.delete(tabId);
}

// Tests need to reset the module-level registry between runs.
export function _resetOtpRequests() {
  requestingTabs.clear();
}

function pruneOtpRequests() {
  const cutoff = Date.now() - REQUEST_TTL_MS;
  for (const [id, req] of requestingTabs) {
    if (req.ts < cutoff) requestingTabs.delete(id);
  }
}

export function connectToNativeHost() {
  const hostName = "com.tether.extension";
  if (typeof browser !== 'undefined' && browser.runtime && browser.runtime.connectNative) {
    port = browser.runtime.connectNative(hostName);
  } else if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.connectNative) {
    port = chrome.runtime.connectNative(hostName);
  } else {
    console.error("Native messaging not supported in this environment");
    return null;
  }

  port.onMessage.addListener((message) => {
    handleNativeMessage(message);
  });

  port.onDisconnect.addListener((p) => {
    let errorMsg = "unknown reason";
    if (p.error) {
        errorMsg = p.error.message;
    } else if (typeof browser !== 'undefined' && browser.runtime.lastError) {
      errorMsg = browser.runtime.lastError.message;
    } else if (typeof chrome !== 'undefined' && chrome.runtime.lastError) {
      errorMsg = chrome.runtime.lastError.message;
    }
    console.log("Disconnected from Tether daemon. Reason:", errorMsg);
    port = null;
  });

  return port;
}

export function sendToNativeHost(message) {
  // Under MV3 the service worker (and its native host) can be torn down between
  // sends; lazily re-establish the port so messages and the subscription survive.
  if (!port) {
    connectToNativeHost();
  }
  if (port) {
    port.postMessage(message);
  } else {
    console.error("Not connected to Tether daemon");
  }
}

function isValidOtp(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= 64;
}

function normalizeDomain(d) {
  return String(d || '').toLowerCase().replace(/\.+$/, '');
}

// Common two-label public suffixes. Without these, "amazon.co.uk" would be
// reduced to "co.uk" and match every *.co.uk site. This is a curated subset of
// the Public Suffix List covering the common country-code second-level domains.
const MULTI_PART_SUFFIXES = new Set([
  'co.uk', 'org.uk', 'gov.uk', 'ac.uk', 'net.uk', 'me.uk', 'ltd.uk', 'plc.uk', 'sch.uk',
  'com.au', 'net.au', 'org.au', 'edu.au', 'gov.au', 'asn.au', 'id.au',
  'co.jp', 'or.jp', 'ne.jp', 'ac.jp', 'go.jp',
  'co.nz', 'net.nz', 'org.nz', 'govt.nz', 'ac.nz',
  'com.br', 'net.br', 'org.br', 'gov.br', 'edu.br',
  'co.in', 'net.in', 'org.in', 'gen.in', 'firm.in', 'ind.in',
  'co.kr', 'or.kr', 'ne.kr', 'go.kr', 'ac.kr',
  'com.mx', 'net.mx', 'org.mx', 'edu.mx', 'gob.mx',
  'com.ar', 'net.ar', 'org.ar', 'gob.ar', 'edu.ar',
  'co.za', 'org.za', 'net.za', 'gov.za', 'ac.za',
  'com.sg', 'net.sg', 'org.sg', 'gov.sg', 'edu.sg',
  'com.hk', 'net.hk', 'org.hk', 'edu.hk', 'gov.hk', 'idv.hk',
  'com.tw', 'net.tw', 'org.tw', 'edu.tw', 'gov.tw',
  'com.cn', 'net.cn', 'org.cn', 'gov.cn', 'edu.cn',
  'com.tr', 'net.tr', 'org.tr', 'gov.tr', 'edu.tr',
  'com.ua', 'net.ua', 'org.ua', 'gov.ua', 'edu.ua',
  'com.ru', 'net.ru', 'org.ru',
  'co.id', 'net.id', 'or.id', 'go.id', 'ac.id',
  'co.th', 'in.th', 'go.th', 'ac.th',
  'com.vn', 'net.vn', 'org.vn', 'gov.vn', 'edu.vn',
  'co.il', 'org.il', 'net.il', 'gov.il', 'ac.il',
  'com.ph', 'net.ph', 'org.ph', 'gov.ph', 'edu.ph',
  'com.my', 'net.my', 'org.my', 'gov.my', 'edu.my',
  'com.pk', 'net.pk', 'org.pk', 'gov.pk', 'edu.pk',
  'com.eg', 'net.eg', 'org.eg', 'gov.eg', 'edu.eg',
  'com.sa', 'net.sa', 'org.sa', 'gov.sa', 'edu.sa',
  'com.ng', 'net.ng', 'org.ng', 'gov.ng', 'edu.ng',
  'co.ke', 'or.ke', 'ne.ke', 'go.ke', 'ac.ke',
  'com.co', 'net.co', 'org.co', 'gov.co', 'edu.co',
  'com.pe', 'net.pe', 'org.pe', 'gob.pe', 'edu.pe',
  'com.ec', 'net.ec', 'org.ec', 'gob.ec', 'edu.ec',
  'com.uy', 'net.uy', 'org.uy', 'gub.uy', 'edu.uy',
  'com.ve', 'net.ve', 'org.ve', 'gob.ve', 'edu.ve',
  'com.py', 'net.py', 'org.py', 'gov.py', 'edu.py',
  'com.bo', 'net.bo', 'org.bo', 'gob.bo', 'edu.bo',
  'com.do', 'net.do', 'org.do', 'gov.do', 'edu.do',
  'com.gt', 'net.gt', 'org.gt', 'gob.gt', 'edu.gt',
  'co.cr', 'or.cr', 'go.cr', 'ac.cr'
]);

// eTLD+1 approximation: strip the public suffix and return it plus one label.
export function registrableDomain(host) {
  const d = normalizeDomain(host);
  if (!d) return '';
  // IP literals are not DNS names; compare them verbatim.
  if (/^\d{1,3}(\.\d{1,3}){3}$/.test(d) || d.includes(':')) return d;

  const labels = d.split('.');
  if (labels.length === 1) return d;

  const lastTwo = labels.slice(-2).join('.');
  if (MULTI_PART_SUFFIXES.has(lastTwo)) {
    if (labels.length === 2) return d;
    return labels.slice(-3).join('.');
  }
  return lastTwo;
}

// A page host matches a pinned sender domain when they share the same
// registrable domain: "accounts.google.com" matches "google.com", and
// "www.amazon.co.uk" matches "amazon.co.uk", while "amazon.com.evil.com" and
// "evilamazon.com" never match "amazon.com". An empty domain means the OTP is
// not pinned and any host may take it.
export function hostMatchesDomain(host, domain) {
  const d = normalizeDomain(domain);
  if (!d) return true;
  return registrableDomain(host) === registrableDomain(d);
}

// Deliver an OTP only to tabs that asked for one, most-relevant first, and stop
// at the first tab that reports it actually filled a field. We never blast the
// code to unrelated tabs: a page with an OTP-shaped input must not be handed a
// code it did not request, and when the code came from email we only hand it to
// the domain the email was sent by.
export function deliverOtpToTabs(message) {
  if (typeof chrome === 'undefined' || !chrome.tabs) return;
  if (!message || !isValidOtp(message.otp)) return;

  pruneOtpRequests();

  const senderDomain = typeof message.sender_domain === 'string' ? message.sender_domain : '';

  chrome.tabs.query({}, function(tabs) {
    const byId = new Map((tabs || []).map((t) => [t.id, t]));

    const candidates = [...requestingTabs.entries()]
      .filter(([id, req]) => byId.has(id) && hostMatchesDomain(req.host, senderDomain))
      .sort((a, b) => {
        const ta = byId.get(a[0]);
        const tb = byId.get(b[0]);
        // Prefer the tab the user is actually looking at.
        if (!!ta.active !== !!tb.active) return ta.active ? -1 : 1;
        // Then the most recently requesting tab.
        return (b[1].ts || 0) - (a[1].ts || 0);
      })
      .map(([id]) => id);

    (function next(i) {
      if (i >= candidates.length) return;
      const tabId = candidates[i];
      chrome.tabs.sendMessage(
        tabId,
        { action: "fill_otp", otp: message.otp, otp_id: message.otp_id },
        (response) => {
          // Tabs without a content script (about:, the web store, ...) set lastError.
          void chrome.runtime.lastError;
          if (response && response.filled) {
            sendToNativeHost({ command: "consume_otp", otp_id: message.otp_id });
          } else {
            next(i + 1);
          }
        }
      );
    })(0);
  });
}

function handleNativeMessage(message) {
  if (!message || typeof message !== 'object') return;
  // Only react to the commands we understand; ignore anything else.
  if (message.command === "otp_available") {
    deliverOtpToTabs(message);
  }
}
