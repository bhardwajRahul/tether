// Background worker script
// This runs in the background of the browser or mail client

// Import the native messaging module if supported (MV3 modules)
// For MV2 (Thunderbird), we might need to load this differently or bundle it.
import {
  connectToNativeHost,
  sendToNativeHost,
  registerOtpRequest,
  clearOtpRequest,
} from '../shared/native.js';

let nativePort = connectToNativeHost();

function isValidHostname(host) {
  return typeof host === 'string' && host.length > 0 && host.length <= 253;
}

// Listen for messages from content scripts or the mail extractor
if (typeof chrome !== 'undefined' && chrome.runtime) {
  chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (!request || typeof request !== 'object') return;

    // Only accept messages that originate from one of our own content scripts
    // running in a tab. Web pages and other extensions cannot reach us here,
    // but validate the sender explicitly anyway.
    const fromTab = sender && sender.tab && typeof sender.tab.id === 'number';
    if (!fromTab) return;

    if (request.action === "found_otp_in_email") {
      // Legacy path from older mail builds; the mail extractor now talks to the
      // daemon directly. Kept for compatibility.
      if (typeof request.otp === 'string' && request.otp.length > 0) {
        sendToNativeHost({
          command: "new_otp",
          otp: request.otp,
          source: typeof request.source === 'string' ? request.source : ''
        });
      }
      sendResponse({ status: "sent_to_daemon" });
      return;
    }

    if (request.action === "request_otp_for_site") {
      if (!isValidHostname(request.url)) {
        sendResponse({ status: "invalid_host" });
        return;
      }
      // Remember which tab asked, so the OTP is delivered back to it alone.
      registerOtpRequest(sender.tab.id, request.url);
      // Query the C++ daemon for an OTP.
      sendToNativeHost({ command: "request_otp", url: request.url });
      sendResponse({ status: "requested" });
      return;
    }

    if (request.action === "consume_otp") {
      clearOtpRequest(sender.tab.id);
      sendToNativeHost({ command: "consume_otp", otp_id: request.otp_id || 0 });
      sendResponse({ status: "consumed" });
      return;
    }
  });
}
