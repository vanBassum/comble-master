// Singleton backend service — all communication over a single WebSocket.

import { DEV_HOST } from "@/config"

const TOKEN_KEY = "device.token"

// ── Types ────────────────────────────────────────────────────────

type BroadcastHandler = (msg: Record<string, unknown>) => void
type BinaryHandler = (data: ArrayBuffer) => void

interface PendingRequest {
  resolve: (data: unknown) => void
  reject: (err: Error) => void
  timer: ReturnType<typeof setTimeout>
  timeoutMs: number
  chunks: Uint8Array[]
  received: number
  // When set, each non-final reply chunk is parsed as its own JSON message and
  // passed here (e.g. upload progress); the final chunk resolves the request.
  // Absent → default behaviour: accumulate all chunks and parse once at FINAL.
  onMessage?: (msg: Record<string, unknown>) => void
  // Binary reply mode (e.g. partition download): accumulate raw chunks and
  // resolve with the reassembled Uint8Array instead of parsing JSON. onData
  // reports cumulative bytes received, for progress.
  binary?: boolean
  onData?: (received: number) => void
}

export type ConnectionStatus = "connected" | "connecting" | "disconnected"
type StatusHandler = (status: ConnectionStatus) => void
type AuthHandler = (authenticated: boolean) => void

// ── Service ──────────────────────────────────────────────────────

// Session ids correlate a reply with its request. The device is single-in-flight
// (one active session at a time), so opens are SERIALIZED through a FIFO queue
// (see `enqueue`): callers still fire concurrently, but only one session is open
// on the wire at once, and the next starts on the previous reply's FINAL. Ids
// stay within 16 bits to match the wire.
let nextSession = 1

const FLAG_FINAL = 0x01
const FLAG_REJECT = 0x02

// The device UI runs in two places, and its WebSocket follows the page:
//   • served by the device            → ws://<device>/ws
//   • served through the relay server → ws://<server>/devices/<id>/ws
//
// Resolving "ws" against the page's own directory covers both, which keeps this
// file ignorant that a relay exists — no device id is parsed here. `pnpm dev`
// still talks to DEV_HOST.
function resolveWsUrl(): string {
  const proto = location.protocol === "https:" ? "wss:" : "ws:"
  if (import.meta.env.DEV) return `${proto}//${DEV_HOST}/ws`

  // Drop the document name so "/devices/x/" and "/devices/x/index.html" both
  // resolve to "/devices/x/ws".
  const dir = location.pathname.replace(/[^/]*$/, "")
  return `${proto}//${location.host}${dir}ws`
}

class BackendService {
  private ws: WebSocket | null = null
  private pending = new Map<number, PendingRequest>()
  private broadcastHandlers = new Set<BroadcastHandler>()
  private binaryHandlers = new Set<BinaryHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null
  private connecting: Promise<void> | null = null
  // Tail of the open-serialization queue. Each enqueued task runs only after the
  // previous one has fully settled (its reply's FINAL received, or it failed).
  private queue: Promise<unknown> = Promise.resolve()
  private _status: ConnectionStatus = "disconnected"
  private token: string | null = sessionStorage.getItem(TOKEN_KEY)
  private authHandlers = new Set<AuthHandler>()
  private _authenticated = false
  private _authResolved = false

  get status(): ConnectionStatus {
    return this._status
  }

  get authenticated(): boolean {
    return this._authenticated
  }

  // True once the first handshake has settled either way (authed or
  // needs-login). Lets a late-subscribing hook skip its "checking" state
  // instead of waiting on the timeout fallback.
  get authResolved(): boolean {
    return this._authResolved
  }

  get hasToken(): boolean {
    return this.token !== null
  }

  onAuthChange(fn: AuthHandler): () => void {
    this.authHandlers.add(fn)
    return () => {
      this.authHandlers.delete(fn)
    }
  }

  private setAuthenticated(auth: boolean) {
    this._authenticated = auth
    this._authResolved = true
    this.authHandlers.forEach((fn) => fn(auth))
  }

  private setStatus(s: ConnectionStatus) {
    if (s !== this._status) {
      this._status = s
      this.statusHandlers.forEach((fn) => fn(s))
    }
  }

  onStatusChange(fn: StatusHandler): () => void {
    this.statusHandlers.add(fn)
    return () => {
      this.statusHandlers.delete(fn)
    }
  }

