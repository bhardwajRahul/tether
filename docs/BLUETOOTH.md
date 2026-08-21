# Bluetooth: iPhone messages and notifications

Tether accesses SMS/iMessage and other apps' notifications via Bluetooth because iOS apps are
not allowed to access these. It presents the Linux machine to iOS as a paired
Bluetooth accessory. No iOS-side code is involved.

| Feature | Mechanism | Transport | Linux role |
|---|---|---|---|
| SMS / iMessage, read + send | **MAP** (Message Access Profile) via BlueZ `obexd` | BR/EDR | OBEX client |
| Contacts, for sender names | **PBAP** (Phonebook Access Profile) via `obexd` | BR/EDR | OBEX client |
| Notifications from any app | **ANCS** (Apple Notification Center Service) | BLE / GATT | GATT central |

The Tether iOS app is unrelated to these features and just keeps handling clipboard sync and file transfer over TCP + mTLS.

## Delivery modes

Not every machine can do all of it, so Tether resolves one of two modes from live capabilities.

- Full mode: MAP + PBAP + ANCS. Requires BR/EDR, LE, and advertising support, plus
  BlueZ >= 5.86 running with its experimental bearer API (org.bluez.Bearer.LE1).
- Compatibility mode: MAP + PBAP only. Messages and contacts work, notification mirroring does not (older BlueZ).

`tether --bt-status` reports which applies. `scripts/bt-probe.sh` is a
repo-only development probe that covers a few things the daemon does not check
(BlueZ version, `obex.service`, tooling); it is not installed by any package.

## Setup requirements

Two of them need a system change, and both are reported with the exact command by:

```bash
tether --bt-setup
```

It prints only what is still missing and never applies anything: both steps
change how the machine behaves over Bluetooth outside Tether. The GTK app shows
the same list on the Devices page with a "Copy commands" button. The rest of this
section is what those commands do and why.

Class of Device must be A/V Hands-Free (major 4, minor 8). iOS only offers the
"Show Message Notifications" and "Sync Contacts" permissions to a device presenting this
class. `sudo btmgmt class 4 8` sets it for now.

**It does not survive a `bluetoothd` restart**. bluetoothd rewrites the
class from its own default every time it starts. Already-granted session survives that, but a phone that has not yet granted the permissions will refuse MAP and PBAP with an OBEX error that seems like a missing service record rather than
like a permissions problem.
The packaged unit handles that:

```bash
sudo systemctl enable --now tether-btclass@hci0
```

`PartOf=bluetooth.service` re-runs it on every `bluetooth.service` restart. It writes the
class, reads it back, and retries for ten seconds.

**BlueZ needs the experimental bearer API** for ANCS, and it must be active
*before* pairing: a bond made without it has no LE half. The package ships the
drop-in, already pointing at this distro's `bluetoothd`:

```bash
sudo mkdir -p /etc/systemd/system/bluetooth.service.d
sudo cp /usr/share/tether/bluetooth-experimental.conf /etc/systemd/system/bluetooth.service.d/
sudo systemctl daemon-reload && sudo systemctl restart bluetooth
```

It is shipped to `/usr/share` rather than installed into
`bluetooth.service.d` directly, because a drop-in there takes effect the moment
the package lands, and changing how `bluetoothd` runs for the whole machine is
the user's decision.

Without it `bluetoothd` still registers `org.bluez.Bearer.LE1`, but as an empty
marker: no properties, no `Connect()`. So the interface being present is not
evidence the API is available, and code that reads it that way sees an LE bearer
that can never connect and a bond that never looks dual.

**`obexd` must be running** (user service `obex`) for MAP and PBAP. It is socket-activated under normal use.

## Permissions on the phone

After pairing, the iPhone offers "Show Message Notifications" and "Sync Contacts" under
Settings -> Bluetooth -> (i) for the computer's entry. Off by default.

- The toggles can take minutes to appear after paring, and appear only while the ANCS 
  advertisement is actively broadcasting. Failed MAP connection attempts alone do not surface them.
- Closing and reopening the entry's detail page refreshes what iOS shows.
- A failed pairing can leave two records for the same computer, the toggles may appear
  under either one. Check both, and delete both before retrying a clean pairing.

Without the relevant permission, MAP or PBAP is visible at the SDP level but rejects the
OBEX connection with `Forbidden` / `0x43`. That is a permissions state, not a pairing
failure (do not re-pair). A transport-level `Connection refused (111)` is different and
usually means another computer already owns the iPhone's single MAP session.

## Conflicts

The iPhone serves one MAP session at a time. Any other program on any machine holding it will block Tether.

## Known limits

These are properties of what iOS exposes:

- MAP reports both SMS and iMessage as `Type: sms-gsm`. The transport a message used is not knowable.
- The iPhone's MAP sent folder is empty and sending produces no useful outgoing event, so sent history cannot be recovered from the phone. Tether records its own sends.
- ANCS supports positive/negative notification actions only. There is no free-text reply over ANCS, replies go through MAP.
- No attachments, reactions, typing indicators, or read receipts.
- MAP gives no conversation identifier and no participant list for group messages, so group support is a guess and is conservative by default.

