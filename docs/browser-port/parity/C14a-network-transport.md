# C14a: Network transport (virtual LAN over WebRTC or WebSocket relay)

Class: Complex
Depends on: C01 (hook foundation: memory helpers, symbol macros, app::Settings shim, host-settings channel, selected hook source compilation). Versus LAN verification also needs C13a (versus image runtime).
Native sources: src/xdk/xnet.cpp, src/xdk/hle.h, src/kernel/boot.cpp (Relaunch), src/app/app.h, src/app/settings.cpp, README.md (Playing Online)

## Goal

Two or more browsers find and join each other's System Link sessions through the game's own menus, with no install and no port forwarding. The guest still sees UDP sockets, broadcasts and XNADDRs. The browser carries the datagrams over a virtual LAN: one room per group, a virtual IPv4 per page, WebRTC data channels between peers, and a WebSocket relay when peer to peer fails. This chunk delivers the transport, the XNet and Winsock HLE, the signalling and relay server, the co-op settings side channel, the Coop menu gate and the hosting UX. Co-op lockstep, mouse and local players are C14b. Voice is C14c.

## Native behaviour (cite file:line)

- The network HLE is src/xdk/xnet.cpp (not under src/kernel). Its header (xnet.cpp:1-11): the game reaches the network only through the XNet winsock thunks, replaced here. The XNet object stays null, so Live QoS and XOnline calls fail on their own. An XNADDR's `ina` is the peer's real IPv4. `--instance N` binds 127.0.0.N for local tests (`app::Instance`, src/app/app.h:124). Join IP sends system link search broadcasts to a host beyond the LAN.
- Replaced entries, line numbers in xnet.cpp, each through `HLE_STDCALL` on the image's own SYM_ address (src/xdk/hle.h:13-21): XNetStartup 502, WSAStartup 506, XNetGetEthernetLinkStatus 514 (link up), XOnlineStartup 515, XOnlineCleanup 516, WSAGetLastError 517, XNetRandom 519, XNetCreateKey 523, XNetRegisterKey 529, XNetUnregisterKey 534, XNetGetTitleXnAddr 536, XNetXnAddrToInAddr 541, XNetInAddrToXnAddr 556, XNetDnsLookup 574, XNetDnsRelease 594, socket 614 (UDP; VDP protocol 254 treated as UDP; TCP stream), closesocket 624, shutdown 638, ioctlsocket 643, setsockopt 656, getsockopt 663, getsockname 671, bind 681, connect 699, listen 711, accept 716, select 734, recvfrom 776, recv 793, send 804, sendto 818, WSARecvFrom 872, WSARecv 873, WSASendTo 874, WSASend 875, WSAGetOverlappedResult 877, WSACancelOverlappedIO 906.
- Both images carry these symbols (for example story config/scda_v2.toml:208-229, versus config/sc4_adv.toml:17-18, 45-46, 205-226), so one implementation serves both images.
- Overlapped receives are queued (`PendingRecv`, xnet.cpp:70-73). A pump thread completes them and signals the guest event (`Pump`, xnet.cpp:400-431).
- XNADDR layout: ina, inaOnline, wPortOnline, abEnet[6] at +0x0A, size 0x24 (xnet.cpp:37-39). `OwnXnAddr` derives a MAC from the address (xnet.cpp:433-438).
- Broadcasts to 255.255.255.255 also go to the join address and recent public peers, with NAT port rewrites (`SendOnce`, xnet.cpp:287-321). LAN discovery answers on UDP 9100 (xnet.cpp:7). README.md:157-163 lists UDP 9100, 1000 and 1010 for hosting.
- Co-op settings side channel: on PRUDP port 1000 a host sends `SCDC` version 1, 16 bytes (magic, version, 3 spare, rate, floor) every 250 ms to its PRUDP peers (xnet.cpp:104-136, `SendCoop` 371). The client strips these packets before the game sees them (`Recv`, xnet.cpp:262). The values survive a relaunch via `--coop-host=rate,floor` (`CoopHostValues`, xnet.cpp:478-491; src/kernel/boot.cpp:184-187).
- `CloseSockets` frees ports before a relaunch (xnet.cpp:493-498, called at src/kernel/boot.cpp:189).
- VDP route table for the voice link: peer address to the socket and port the game last sent VDP to (xnet.cpp:80-84, `Send` 322-340, `SendVoiceLink` 454-470). `Recv` hands `SCDV` voice link packets to the audio code before the game (xnet.cpp:262).
- Settings: `join_ip` (src/app/app.h:68; src/app/settings.cpp:119, 178).