  connect() {
    this.ensureConnected().catch(() => {})
  }

  private ensureConnected(): Promise<void> {
    if (this.ws?.readyState === WebSocket.OPEN) return Promise.resolve()
    if (this.connecting) return this.connecting
    return this.doConnect()
  }

  private doConnect(): Promise<void> {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    if (this.connecting) return this.connecting

    this.setStatus("connecting")

    const p = new Promise<void>((resolve, reject) => {
      const ws = new WebSocket(resolveWsUrl())
      ws.binaryType = "arraybuffer"
      let opened = false

      ws.onopen = () => {
        opened = true
        this.ws = ws
        this.setStatus("connected")
        void this.doHandshake()   // establishes auth in-band; sets authenticated
        resolve()
      }

      ws.onmessage = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          this.onBinaryChunk(ev.data)
          return
        }
        if (typeof Blob !== "undefined" && ev.data instanceof Blob) {
          ev.data.arrayBuffer().then((buf) => this.onBinaryChunk(buf))
        }
      }

      ws.onclose = () => {
        this.ws = null
        this.stopHeartbeat()
        this.setStatus("disconnected")
        // Keep `authenticated` as-is across a brief drop — the reconnect's
        // auth{key} either resumes silently or fails (then doHandshake shows login).
        for (const [, req] of this.pending) {
          clearTimeout(req.timer)
          req.reject(new Error("WebSocket closed"))
        }
        this.pending.clear()
        if (!opened) reject(new Error("Connection failed"))
        this.reconnectTimer = setTimeout(() => {
          this.doConnect().catch(() => {})
        }, 2000)
      }

      ws.onerror = () => ws.close()
    })

    this.connecting = p
    p.catch(() => {}).then(() => {
      if (this.connecting === p) this.connecting = null
    })
    return p
  }

  // Establish auth in-band right after the socket opens: hello tells us whether
  // auth is required; if so, resume with the stored key or fall back to the
  // login page. Runs on every (re)connect.
  private async doHandshake() {
    try {
      const info = await this.send<{ authRequired: boolean }>("auth hello")
      if (!info.authRequired) {
        this.setAuthenticated(true)
        this.startHeartbeat()
        return
      }
      if (this.token) {
        const res = await this.send<{ ok: boolean }>("auth resume", { key: this.token })
        if (res.ok) {
          this.setAuthenticated(true)
          this.startHeartbeat()
          return
        }
        this.token = null
        sessionStorage.removeItem(TOKEN_KEY)
      }
      this.setAuthenticated(false)   // needs login
    } catch {
      /* socket died mid-handshake — onclose handles the reconnect */
    }
  }

  private startHeartbeat() {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => {
      if (this.ws?.readyState !== WebSocket.OPEN) return
      this.send("system ping").catch(() => {
        this.setStatus("disconnected")
        this.ws?.close()
      })
    }, 15000)
  }

  private stopHeartbeat() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
  }

  // Run `task` after every previously-enqueued task has settled — the
  // open-serialization queue. A task's failure never stalls the queue.
  private enqueue<T>(task: () => Promise<T>): Promise<T> {
    const run = this.queue.then(task, task)
    this.queue = run.then(
      () => {},
      () => {},
    )
    return run
  }

  private allocSession(): number {
    const session = nextSession
    nextSession = nextSession >= 0xffff ? 1 : nextSession + 1
    return session
  }

  // Send one session chunk: [session:u16 LE | flags | payload].
  private sendChunk(session: number, flags: number, payload: Uint8Array) {
    const frame = new Uint8Array(3 + payload.length)
    frame[0] = session & 0xff
    frame[1] = (session >> 8) & 0xff
    frame[2] = flags
    frame.set(payload, 3)
    this.ws!.send(frame)
  }

  // Register a pending reply for `session`; resolves when its FINAL chunk
  // arrives (reassembled in onBinaryChunk), rejects on REJECT/timeout/close.
  // For a streamed reply the timeout is idle-based: onBinaryChunk bumps it on
  // each chunk (see bumpTimer), so it bounds silence, not total transfer time.
  private awaitReply<T>(
    session: number,
    opts: {
      timeoutMs?: number
      onMessage?: (msg: Record<string, unknown>) => void
      binary?: boolean
      onData?: (received: number) => void
    } = {},
  ): Promise<T> {
    const timeoutMs = opts.timeoutMs ?? 10000
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(session)
        reject(new Error("Request timeout"))
      }, timeoutMs)
      this.pending.set(session, {
        resolve: resolve as (data: unknown) => void,
        reject,
        timer,
        timeoutMs,
        chunks: [],
        received: 0,
        onMessage: opts.onMessage,
        binary: opts.binary,
        onData: opts.onData,
      })
    })
  }

  // Restart a pending reply's idle timeout — called on each streamed chunk so a
  // large-but-steady transfer isn't killed by the total-duration cap.
  private bumpTimer(session: number) {
    const req = this.pending.get(session)
    if (!req) return
    clearTimeout(req.timer)
    req.timer = setTimeout(() => {
      this.pending.delete(session)
      req.reject(new Error("Request timeout"))
    }, req.timeoutMs)
  }

  async send<T>(
    type: string,
    params: Record<string, unknown> = {},
  ): Promise<T> {
    return this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<T>(session)
      // Request = one FINAL session chunk: the command JSON + '\n' (the device
      // splits the header line from any body; these commands have no body).
      const body = new TextEncoder().encode(JSON.stringify({ type, ...params }) + "\n")
      this.sendChunk(session, FLAG_FINAL, body)
      return reply
    })
  }

  // Reassemble a reply from its session chunks. Each chunk is
  // [session:u16 LE | flags | payload]; FLAG_FINAL ends the reply, FLAG_REJECT
  // is a transport/framework refusal whose payload is the reason.
  private onBinaryChunk(data: ArrayBuffer) {
    const view = new Uint8Array(data)
    if (view.length < 3) return
    const session = view[0] | (view[1] << 8)
    const flags = view[2]

    // Session 0 is reserved for device-initiated broadcasts (log lines).
    if (session === 0) {
      try {
        this.broadcastHandlers.forEach((fn) => fn(JSON.parse(new TextDecoder().decode(view.subarray(3)))))
      } catch {
        /* malformed broadcast — ignore */
      }
      return
    }

    const req = this.pending.get(session)
    if (!req) {
      // No matching request — hand to legacy binary subscribers (unused here).
      this.binaryHandlers.forEach((fn) => fn(data))
      return
    }
    if (flags & FLAG_REJECT) {
      this.pending.delete(session)
      clearTimeout(req.timer)
      req.reject(new Error(new TextDecoder().decode(view.subarray(3)) || "rejected"))
      return
    }

    // Binary reply (e.g. partition download): accumulate raw chunks, report
    // cumulative bytes for progress, and resolve with the reassembled bytes at
    // FINAL. The caller interprets the payload (image bytes, or a short JSON
    // error object the device may send instead).
    if (req.binary) {
      const chunk = view.subarray(3)
      req.chunks.push(chunk)
      req.received += chunk.length
      this.bumpTimer(session)
      req.onData?.(req.received)
      if (flags & FLAG_FINAL) {
        this.pending.delete(session)
        clearTimeout(req.timer)
        const buf = new Uint8Array(req.received)
        let off = 0
        for (const c of req.chunks) {
          buf.set(c, off)
          off += c.length
        }
        req.resolve(buf)
      }
      return
    }

    // Streaming reply: each chunk is one complete JSON message. Intermediate
    // chunks go to onMessage; the FINAL chunk resolves the request.
    if (req.onMessage) {
      const text = new TextDecoder().decode(view.subarray(3))
      if (flags & FLAG_FINAL) {
        this.pending.delete(session)
        clearTimeout(req.timer)
        try {
          req.resolve(text.length ? JSON.parse(text) : {})
        } catch (e) {
          req.reject(e instanceof Error ? e : new Error("bad reply"))
        }
      } else if (text.length) {
        this.bumpTimer(session)
        try {
          req.onMessage(JSON.parse(text))
        } catch {
          /* ignore a malformed progress message */
        }
      }
      return
    }

    req.chunks.push(view.subarray(3))
    if (flags & FLAG_FINAL) {
      this.pending.delete(session)
      clearTimeout(req.timer)
      const total = req.chunks.reduce((a, c) => a + c.length, 0)
      const buf = new Uint8Array(total)
      let off = 0
      for (const c of req.chunks) {
        buf.set(c, off)
        off += c.length
      }
      const text = new TextDecoder().decode(buf)
      try {
        req.resolve(text.length ? JSON.parse(text) : {})
      } catch (e) {
        req.reject(e instanceof Error ? e : new Error("bad reply"))
      }
    }
  }

  subscribe(fn: BroadcastHandler): () => void {
    this.broadcastHandlers.add(fn)
    this.ensureConnected().catch(() => {})
    return () => {
      this.broadcastHandlers.delete(fn)
    }
  }

  subscribeBinary(fn: BinaryHandler): () => void {
    this.binaryHandlers.add(fn)
    this.ensureConnected().catch(() => {})
    return () => {
      this.binaryHandlers.delete(fn)
    }
  }

  // ── API methods ──────────────────────────────────────────────

  async getInfo(): Promise<DeviceInfo> {
    return this.send<DeviceInfo>("system info")
  }

  async getLogs(): Promise<LogsResponse> {
    return this.send<LogsResponse>("log list")
  }

  async getUpdateStatus(): Promise<UpdateStatus> {
    return this.send<UpdateStatus>("partition status")
  }

  async getSettings(): Promise<SettingsResponse> {
    return this.send<SettingsResponse>("settings list")
  }

  async setSetting(key: string, value: string): Promise<{ ok: boolean }> {
    return this.send("settings set", { key, value })
  }

  async saveSettings(): Promise<{ ok: boolean }> {
    return this.send("settings save")
  }

  async wifiScan(): Promise<WifiScanResponse> {
    return this.send<WifiScanResponse>("wifi scan")
  }

  async getPartitions(): Promise<PartitionsResponse> {
    return this.send<PartitionsResponse>("partition list")
  }

  async reboot(): Promise<{ ok: boolean }> {
    return this.send("system reboot")
  }

  async getLed(): Promise<LedState> {
    return this.send<LedState>("led get")
  }

  /** One frame of the device's own touchscreen, as the panel is drawing it.
   *
   *  The reply is not one document: a JSON header record, a newline, then the frame
   *  as raw RGB565 (little-endian uint16, `stride` bytes per row) — the same shape
   *  `web read` uses for a file. So it is read as a binary session and split at the
   *  first newline, rather than parsed as JSON.
   *
   *  `scale` decimates on the DEVICE, which is the only place worth doing it: a full
   *  320x480 frame is 300 KB and most of a second of radio time, while the same frame
   *  at scale 2 is 75 KB and still more than a browser needs to show what the panel
   *  is doing. */
  async capturePanel(scale: 1 | 2 | 4 = 1): Promise<PanelFrame> {
    const buf = await this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<Uint8Array<ArrayBuffer>>(session, {
        timeoutMs: 20000,
        binary: true,
      })
      const body = new TextEncoder().encode(
        JSON.stringify({ type: "ui screenshot", scale }) + "\n",
      )
      this.sendChunk(session, FLAG_FINAL, body)
      return reply
    })

    const nl = buf.indexOf(0x0a)
    if (nl < 0) throw new Error("capture reply had no header")

    const header = JSON.parse(new TextDecoder().decode(buf.subarray(0, nl))) as {
      ok: boolean
      error?: string
      screen: string
      width: number
      height: number
      stride: number
      scale: number
      bytes: number
    }
    // A refusal (no display, out of memory) is a successful reply carrying ok:false,
    // so it has to be read out of the payload or it becomes a zero-pixel "frame".
    if (!header.ok) throw new Error(header.error ?? "capture failed")

    const pixels = buf.subarray(nl + 1)
    if (pixels.length < header.bytes)
      throw new Error(`short frame: got ${pixels.length} of ${header.bytes} bytes`)

    return {
      screen: header.screen,
      width: header.width,
      height: header.height,
      stride: header.stride,
      scale: header.scale,
      pixels,
    }
  }

  /** Press the panel from here. Coordinates are in the panel's own pixels — 0,0 top
   *  left, whatever `capturePanel` reported as width and height — and a "tap" is held
   *  long enough on the device for LVGL to poll it, so it lands as a real press and
   *  release rather than a blip between two polls. */
  async touchPanel(
    x: number,
    y: number,
    action: "tap" | "press" | "release" | "move" = "tap",
  ): Promise<PanelTouchResult> {
    return this.send<PanelTouchResult>("ui touch", {
      x: Math.round(x),
      y: Math.round(y),
      action,
    })
  }

  /** Put a named screen on the panel: home, settings, wifi, password, slaves, code,
   *  pairing. Mostly a convenience for scripts — a person uses the panel itself. */
  async gotoPanelScreen(screen: string): Promise<{ ok: boolean; screen?: string }> {
    return this.send("ui goto", { screen })
  }

  /** Turns the link indication on or off. Omitted fields are left alone by the
   *  device, so `{}` is a no-op that still reports the current state. */
  async setLed(params: { enabled?: boolean }): Promise<LedState> {
    return this.send<LedState>("led set", params)
  }

  /** Returns false on wrong password; throws on connection failure. On success
   *  stores the session key and marks the connection authenticated. */
  async login(password: string): Promise<boolean> {
    await this.ensureConnected()
    const res = await this.send<{ ok: boolean; key?: string }>("auth login", { password })
    if (!res.ok) return false
    this.token = res.key ?? null
    if (this.token) sessionStorage.setItem(TOKEN_KEY, this.token)
    this.setAuthenticated(true)
    this.startHeartbeat()
    return true
  }

  /** One command whose REQUEST has a body: an envelope chunk (not FINAL), then
   *  `body` streamed on the same session in window-sized pieces, then one reply.
   *
   *  The generic form of what `uploadPartition` does by hand, kept generic because
   *  `partition write` is not the only command shaped like this. What is NOT here is
   *  the erase-write-activate sequence: that is partition policy and belongs to
   *  whoever knows about partitions, which is the page above, not this transport.
   *
   *  Runs through the open queue, so nothing else touches the socket mid-upload — the
   *  device would REJECT an interleaved session id. */
  async uploadSession<T>(
    type: string,
    params: Record<string, unknown> | undefined,
    body: Blob,
    onProgress?: (fraction: number) => void,
  ): Promise<T> {
    return this.enqueue(async () => {
      await this.ensureConnected()

      const session = this.allocSession()
      const total = body.size

      // Progress is DEVICE-driven: the handler streams {"p":<bytesWritten>} as it
      // works, and those are mapped to a fraction. Client-side "bytes sent" cannot
      // see the device's write position — the OS buffers the socket — so it would
      // race to 1 while the write is still in flight.
      const reply = this.awaitReply<T>(session, {
        timeoutMs: 120000,
        onMessage: (msg: Record<string, unknown>) => {
          if (total && typeof msg.p === "number")
            onProgress?.(Math.min(1, msg.p / total))
        },
      })

      const envelope = new TextEncoder().encode(
        JSON.stringify({ type, ...(params ?? {}) }) + "\n",
      )
      this.sendChunk(session, 0, envelope)

      // CHUNK matches the device's inbound window: a larger frame is refused, not
      // split.
      const CHUNK = 4096
      let sent = 0
      while (sent < total) {
        const end = Math.min(sent + CHUNK, total)
        const slice = new Uint8Array(await body.slice(sent, end).arrayBuffer())
        const isLast = end >= total
        await this.drainBuffer()
        this.sendChunk(session, isLast ? FLAG_FINAL : 0, slice)
        sent = end
      }
      // A zero-length body still needs a FINAL to close the request direction.
      if (total === 0) this.sendChunk(session, FLAG_FINAL, new Uint8Array(0))

      const result = await reply
      onProgress?.(1)
      return result
    })
  }

  /** Upload a .bin as one streamed `writePartition` session: an envelope chunk
   *  ({"type":"writePartition","partition":...}\n) followed by body chunks, the
   *  last carrying FLAG_FINAL. The device drains it straight to flash and replies
   *  once, at end-of-stream. Runs through the open queue, so nothing else touches
   *  the socket mid-upload (the device would REJECT an interleaved session id). */
  async uploadPartition(
    partition: string,
    file: File,
    onProgress?: (percent: number) => void,
  ): Promise<UploadResult> {
    return this.enqueue(async () => {
      await this.ensureConnected()

      // Erase first: `partition write` never erases, and flash bits only clear on
      // erase, so writing over stale content would produce an image that fails
      // validation at activate.
      const cleared = await this.send<{ ok: boolean; error?: string }>("partition clear", { partition })
      if (!cleared.ok) throw new Error(cleared.error ?? "partition clear failed")

      const session = this.allocSession()
      const total = file.size

      // Progress is DEVICE-driven: the handler streams {"p":<bytesWritten>}
      // messages as it flashes, and we map those to a percentage. Client-side
      // "bytes sent" can't see the device's write position (the OS buffers the
      // socket), so it would race to 100% while the write is still in flight.
      const reply = this.awaitReply<{ ok: boolean; size?: number; error?: string }>(session, {
        timeoutMs: 120000,
        onMessage: (msg) => {
          if (total && typeof msg.p === "number") {
            onProgress?.(Math.min(100, Math.round((msg.p / total) * 100)))
          }
        },
      })

      // Envelope chunk (not FINAL — the body follows on the same session id).
      // The route is TWO words: ReadCommandRoute splits "<category> <command>" and
      // rejects a single-word type outright, so "writePartition" never reached the
      // handler — every upload from this page was refused before it started.
      const envelope = new TextEncoder().encode(JSON.stringify({ type: "partition write", partition }) + "\n")
      this.sendChunk(session, 0, envelope)

      // Body chunks. CHUNK matches the device's inbound window (see WebSocketHandler).
      const CHUNK = 4096
      let sent = 0
      while (sent < total) {
        const end = Math.min(sent + CHUNK, total)
        const slice = new Uint8Array(await file.slice(sent, end).arrayBuffer())
        const isLast = end >= total
        await this.drainBuffer()
        this.sendChunk(session, isLast ? FLAG_FINAL : 0, slice)
        sent = end
      }
      // A zero-length file still needs a FINAL to close the request direction.
      if (total === 0) this.sendChunk(session, FLAG_FINAL, new Uint8Array(0))

      const res = await reply
      if (!res.ok) throw new Error(res.error ?? "partition write failed")

      // Validate and switch the boot slot only once every byte landed. Until this
      // point the old slot still boots, so a failed upload leaves the device intact.
      const act = await this.send<{ ok: boolean; error?: string }>("partition activate", { partition })
      if (!act.ok) throw new Error(act.error ?? "partition activate failed")

      onProgress?.(100)
      return { ok: true, size: res.size ?? sent }
    })
  }

  // Backpressure: don't let the browser-side WS buffer outrun the socket.
  private async drainBuffer(limit = 64 * 1024) {
    while (this.ws && this.ws.bufferedAmount > limit) {
      await new Promise((r) => setTimeout(r, 20))
    }
  }

  /** One command whose REPLY is a stream: the envelope goes out as a single FINAL
   *  chunk and the device writes bytes back until it FINALs, with no length header.
   *
   *  The generic form of `downloadPartitionFile`, and the mirror of `uploadSession`.
   *  It stops at the bytes on purpose: saving a file is a host concern — an anchor
   *  click here, something else in another shell — while "give me the bytes" is the
   *  same question everywhere, which is what makes it expressible in the contract.
   *
   *  `total`, when the caller knows it, is used for progress AND for a length check.
   *  The device always streams a whole partition, so a short read means a mid-stream
   *  flash or socket failure produced a truncated image followed by a FINAL — which
   *  would otherwise be saved as a corrupt file that looks fine.
   *
   *  Runs through the open queue, so it owns the socket until it finishes: the device
   *  would REJECT an interleaved session id. */
  async downloadSession(
    type: string,
    params: Record<string, unknown> | undefined,
    total?: number,
    onProgress?: (fraction: number) => void,
  ): Promise<Blob> {
    const buf = await this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<Uint8Array<ArrayBuffer>>(session, {
        timeoutMs: 120000,
        binary: true,
        onData: (received) => {
          if (total) onProgress?.(Math.min(1, received / total))
        },
      })
      const body = new TextEncoder().encode(
        JSON.stringify({ type, ...(params ?? {}) }) + "\n",
      )
      this.sendChunk(session, FLAG_FINAL, body)
      return reply
    })

    // A short reply that parses as a JSON error means the device refused instead of
    // streaming bytes — an unknown partition, say. It arrives as a successful reply,
    // so it has to be read out of the payload or a failure becomes a tiny "image".
    if (buf.length < 256) {
      const text = new TextDecoder().decode(buf)
      if (text.startsWith('{"ok":false'))
        throw new Error(JSON.parse(text).error ?? "download failed")
    }

    if (total && buf.length !== total)
      throw new Error(`incomplete download: got ${buf.length} of ${total} bytes`)

    onProgress?.(1)
    return new Blob([buf])
  }

  /** Download a partition image as one outbound streamed session and save it as
   *  <label>.bin. The device writes the raw partition bytes to the reply stream,
   *  chunked and ended by FINAL (or, on failure, a short JSON error object). The
   *  reply is chunked with no length header, so progress is computed against
   *  expectedSize — the UI knows it from the partition table. Runs through the
   *  open queue, so it owns the socket until it finishes (the device would REJECT
   *  an interleaved session id). */
  async downloadPartitionFile(
    label: string,
    expectedSize?: number,
    onProgress?: (percent: number) => void,
  ): Promise<void> {
    const buf = await this.enqueue(async () => {
      await this.ensureConnected()
      const session = this.allocSession()
      const reply = this.awaitReply<Uint8Array<ArrayBuffer>>(session, {
        timeoutMs: 120000,
        binary: true,
        onData: (received) => {
          if (expectedSize) onProgress?.(Math.min(100, Math.round((received / expectedSize) * 100)))
        },
      })
      // Request = one FINAL chunk: the command envelope, no body.
      const body = new TextEncoder().encode(JSON.stringify({ type: "partition read", partition: label }) + "\n")
      this.sendChunk(session, FLAG_FINAL, body)
      return reply
    })

    // A short reply that is a JSON error object means the device refused the
    // request (e.g. unknown partition) instead of streaming image bytes.
    if (buf.length < 256) {
      const text = new TextDecoder().decode(buf)
      if (text.startsWith('{"ok":false')) throw new Error(JSON.parse(text).error ?? "download failed")
    }
    // The device always streams the whole partition; a short read (a mid-stream
    // flash/socket failure) would leave a truncated image + FINAL, so verify the
    // length rather than silently saving a corrupt file.
    if (expectedSize && buf.length !== expectedSize) {
      throw new Error(`incomplete download: got ${buf.length} of ${expectedSize} bytes`)
    }

    const url = URL.createObjectURL(new Blob([buf]))
    const a = document.createElement("a")
    a.href = url
    a.download = `${label}.bin`
    a.click()
    URL.revokeObjectURL(url)
    onProgress?.(100)
  }

}