## Troubleshooting

Start with the read-only checks. `tether --bt-setup` says what system setup is
still missing, `tether --bt-status` says what the hardware and the stack can do,
and `tether --bt-connection` says what is actually up right now. From a source
checkout, `./scripts/bt-probe.sh` adds BlueZ-version, tooling, and `obexd`
checks the daemon does not make.

| Symptom | Cause | What to do |
|---|---|---|
| Messages and contacts worked, then stopped, and the error mentions a service record | `bluetoothd` restarted and reset the Class of Device | `sudo systemctl enable --now tether-btclass@hci0`, then re-pair if the phone dropped the bond |
| The phone never offers notifications / Sync Contacts | The class is wrong, or the ANCS advertisement is not running | Check for `class=ok` in `tether --bt-status`, can take minutes |
| MAP or PBAP reports `forbidden` | The matching toggle on the phone is off | Turn it on. This is not a pairing failure |
| MAP reports `busy`, or the transport says `Connection refused (111)` | Another computer holds the iPhone's single MAP session | Stop the other client |
| Pairing fails with `br-connection-key-missing` | A stale bond on one side, or the adapter is not `Pairable` | Delete the computer's entry on the phone (Forget This Device) and `tether --bt-unpair <addr>` locally, then pair again |
| The phone shows two entries for this computer | A failed pairing left both a Classic and an LE record | Delete both on the phone before retrying |
| LE never connects and the log repeats `org.bluez.Error.InProgress` | BlueZ is holding an auto-connect registration that never completed | `sudo systemctl restart bluetooth` -- nothing short of that clears it, see 2026-08-19 below. With `tether-btclass@hci0` enabled the class survives the restart |
| The status says "Nothing is listening for the iPhone's LE link" | BlueZ refused to register for the phone's LE connection, so nothing is waiting for it | `sudo systemctl restart bluetooth`. This is different from waiting on the phone, which the status says instead when a registration is open |
| LE never connects and the log repeats `le-connection-abort-by-local` | Something on this side is cancelling the connection. Tether's own cause was a `PreferredBearer` write racing the async connect, fixed; anything else writing that property during a connect will do the same | Check no other Bluetooth tool is driving the same device. The phone is not the cause: `abort-by-local` means the local host cancelled |
| Notifications stopped and never came back, while messages and contacts kept working | Fixed. The bearer supervisor used to stop retrying LE after six attempts, and only a Classic drop or a daemon restart re-armed it | Nothing. LE is now retried indefinitely at the five-minute ceiling, and the ANCS solicitation is kept on air whenever LE is down |
| `tether --bt-status` reports `Bond: BR/EDR only` | The bond was made without cross-transport key derivation, so it has no LE half and can never carry ANCS | Forget this computer on the iPhone and pair again. Check `secure-connections` in the same output first: re-pairing cannot help while it is off |
| The status says the iPhone is not answering on LE, and its permission is on | The phone's Bluetooth stack is wedged, which the granted permission does not prevent | Turn Bluetooth off and back on **on the iPhone**. Re-pairing and re-toggling the permission do not clear this |
| Everything connects but `ancs_ready` stays false | Compatibility mode, or iOS has not authorized notification content yet | Check `Mode:` in `tether --bt-status`. In full mode the daemon retries, the first request returns `NotPermitted` until the prompt on the phone is approved |
| A group conversation cannot be replied to | Working as designed until the route is unambiguous | The thread's `reply_reason` says which condition failed |
| The iPhone's audio moves to the computer when Tether connects | The machine advertises itself as a Bluetooth speaker/headset, and iOS routes to it. Not caused by Tether beyond bringing the link up | See "Keeping the phone's audio on the phone" below |

### Keeping the phone's audio on the phone

Once the Classic link is up, the iPhone's calls, music, and system sounds play on the
computer instead of the phone. PipeWire registers A2DP sink and HFP audio-gateway
endpoints for every adapter, so the machine advertises itself as a speaker and headset,
and iOS routes to a bonded device that offers one. Tether only brings the link up; the
routing decision is the phone's. Every desktop with Bluetooth audio behaves this way.

To confirm it, with the phone connected:

```bash
pactl list cards
```

The phone appears as `bluez_card.<ADDR>` with `Active Profile: audio-gateway`.

The fix is to stop advertising the roles a phone connects to, while keeping the ones
headphones use. On WirePlumber 0.5 and later, write
`~/.config/wireplumber/wireplumber.conf.d/51-no-phone-audio.conf`:

```
monitor.bluez.properties = {
  bluez5.roles = [ a2dp_source hfp_ag bap_source ]
}
```

