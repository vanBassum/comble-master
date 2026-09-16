import { useCallback, useEffect, useRef, useState } from "react"
import { backend, type PanelFrame } from "@/lib/backend"

// How long to wait between frames in live mode. Not a frame rate: a capture takes as
// long as it takes (300 KB over the same socket everything else on this page shares),
// and this is the breathing room after each one so the sidebar's polls and the
// console's broadcasts are not permanently queued behind the view. Raising it costs
// smoothness; lowering it costs everyone else.
const LIVE_GAP_MS = 250

// Always the panel's own resolution, live view included. A half-size frame has to be
// scaled back up to 320 px to sit in the same box, and scaling a 320x480 UI is exactly
// where it falls apart — the hairlines and the stems of 14px type are the first thing
// resampling eats. The device can send a decimated frame (`ui screenshot -scale`) and
// that is still worth having for a script on a bad link, but the page does not want it:
// with the browser transport's 4 KB window a full frame is a few hundred milliseconds,
// which is a live view by any reasonable measure.
const SCALE = 1 as const

// After a press, give LVGL time to act on it before looking again — a screen load, a
// list redraw. Shorter than this and the frame shows the panel mid-thought.
const AFTER_TOUCH_MS = 280

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

/**
 * The panel as something to look at and press: `ui screenshot` in, `ui touch` out.
 *
 * Both halves go through the ordinary command queue, so a capture and a press cannot
 * overlap on the wire — which is also why live mode is a loop that waits for its own
 * reply rather than an interval that would pile requests up behind a slow link.
 */
export function usePanel() {
  const [frame, setFrame] = useState<PanelFrame | null>(null)
  const [live, setLive] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [frameMs, setFrameMs] = useState<number | null>(null)

  // A reply can land after unmount — the transport has no cancellation — so every
  // setState is guarded rather than assumed safe.
  const alive = useRef(true)
  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const capture = useCallback(async (scale: 1 | 2 | 4 = SCALE) => {
    const started = performance.now()
    try {
      const f = await backend.capturePanel(scale)
      if (!alive.current) return
      setFrame(f)
      setFrameMs(Math.round(performance.now() - started))
      setError(null)
    } catch (e) {
      if (!alive.current) return
      // Keep the last frame on screen: a dropped capture is not news, and blanking
      // the panel on every hiccup would read as the device having gone away.
      setError(errorMessage(e))
      throw e
    }
  }, [])

  /// One full-resolution frame, on demand.
  const refresh = useCallback(() => {
    setBusy(true)
    capture(SCALE)
      .catch(() => {})
      .finally(() => {
        if (alive.current) setBusy(false)
      })
  }, [capture])

  /// Press the panel at a point in ITS pixels, then look at what that did.
  const tap = useCallback(
    async (x: number, y: number) => {
      setBusy(true)
      try {
        const res = await backend.touchPanel(x, y)
        if (!alive.current) return
        if (!res.ok) {
          setError(res.error ?? "the device refused the press")
          return
        }
        setError(null)
        // In live mode the loop is already coming round; asking here as well would
        // put two captures on the queue for one press.
        if (!live) {
          await sleep(AFTER_TOUCH_MS)
          await capture(SCALE).catch(() => {})
        }
      } catch (e) {
        if (alive.current) setError(errorMessage(e))
      } finally {
        if (alive.current) setBusy(false)
      }
    },
    [capture, live],
  )

  // The live loop. Each pass waits for its own reply, so a slow link slows the view
  // down instead of flooding the queue. A failed capture backs off rather than
  // spinning: the socket is usually reconnecting, and hammering it does not help.
  useEffect(() => {
    if (!live) return
    let cancelled = false

    const run = async () => {
      while (!cancelled) {
        try {
          await capture(SCALE)
        } catch {
          await sleep(1500)
          continue
        }
        await sleep(LIVE_GAP_MS)
      }
    }
    void run()

    return () => {
      cancelled = true
    }
  }, [live, capture])

  // One frame as soon as the page opens, so it shows the panel rather than an empty
  // box waiting to be told to look.
  useEffect(() => {
    refresh()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  return { frame, live, setLive, busy, error, frameMs, refresh, tap }
}