const instance = new BackendService()
instance.connect()
export const backend = instance

// ── Types ────────────────────────────────────────────────────────

export interface DeviceInfo {
  name: string
  project: string
  firmware: string
  idf: string
  date: string
  time: string
  chip: string
  cpu: string
  ip: string
  heapFree: number
  heapMin: number
  deviceTime: string
}

// ── The LED, this template's worked example ──────────────────────────────────
//
// A product built from Strux deletes this pair along with LedManager and puts its
// own feature's commands here — the demo is the only thing in this file that is not
// framework.

/** One captured frame of the device's panel. `pixels` is RGB565, little-endian, and
 *  `stride` is its row pitch in BYTES — which is not always width*2, because the
 *  device may send a decimated frame and LVGL is free to pad a row. */
export interface PanelFrame {
  screen: string
  width: number
  height: number
  stride: number
  scale: number
  pixels: Uint8Array
}

export interface PanelTouchResult {
  ok: boolean
  error?: string
  x?: number
  y?: number
  action?: string
  screen?: string
}

export interface LedState {
  /** Whether the LED is showing the relay link at all. */
  enabled: boolean
  /** The relay pipe's state — what the LED is indicating. */
  connected: boolean
  /** The pin, which is `enabled && connected`. */
  on: boolean
}

export interface UpdateStatus {
  firmware: string
  running: string
  nextSlot: string
}

export interface UploadResult {
  ok: boolean
  size: number
}

export type SettingType = "string" | "int32" | "uint32" | "float" | "bool"

export const NUMERIC_SETTING_TYPES: SettingType[] = ["int32", "uint32", "float"]

export interface SettingEntry {
  key: string
  label: string
  type: SettingType
  value: string | number | boolean
}

export interface SettingsResponse {
  settings: SettingEntry[]
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  channel: number
  secure: boolean
}

export interface WifiScanResponse {
  ok: boolean
  networks: WifiNetwork[]
}

export interface LogsResponse {
  lines: string[]
}

export interface Partition {
  label: string
  type: string
  subtype: string
  offset: number
  size: number
  running: boolean
  nextOta: boolean
  uploadable: boolean
  version: string
}

export interface PartitionsResponse {
  partitions: Partition[]
}