Then `systemctl --user restart wireplumber` and reconnect the phone. Role names are
from this machine's perspective, not the remote device's: `a2dp_source` and `hfp_ag`
are the roles that drive headphones, `a2dp_sink` and `hfp_hf` are the roles that make
the machine a destination for a phone. Dropping the second pair leaves the iPhone
nothing to route to, so its audio stays local, while headphones keep both A2DP
playback and the HFP microphone. `bap_source` keeps LE Audio playback; drop it too if
nothing here uses LE Audio. On WirePlumber 0.4 the same setting goes in
`~/.config/wireplumber/bluetooth.lua.d/51-no-phone-audio.lua` as
`bluez_monitor.properties["bluez5.roles"]`.

Two things that look like fixes and are not:

- `pactl set-card-profile bluez_card.<ADDR> off` stops the computer playing the audio,
  and WirePlumber remembers it in `~/.local/state/wireplumber/default-profile`, but the
  iPhone still believes it is routed to the computer. The audio goes nowhere and the
  phone is silent.
- `device.disabled = true` in a `monitor.bluez.rules` entry does nothing. Only the
  alsa, v4l2, and libcamera monitors honour that property.

### Reporting a problem

```bash
tether --bt-diagnostics
```

Prints the delivery mode, auth strategy, Bluetooth settings, current connection state, and timeline of recent link and pairing transitions.

It is redacted for pasting into an issue. Bluetooth addresses, phone numbers, email
addresses, and home and runtime directories become numbered placeholders.
Messages, contact names, and notification content are dropped, and message and notification events never enter the timeline at all. Read it before
you post it anyway.

## Recorded results

Per the maintenance rule below, every entry records phone model, iOS version, BlueZ
version, and controller, and distinguishes what was captured from what was inferred.

### 2026-07-11 — MediaTek MT7925, BlueZ 5.87, iPhone 15 Pro full mode

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| Kernel | 7.1.8-arch1-3 (Arch Linux) |
| BlueZ | 5.87, running with `-E` |
| Adapter roles | central + peripheral; 15 advertising instances |
| Adapter class | `0x7c0408` — A/V Hands-Free |
| Phone | iPhone 15 Pro, iOS _(version not recorded - fill in)_ |

Captured observations against the bonded phone:

- Dual bond confirmed. `Device1` reports `Paired`, `Bonded`, `Connected`, and `Trusted` all true, and `org.bluez.Bearer.LE1` independently reports `Paired`, `Bonded`, and `Connected` all true. One bond covers BR/EDR and LE.
- ANCS is live. Service `7905f431-b5ce-4e99-a40f-4b1e122d00d0` is in the GATT tree along with all three characteristics: Notification Source
  `9fbf120d-6301-42d9-8c58-25e699a21dbd`, Control Point
  `69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9`, and Data Source
  `22eac6e9-24d6-4bb5-be44-b36ace7c7bfb`.
- MAP and PBAP are advertised in the device's profile UUID list: `0x1132` (Message Access Server), `0x1133` (Message Notification Server), and `0x112f` (Phonebook Access Server).
- Apple Media Service `89d3502b-0f36-433a-8ef4-c502ad55f8dc` is also present.

### 2026-07-13 — MAP/PBAP refused while ANCS worked

Captured with the bond fully connected (`Connected` and `ServicesResolved` true, `Bearer.LE1` connected) and the phone unlocked and attended:

- Opening either OBEX session failed with `org.bluez.obex.Error.Failed: Unable to find service record`.
- The same call made by hand with `busctl --user call org.bluez.obex ... CreateSession` failed identically, so this is obexd's SDP lookup, not a Tether problem.
- `bluetoothd` logged the layer underneath:
  `record_cb() Unable to get Hands-Free unit SDP record: Connection refused`
  and `connect to <phone>: Connection refused (111)`. obexd reports a missing
  record whenever its SDP fetch is refused, so "Unable to find service record"
  can be a refusal with a misleading name.
- A full `Disconnect()` / `Connect()` cycle forced fresh SDP discovery. BlueZ
  still reported `0x1132`, `0x1133`, and `0x112f` afterwards, so those UUIDs were
  not stale, yet obexd still could not fetch the records. BlueZ's device
  discovery and obexd's record fetch can disagree even on fresh data.
- ANCS worked throughout. The LE/GATT half of the bond is healthy while the BR/EDR profile half is refused, independent.

Resolved on 2026-07-13. The cause was neither candidate listed at the time: 
the adapter's Class of Device had reverted to Computer/Laptop, so iOS
no longer treated the machine as an eligible accessory and refused the record
fetch.

Also worth knowing: a `Connect()` that times out leaves an attempt in flight, and
further attempts fail fast with `br-connection-busy`. Retrying through that state
prolongs it, which is why the bearer supervisor backs off exponentially
instead of retrying on a fixed interval.

### 2026-07-16 - Cause of the OBEX refusal

