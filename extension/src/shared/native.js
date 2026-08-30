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

// A page host matches a pinned sender domain when it is equal or a subdomain:
// "amazon.com" matches "amazon.com" and "www.amazon.com", but not
// "amazon.com.evil.com" or "evilamazon.com". An empty domain means the OTP is
// not pinned and any host may take it.
export function hostMatchesDomain(host, domain) {
  const d = normalizeDomain(domain);
  if (!d) return true;
  const h = normalizeDomain(host);
  if (h === d) return true;
  return h.endsWith('.' + d);
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