## Browser today (cite file:line; where the work plugs in)

- XNet is refused: `XNetStartup` returns 0x276B (WSASYSNOTREADY) (src/browser/sdk-provider.cpp:60-68), policy `SdkPolicy::UnavailableXNet` (src/browser/boot-wasm.cpp:298, 300). No socket entries are replaced.
- The menu route swallows Coop (selection 1) and Versus (selection 2) on the game mode list (src/browser/menu-route.cpp:250-259). Lifting Versus belongs to C13a; lifting Coop belongs here.
- In-process restart from front end to mission keeps the module and rebuilds `Session` (src/browser/boot-wasm.cpp:467-496). Story restart state (launch page, display, counters) lives in the story state struct (boot-wasm.cpp:65-70).
- A host thread can signal a guest event through `Dispatcher::SetEvent` (src/browser/dispatcher.h:97).
- Shared memory transports to copy from: src/browser/audio-transport.mjs and src/browser/webgpu-transport.mjs.
- settings.mjs supports options, booleans and numbers only (src/browser/settings.mjs:1-21); there is no text kind.
- The dev server is static HTTP with cross origin isolation headers, without WebSocket or signalling (tools/browser/serve.mjs:20-40).
- The front end main menu has Start game and Options only (src/browser/menu.html:136-140).

## Design

- Virtual LAN: each page joins a room on a signalling server and gets a virtual IPv4 in 10.88.0.0/24, assigned by the server. The guest sees it as its `ina` and through getsockname.
- A datagram is `{srcIp, srcPort, dstIp, dstPort, payload}`. 255.255.255.255 fans out to every other room member; unicast goes to one member.
- Peer links prefer an `RTCDataChannel` with `ordered: false, maxRetransmits: 0` (UDP semantics). The server WebSocket relays when ICE fails or the user picks relay only.
- Join IP, NAT rewrites and public address tracking from xnet.cpp are not ported; the room replaces them.
- `SCDC` (and `SCDV`, C14c) keep their native formats, so a later UDP bridge to native PC players stays possible.
- Chromium has no RTCPeerConnection in workers, so the network service runs on the page main thread. It exchanges datagrams with the Wasm through two SharedArrayBuffer rings, one per direction. The service outlives runtime restarts and image switches; guest sockets are closed on teardown, as native `CloseSockets` does.

## Tasks (numbered, concrete, each verifiable)

1. Datagram rings: fixed SharedArrayBuffer rings of length prefixed records `{src, dst, payload}` with a sequence word, one per direction, bounded to 256 records and 1500 payload bytes. Write node unit tests first: wrap around, full ring drop counter, oversize rejection, torn read detection.
2. Browser xnet provider (new C++ file under src/browser): virtual sockets with per socket receive queues and an ephemeral port allocator. It covers `socket`, `bind`, `sendto`, `recvfrom`, `select`, `ioctlsocket FIONBIO`, `getsockname`, `setsockopt` (SO_BROADCAST and buffer sizes accepted), and overlapped `WSARecvFrom`, `WSASendTo`, `WSAGetOverlappedResult` and `WSACancelOverlappedIO`, completed through `Dispatcher::SetEvent`. The XNet address calls return the virtual IP. Use the same entry list as xnet.cpp:502-917, wired through C01 symbol macros so both images share the source. TCP entries return WSAEOPNOTSUPP until a trace shows they are used. Native and Wasm unit tests: bind conflicts, broadcast not looped back to the sender, overlapped completion order, cancel, close with pending receives.
3. Policy switch: replace `SdkPolicy::UnavailableXNet` with a networked policy while a room is joined. Keep the refusal when no network service exists, so offline behaviour is unchanged. Verify both with a trace of `XNetStartup` results.
4. Page network service `net-service.mjs`: room join, virtual IP, peer table, offer and answer over the signalling socket, one data channel per peer, relay fallback, broadcast fan out, and per peer RTT and loss counters. Write unit tests with fake RTCPeerConnection and WebSocket: routing, fallback after ICE failure, member leave, rejoin.
5. Signalling and relay server `tools/browser/relay.mjs` (node). Use no new dependency if a minimal RFC 6455 server stays small; otherwise pin one. It provides rooms by code, virtual IP assignment, SDP and ICE forwarding, and datagram relay with a per room rate limit and payload cap. Test two fake clients exchanging datagrams through it.
6. `SCDC` side channel: port `TakeCoopPacket` and `SendCoop` (xnet.cpp:104-136, 371) onto virtual port 1000. Keep the received host values in the story state next to the launch page (boot-wasm.cpp:65-70) so they survive `image_restart`, replacing the native `--coop-host` argument. Expose `CoopHostValues` and `SetCoopHost` with the native signatures for C14b. Unit test parse and strip.
7. Lift the Coop gate: allow selection 1 in src/browser/menu-route.cpp:250. Verify in two tabs that the co-op menus open and System Link lists a session hosted by the other tab.
8. Teardown: on runtime restart, image switch or Stop, close all guest sockets and drop queued datagrams, while keeping the room and peer links. Verify the restarted runtime can bind port 1000 and 9100 again.
9. Hosting UX in menu-shell.mjs and menu.html (see Settings and UI). Show clear errors for server unreachable, room full and peer lost.