Same hardware as above. The phone was made to forget this computer and its Bluetooth
stack was reset, which cleared the condition and allowed a clean re-pair through
Tether's own `bt_pair` for the first time.

Class of Device is the cause of the refused record fetch. `btmgmt class 4 8` does
not persist across a `bluetoothd` restart! The adapter reverts to Computer/Laptop
(`0x...010c`). iOS declines to serve MAP and PBAP SDP records and the
refusal is a `Connection refused (111)` under obexd's misleading "Unable to find
service record". Restoring the class and re-pairing opened PBAP immediately. This makes
the CoD a runtime dependency, not a one-time setup step. Anything that restarts
`bluetoothd` silently breaks messages and contacts until it is set again.

The adapter must be `Pairable` for connect-first to work. Idles with `Pairable: no`. 
Because connect-first deliberately makes the iPhone the authentication initiator, BlueZ then refuses the inbound pairing request: the Linux side
displays its numeric comparison, iPhone never shows one at all, and fails with `br-connection-key-missing`. 
Tether now turns `Pairable` and `Discoverable` on for the duration of the transaction and restores both afterwards.

A stale record on the phone reproduces `br-connection-key-missing` indefinitely. A
failed authentication can leave iOS holding a record for the computer while Linux has
none. The bond must be removed on both sides (Forget This Device and `bluetoothctl remove`) before retrying. 
iOS shows no prompt in this state, which is what distinguishes it from a user-cancelled pairing.

Results of the successful transaction:

- Discovery -> `Device1.Connect()` on the unpaired device -> numeric check on both screens -> `Paired`, 
then the ANCS solicitation.
- `Device1` and `org.bluez.Bearer.LE1` both report `Paired` and `Bonded`, and
  `BREDR.Connected` and `LE.Connected` are both true. Connect-first produced the
  cross-transport bond on this controller, upgrading the previous line's inference to a capture.
- PBAP opened on the first attempt. MAP reported `Forbidden`, which is a permission
  rather than the transport refusal above, and exactly the distinction `classify_obex_error()` exists to make.
  Granting "Show Message Notifications" cleared it within one poll, with no re-pair and no session restart.
- Messages then listed from `telecom/msg/inbox` and threaded correctly.

Obexd API details, both on BlueZ 5.87 and neither matching what the D-Bus documentation says (shocker):

- `ListMessages` returns `(a{oa{sv}})`: dict keyed by message object path,
  not the `(a(oa{sv}))` array of structs that the API reference says. GDBus rejects the reply on type mismatch(!!?!)
- `MessageAccess1.SetFolder` walks from the session root. `"inbox"`
  alone fails with `Internal Server Error`. `"telecom/msg/inbox"` succeeds, as does
  `"/telecom/msg/inbox"` and a `"telecom"` step. `ListMessages` takes a folder arg relative to the current folder, so an empty string lists the folder
  `SetFolder` already selected.

Message listing fields as actually delivered: `Subject` (body),
`Timestamp` (ISO-8601) local time with no zone suffix (`20254816T509517`),
`Sender`, `SenderAddress`, `Recipient`, `RecipientAddress`, `Type` (`sms-gsm` for both SMS and iMessage), 
`Read`, `Sent`, `Folder`, `Size`, and `ConversationId`.
`ConversationId` present but zero on every message, confirming IT CANNOT BE a thread identifier(?!?).

