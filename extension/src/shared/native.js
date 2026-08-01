// Native messaging connection logic
let port = null;

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
    console.log("Received message from Tether daemon:", message);
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

// offer the code to tabs one at a time, most-recently-used first, and stop at the
// first tab that reports it actually filled a field.
export function deliverOtpToTabs(message) {
  if (typeof chrome === 'undefined' || !chrome.tabs) return;

  chrome.tabs.query({}, function(tabs) {
    const ordered = [...(tabs || [])].sort((a, b) => (b.lastAccessed || 0) - (a.lastAccessed || 0));

    (function next(i) {
      if (i >= ordered.length) return;
      chrome.tabs.sendMessage(
        ordered[i].id,
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
  // Dispatch native messages to other parts of the extension
  if (message.command === "otp_available" && message.otp) {
    deliverOtpToTabs(message);
  }
}