## Settings and UI (settings.mjs entries, Options panel / main menu changes)

- settings.mjs gains a text kind: trimmed string with a maximum length and an allowed charset, with normalisation tests.
- `netServer`: text, initial the relay path on the page origin; Developer tab.
- `netTransport`: options `auto`, `relay`; initial `auto`; Network tab. Relay only hides peer IPs from each other.
- `playerName`: text, shown in the room peer list only.
- Main menu: a "Play together" button opens a panel with Host (creates a room and shows its code and an invite link `?room=CODE`), Join (enter a code) and Leave, plus the peer list with RTT and the transport in use. Opening the page with `?room=` joins once the ISO is ready. After that the players use the game's own menus as on a LAN (Coop or Versus, System Link or LAN, pick the session).

## Acceptance criteria

- Two tabs on one machine, and two machines on different networks, join one room and exchange datagrams over WebRTC; with `relay` selected they exchange them through the server.
- A co-op host's System Link session appears in the client's session list, and the client can join the lobby.
- With no room joined, XNet is still refused and no network traffic leaves the page.
- After a front end to mission restart, both runtimes rebind their ports and the room stays connected.
- After C13a: a versus LAN session hosted in one tab is listed in the other.

## Testing

- Unit tests first (node): rings, routing table, broadcast fan out, relay fallback, room protocol, settings text kind, `SCDC` parsing.
- Native and Wasm tests for the xnet provider with a loopback transport (two providers in one process).
- Browser: agent-browser CLI with two sessions against a separate served build dir and a local relay on its own port. Never use port 8125 or out/boot-m5. Mute audio with out/front-end/mute.js.
- Soak: 30 minutes in a lobby with two tabs, logging RTT, ring drops and heap.

## Risks and open questions

- Production hosting: who runs the signalling and relay server, and is TURN needed? Symmetric NAT and strict corporate networks defeat WebRTC, and the relay then carries all traffic with extra latency.
- Privacy: WebRTC exposes candidate IPs to peers. Relay only mode avoids that.
- Does either image use TCP (`connect`, `accept`, `listen`)? Both define the symbols. Trace before implementing.
- Main thread jank delays datagrams, because the service lives on the page. Measure; a WebSocket only transport could move to a worker if needed.
- Does the Quazal session reconnect after the co-op front end to mission restart, with the virtual sockets reopened? Verify early.
- How many players does versus support, and how much traffic does each send? Unknown until C13a boots versus.
- Interop with native PC players would need a UDP bridge on the relay. It is not planned here, but the formats stay compatible.

## Out of scope

- Co-op lockstep, co-op mouse and local second player (C14b).
- Communicator, voice and the `SCDV` link (C14c).
- Xbox Live, XOnline and matchmaking beyond rooms.
- A UDP bridge to native PC players.