`SenderAddress` is in whatever form the phone has (both bare national number (`5129328901`) and (`+15129328901`) in the same inbox (!?!).
Tether does not invent a country code, so the same person can appear as two threads if their number is in the phone both ways.

A message's D-Bus object path is not a stable identifier. obexd names messages
`/org/bluez/obex/client/session<N>/message<id>`, and `<N>` is the OBEX session number,
which increments on every reconnect. Same message observed as `session9/message…` and `session11/message…` across a restart.
Only the trailing `<id>` is stable, so it is what Tether dedupes and persists on. 
The full path is kept separately and refreshed on each re-listing, because it is what `Message1` calls must
address. Keying history on the path instead duplicates the entire mailbox on every reconnect.

### Sending

`MessageAccess1.PushMessage(sourcefile, folder, args)` takes a file, so the 
bMessage is staged in `$XDG_RUNTIME_DIR` mode 0600 and removed afterwards (`Charset` must be `utf8`, 
let obexd transcode to the native charset mangles anything outside the phone's default encoding.)

Success means the phone accepted the message, not that it delivered it. MAP reports
nothing back about delivery. Tether records its own sends locally, and that record is the
only evidence they happened. A push that times out is reported.

`TYPE` is always `SMS_GSM`, including for AppleID recipients: iOS decides between SMS
and iMessage on its own, and nothing on the Linux side can force or check that choice.

### ANCS

On the same hardware: after the LE bearer connects, BlueZ enumerates the ANCS
service and all three characteristics: Notification Source, Control Point and Data Source, under the device object, and `StartNotify` succeeds on both notifying
characteristics.

The permission is in Settings -> Bluetooth -> (i) -> Share System Notifications, and it is separate from the Message Notifications toggle that MAP needs.

Control Point responses have no request identifier, so only one request may be out at a time. And Data Source
responses carry no total length (?!?) and come back fragmented, so the only way to know
response has ended is to know exactly which attributes were requested. Those two facts
are why the sequencer exists.

### Group messages

Off by default, `group_messages_enabled` in `~/.config/tether/bluetooth.json`. It also
needs `ancs_content_enabled` (on by default, `tether --bt-ancs-content off` to disable),
so group support cannot work without content mirroring.

MAP delivers a group message with one sender, no participant list and no conversation
identifier. The only other hint is to correlate Apple Messages ANCS notification:
its title is the sender, and its subtitle is either `To you & ...` for an unnamed group or
the group's name. Neither form contains a member list...

- Correlation is bounded to a 30-second window, and two notifications with the same
  text are refused instead of guessing, the wrong choice would put a message in the wrong conversation and send a reply at the wrong people.
- An unnamed group is repliable only when **every** participant name resolves to exactly one contact address. A name matching several contacts is refused.
- A named group stays read-only until we figure out the member list in `~/.config/tether/groups.json`.
    That list affects Tether's reply routing only and never modifies the group on the phone.
- iOS reports nothing when a member is added or removed, so an unknown sender is the only available signal that a member list has gone stale.
- Because iOS supplies a name but no conversation identifier, distinct named groups
  sharing a name collapse into one local thread. There is no way to tell them apart.

Repeated recipient vCards were observed to enter an existing iMessage group when the
recipient set matched, rather than fanning out into separate one-to-one threads. That is
an observation, not a guarantee.

### 2026-07-16: a reverted Class of Device does not revoke a granted session

Same hardware. `systemctl restart bluetooth` reverted the class from
`0x7c0408` to `0x7c010c` (Computer / Laptop), exactly as expected. MAP, PBAP, and ANCS
then reconnected and stayed up, with `tether --bt-status` reporting `class=wrong`
throughout.

So the class governs whether iOS offers and grants the Messages and Contacts permissions,
not whether it keeps honoring permissions it has already granted. The earlier refusal
recorded above happened while those grants were being established. Setting the class is
still required, just not continuously, which is why the breakage is delayed
and looks unrelated to the restart that caused it.

### 2026-07-16: LE bearer stuck on `InProgress`

Same hardware. After the daemon stopped and restarted twice within about twenty seconds while an LE bearer
connect was outstanding, every subsequent `Bearer.LE1.Connect` returned
`org.bluez.Error.InProgress` indefinitely. BR/EDR, MAP, and PBAP were unaffected and stayed up throughout, only ANCS was lost.

`bluetoothctl disconnect` on the device did not clear it, so the stuck state lives above
the ACL. `systemctl restart bluetooth` did clear it: LE connected and ANCS reported
`Notification mirroring is active` on the first attempt afterwards, with no other change.

The supervisor previously did its retry backoff on this error the same way it does on a
refusal. It now retries `InProgress` at the base interval and reserves the exponential growth for refusals,
which is what the phone actually sends when it declines. That does not fix the stuck state, but it stops the daemon from
sleeping through the recovery.

### 2026-08-19 - `Bearer.LE1.Connected` is false on a working ANCS link

| | |
|---|---|
| Controller | MediaTek MT7925 (RZ717) Wi-Fi 7 |
| BlueZ | 5.87, running with `--experimental` |
| Phone | iPhone 15 Pro |

Captured with notification mirroring demonstrably live -- `ancs: Notification
mirroring is active`, the full GATT tree enumerated under the device including
all three ANCS characteristics, notifications arriving:

- `org.bluez.Bearer.LE1.Connected` read **false** throughout, while
  `Paired` and `Bonded` on the same interface read true.
- The link was opened by the phone, inbound, in answer to the solicitation
  advert. BlueZ appears not to credit the LE bearer for a connection it did not
  initiate. An outbound `Bearer.LE1.Connect()` sets it true as expected.

So `Bearer.LE1.Connected` is evidence that LE is up, not evidence that it is
down. Trusting it alone reported "notifications are unavailable" over a session
that was delivering them, and kept the supervisor dialling a link that was
already open.

**Corrected later the same day.** The first version of this entry concluded that
the presence of the ANCS characteristics in the object tree was the better
signal, on the evidence that an LE-down device showed only `avrcp` and `sep1-6`
under its path. That evidence was from a boot in which no LE session had yet been
established. BlueZ caches a bonded device's attributes (`main.conf` `[GATT]
Cache`, default `always`), so once a session has existed the `service*` objects
persist across a disconnect, and reading their presence as a live link reported
LE up on a dead one -- which stopped the bearer supervisor dialling it at all.

Neither obvious property can be trusted alone:

| Signal | Fails when |
|---|---|
| `Bearer.LE1.Connected` | reads false on a live link the phone opened inbound |
| ANCS characteristics present | survive the disconnect, because BlueZ caches them |

`Notifying` on the Notification Source characteristic is the one that holds. A
notify session needs a live ATT link to exist, so it cannot outlive the link the
way a cached attribute does, and it was true throughout the working session
above. That is what `Device::le_link_up()` reads, and what anything tearing the
bearer down refuses to act against.

### 2026-08-19 - The solicitation advert was a one-shot

Same hardware. `LEAdvertisingManager1.ActiveInstances` sat at `0` on a machine
whose LE link had been down for hours, with BR/EDR, MAP and PBAP up throughout.

The advert carries `Timeout = 180`, BlueZ retires it and calls `Release`, and
nothing re-registered it: the only callers were `pair_device()` and
`--bt-solicit`. Since iOS reveals the notification permission, and dials the LE
link, only while it is broadcasting, a bond went permanently quiet three minutes
after pairing unless a human kept running `--bt-solicit`.

It is now driven from the connection loop: on air whenever BR/EDR is up, ANCS is
enabled and LE is down, re-armed each time BlueZ retires it, and unregistered as
soon as LE connects so the advertising instance is freed. A pairing transaction
still owns it outright while it runs.

Measured on the machine above, which had been in the broken state all day: the
daemon brought LE up 12 seconds after starting, with no manual step, and reported
`Notification mirroring is active`.

### 2026-08-19 - A subscription latch reported the phone's permission as missing

Same hardware, with all three iPhone toggles granted and verified.

`ancs_ready` stayed false with the reason "Waiting for notification access to be
allowed on the iPhone", which sends the user to settings that are already
correct. `Notifying` on both notify characteristics read false at the time.

`AncsClientState::subscribed` recorded that `StartNotify` had once succeeded, and
nothing re-read it. BlueZ clears `Notifying` when the LE link goes and restores
the characteristics without it, so the flag outlived what it described. The only
thing that used to clear it was `set_device()` seeing the device path change,
which the ANCS grace window and the cached GATT tree together prevented.

`tick()` now re-reads `Notifying` from BlueZ once per retry interval while
`ready` is false, and rebuilds the subscription -- or the whole discovery, when
the characteristics are gone -- rather than trusting the flag. The status text is
reached only with the subscription verified live, so it now names the toggle
(`Share System Notifications`) and means it.

### 2026-08-19 - A hung LE dial poisons the path until bluetoothd restarts

Same hardware, immediately after the above. Every `Bearer.LE1.Connect` was
*accepted* -- `ConnectResult::Requested`, no error -- and no link ever appeared,
while `StartNotify` on the same device answered `org.bluez.Error.InProgress`
indefinitely. `Bearer.LE1.Disconnect` succeeded and changed nothing.

This is worse than the `InProgress` case recorded on 2026-07-16. Three ways out
were tried against it directly, with the daemon stopped so nothing raced them:

| Attempt | Result |
|---|---|
| `Bearer.LE1.Disconnect` | `Not Connected`. Does not cancel a pending connect |
| `Device1.Disconnect` | Succeeds, BR/EDR returns within seconds, LE still `In Progress` |
| Waiting | `In Progress` indefinitely |

**`sudo systemctl restart bluetooth` is the only thing that clears it**, and a
bearer-level recovery built on `Bearer.LE1.Disconnect` was removed again after
this measurement rather than shipped as a remedy that does not remedy anything.

The consequence for the retry policy is the important part. A hung dial is not
free: it poisons `Bearer.LE1.Connect` for that device for the rest of
bluetoothd's life. Six dials after a clean restart were enough to do it. So the
cap on outbound LE attempts is deliberate and stays, and the link is expected to
come from the phone answering the solicitation advert instead -- which is why
that advert now stays on air the whole time LE is down. `reset()` re-arms the
attempts when the device or the Classic link returns, which includes every
bluetoothd restart, so a machine that recovers gets fresh dials without ever
accumulating them.

### 2026-08-19 - What an LE "connect" actually is, captured

`btmon` over a full session, including a clean re-pair. Two findings that change
how the LE half should be driven.

**`Bearer.LE1.Connect` does not dial.** Six Connect calls produced six
`Add Device` with `Action: Auto-connect remote device (0x02)`, one
`LE Add Device To Accept List`, one `LE Add Device To Resolving List`, eighteen
`LE Set Extended Scan Enable` -- and **zero** `LE Create Connection`. BlueZ
registers the phone for background auto-connect and waits for it to advertise.
An iPhone that is already BR/EDR-connected does not advertise, so the request
simply expires.

So the supervisor now holds exactly one registration open for as long as LE is
down (`BearerOps::le_connect_outstanding()`) instead of repeating a dial.
Repeating was what wedged BlueZ: the request runs for `LE_CONNECT_TIMEOUT_MS`
(45s) while the backoff re-fired after five, so registrations overlapped and
every later one answered `InProgress`. `Requested` and `Busy` are both the
healthy state now -- BlueZ is listening -- and only a refusal counts or backs off.

**The advertisement is correct**, which rules out a long-standing suspicion. It
goes out as `Use legacy advertising PDUs: ADV_IND`, connectable and scannable,
public address, 31 bytes exactly:

```
11 15 <ANCS UUID, little-endian>   solicitation, AD type 0x15
04 ff ff ff 00                     manufacturer data, company 0xffff
04 16 99 99 00                     service data, 16-bit UUID 0x9999
02 01 06                           flags: LE General Discoverable, BR/EDR Not Supported
```

with `Tether` in an 8-byte scan response. The 16-bit form of the service data is
what keeps it inside the legacy 31-byte limit. Nothing here needs trimming.

### 2026-08-19 - Cross-transport key derivation, captured

The re-pair produced a dual bond, and the capture shows exactly how. Immediately
after the BR/EDR link key lands, SMP runs **over the BR/EDR channel**:

```
HCI Event: Link Key Notification
BR/EDR SMP: Pairing Request    Authentication requirement: ... CT2 (0x20)
                               Responder key distribution: EncKey IdKey Sign
BR/EDR SMP: Pairing Response   Responder key distribution: EncKey IdKey
BR/EDR SMP: Identity Information          (the phone's IRK)
BR/EDR SMP: Identity Address Information
MGMT Event: New Identity Resolving Key
MGMT Event: New Long Term Key  Key type: Authenticated key from P-256 (0x03)
```

`Authenticated key from P-256` is the point: the LE LTK is derived from the
BR/EDR Secure Connections link key, which is why Secure Connections is a hard
precondition and why `--bt-status` reports it. The `CT2` bit in the
authentication requirements is what asks for the derivation.

**This is the check for a bond that has no LE half.** Capture a pairing with
`sudo btmon -w /tmp/pair.btsnoop` and look for `BR/EDR SMP: Pairing Request`
followed by `New Long Term Key`. If the SMP exchange never happens, or the key
type is not a P-256 one, the bond is BR/EDR-only and no amount of re-soliciting
or restarting will give it ANCS -- it has to be re-made, and Secure Connections
has to be on first.

The `Role Change ... Role: Peripheral` right before the exchange is also worth
noting: the iPhone takes the central role on the ACL, which is what connect-first
pairing exists to allow.

### 2026-08-19 - `le-connection-abort-by-local` was our own PreferredBearer write

After a reboot, LE would not come up at all: every attempt logged
`org.bluez.Error.Failed: le-connection-abort-by-local`, while the same
`Bearer.LE1.Connect` issued by hand succeeded immediately.

The difference was not the advertisement. The supervisor used to do this:

```
set PreferredBearer = "le"
Bearer.LE1.Connect()          <- asynchronous, returns at once
set PreferredBearer = "bredr" <- milliseconds later, connect still in flight
```

The second write lands while BlueZ is still setting the LE connection up, and
BlueZ abandons it -- which is exactly what `abort-by-local` says: the local host
cancelled it, not the phone.

`PreferredBearer` steers `Device1.Connect`. This path calls the per-bearer
`Bearer.LE1.Connect`, which already names the transport, so the property was
never needed here. It is now written only on the `Device1.Connect` fallback used
by BlueZ builds that publish `Bearer.LE1` without a `Connect` method. With the
writes removed, LE connected on the first attempt from a cold LE-down state and
held for a three-minute soak with zero aborts.

**An earlier reading of this was wrong and is corrected here.** The first
experiment compared "advert on air" against "advert off" and concluded that
advertising and initiating could not coexist without the kernel's Simultaneous
Central and Peripheral feature. That comparison was confounded: the advert was
only ever off while the *daemon was stopped*, so it was also the only condition
in which nothing was rewriting `PreferredBearer`. The advert was not shown to
interfere, and no evidence here says it does.

Tether still sequences the two -- the solicitation stays off air while outbound
attempts are being spent (`BearerStatus::le_dialling`) -- because doing one thing
at a time is cheap now that the connect succeeds on the first attempt. That is a
conservative choice, not a measured requirement.

### 2026-08-19 - The iPhone can go silent on LE with the permission still granted

After a long debugging session -- many connect attempts, several bluetoothd
restarts, a re-pair and a reboot -- LE stopped coming up at all, with everything
on the Linux side verifiable and correct:

- bond dual (`Bearer.LE1` Paired and Bonded), Secure Connections on, class ok
- `Share System Notifications` present **and on** in the iPhone's settings
- both routes provably running: the dial and solicitation phases alternating,
  the advertisement on air 180 seconds at a time
- phone unlocked, in range, BR/EDR + MAP + PBAP working throughout

The failure had changed shape, which is what identified it. A quiescent
`Bearer.LE1.Connect` -- daemon stopped, nothing advertising -- returned
`Connection timed out`, where the same call earlier in the day had returned
success. Not `abort-by-local` (this side cancelling), not `In Progress` (BlueZ
wedged): a properly issued request that nothing answered.

**Cycling Bluetooth off and on on the iPhone cleared it immediately.** The link
came up, ANCS subscribed, `Notifying` true on both characteristics, and a
three-minute soak held with no errors. Nothing on the Linux side changed.

So an iPhone will stop answering on LE while still showing the permission as
granted, and neither toggling that permission nor re-pairing is the remedy --
only cycling the phone's Bluetooth. This is the same shape as the BR/EDR refusal
recorded above, and likely provoked by the sheer volume of connect attempts the
session put at the phone.

Because the two causes need opposite remedies, the status text now separates
them: a transient failure reports that the iPhone is not answering and names the
phone-side Bluetooth cycle, while a refused registration still points at
`systemctl restart bluetooth`. Sharing one message for both is what sent this
session chasing the local stack for an hour.

### 2026-08-20 - Three ways the LE cycle sabotaged itself

A cold boot came up with BR/EDR, MAP and PBAP working and LE never forming, on a
machine that had soaked cleanly the evening before. The local stack measured
correct throughout: `bearer_api: confirmed`, `bond_has_le: true`,
`secure_connections: true`, CoD `0x7c0408`, and the solicitation advert on air
(`ActiveInstances` alternating 0 and 1 on the expected 45s/180s cycle). The
phone was the immediate cause -- a dial with nothing competing, against a freshly
restarted `bluetoothd`, returned `Connection timed out` in 25s, and 180s of
uninterrupted solicitation went unanswered. But reading the daemon log across a
full cycle turned up three faults of our own, each of which turns one bad minute
into a permanent outage.

**The solicitation aborted our own dial.** `connect_le()` is asynchronous, and
`le_dialling` was derived from the clock alone. When the 45s dial window closed
the advert went up immediately, while BlueZ was still connecting, and the connect
died with `le-connection-abort-by-local` -- observed once per cycle, forever. The
window now stays open while a dial is outstanding and closes on the reply, so the
two routes really do alternate instead of overlapping. `connect_le()` abandons
the call at its own timeout, which is what bounds the window.

**Dialling stopped for good after six failures.** `LE_ATTEMPTS_BEFORE_ADVICE`
gated the retry as well as the advice string it is named for, and `le_failures_`
is cleared only by `reset()` or by an observed LE link -- neither of which
happens while BR/EDR stays up. Six failures into a daemon's life, LE was never
dialled again. Combined with the abort above, that is roughly twenty minutes from
boot to a daemon that will not try. This is the "worked all morning, then stopped
until I restarted it" report. The cap now selects the message only; the retry
runs as long as LE is down, paced by the existing 300s backoff ceiling.

**PreferredBearer was written under a live LE link.** The Classic path called
`set_preferred_bearer("bredr")` on every reconnect attempt, including while LE
was up, which drops the LE link -- the mirror image of the race removed on
2026-08-19, where the LE path wrote `"le"` under an in-flight connect. A bug
reporter independently described the consequence exactly: the bond stays
`BR/EDR + LE` but only one bearer holds a live connection at a time, trading off
every few minutes. The property steers the untyped `Device1.Connect` and nothing
else, so it is now written only on that fallback, which is reached only with
nothing connected. It is gone from `BearerOps` entirely -- the supervisor cannot
touch it by construction, which is a stronger guarantee than the test it replaces.

None of the three explains a phone that will not answer. All three explain why it
never recovered once it stopped.

Cycling Bluetooth on the iPhone brought LE straight back -- first dial, no
aborts, both ANCS notify characteristics subscribed, clean over a soak. But the
daemon that had been failing all morning did not notice for minutes: LE shared
BR/EDR's 300s backoff ceiling, so a phone the user had just fixed still read as
broken. LE now backs off to its own 60s ceiling. A dial costs nothing since one
is never issued while another is outstanding, and the failure it recovers from
is the one a human has just cleared by hand.

### PBAP

`Select("int", "pb")` followed by `PullAll` with `Format: vcard30` and `MaxCount` returned 456 contacts on the first attempt.

The transfer object disappears from D-Bus the moment it finishes, so a vanished object
is a normal terminal state rather than a failure (file may still take a bit to appear afterwards),
which is why the pull waits instead of giving up. Contacts are staged in `$XDG_RUNTIME_DIR` rather than `/tmp`, since a
phonebook is personal data, and the cache and journal are both written mode 0600.

***THIS HAS TO CHANGE BEFORE PUBLIC RELEASE.***

## Credits

[BlueFerry](https://github.com/erikwb/blueferry) (Erik Bourget and contributors), `PROTOCOL.md` records the findings this implementation relies on.
Tether's Bluetooth support is an independent implementation written against those published
findings, Apple's ANCS specification, the Bluetooth SIG MAP and PBAP specifications, and
the BlueZ D-Bus API.
