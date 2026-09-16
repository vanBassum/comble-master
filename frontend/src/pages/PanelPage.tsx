import { useCallback, useEffect, useRef } from "react"
import { SmartphoneIcon, RefreshCwIcon } from "lucide-react"

import { Button } from "@/components/ui/button"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { Switch } from "@/components/ui/switch"
import { Label } from "@/components/ui/label"
import { usePanel } from "@/hooks/use-panel"
import type { PanelFrame } from "@/lib/backend"

/**
 * The device's own touchscreen, in the browser — and pressable.
 *
 * Two commands do all of it. `ui screenshot` renders the active screen into a buffer
 * and streams it back as raw RGB565, which the canvas below expands to RGBA; `ui
 * touch` moves a second LVGL pointer, so a click here reaches a widget down the same
 * path a finger on the glass does. Nothing in the panel's UI knows the difference,
 * which is why no screen had to be written twice.
 *
 * On a board with no panel (the C3 SuperMini) the commands are not registered and the
 * capture fails saying so, which is what this page then shows.
 */
export default function PanelPage() {
  const { frame, live, setLive, busy, error, frameMs, refresh, tap } = usePanel()
  const canvasRef = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = canvasRef.current
    if (canvas && frame) drawFrame(canvas, frame)
  }, [frame])

  // A click anywhere on the canvas is a press at the matching point on the glass. The
  // canvas is displayed at whatever width the column allows, so the mapping goes
  // through the element's own rect rather than assuming it is 1:1 — and lands in the
  // PANEL's pixels, which is frame size times the scale it was sent at.
  const onCanvasClick = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      if (!frame) return
      const rect = e.currentTarget.getBoundingClientRect()
      const fx = (e.clientX - rect.left) / rect.width
      const fy = (e.clientY - rect.top) / rect.height
      void tap(fx * frame.width * frame.scale, fy * frame.height * frame.scale)
    },
    [frame, tap],
  )

  const nativeW = frame ? frame.width * frame.scale : 320
  const nativeH = frame ? frame.height * frame.scale : 480

  return (
    <div className="mx-auto max-w-2xl space-y-6">
      <div className="flex items-center gap-3">
        <SmartphoneIcon className="size-6" />
        <h1 className="text-2xl font-bold">Panel</h1>
      </div>

      <Card>
        <CardContent className="flex flex-col items-center gap-3">
          {/* The frame is the device: black behind the glass, and nothing else on the
              page borrowing that radius.

              The canvas is never shown SMALLER than the frame it holds. A 320-wide
              canvas in 240 CSS px is a downscale, and a downscale is where a panel UI
              dies: hairlines, 1px card borders and the stems of 14px type are exactly
              what nearest-neighbour throws away, which made a capture look worse than
              the glass it came from. Native width, and smooth resampling at the phone
              widths that cannot give it that. */}
          <div className="bg-background rounded-xl border p-1.5 shadow-sm">
            <canvas
              ref={canvasRef}
              width={frame?.width ?? 320}
              height={frame?.height ?? 480}
              onClick={onCanvasClick}
              tabIndex={0}
              role="img"
              aria-label={
                frame
                  ? `The device panel, showing the ${frame.screen} screen. Click to press it.`
                  : "The device panel"
              }
              className="ring-offset-background focus-visible:ring-ring block h-auto w-full max-w-[320px] cursor-pointer rounded-lg bg-black focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:outline-none"
              style={{
                aspectRatio: `${nativeW} / ${nativeH}`,
                imageRendering: "auto",
              }}
            />
          </div>

          <p className="text-muted-foreground text-center text-sm">
            Click the screen to press it, exactly where you clicked.
          </p>

          {error && (
            <p className="text-destructive text-center text-sm">
              {error}. Showing the last frame that arrived.
            </p>
          )}
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>View</CardTitle>
        </CardHeader>
        <CardContent className="space-y-5">
          <div className="flex items-start justify-between gap-4">
            <div>
              <Label htmlFor="panel-live" className="text-sm font-medium">
                Live
              </Label>
              <p className="text-muted-foreground text-sm">
                Keep fetching frames at quarter size. It is the device&rsquo;s only
                radio, so everything else on this page gets slower while it runs.
              </p>
            </div>
            <Switch
              id="panel-live"
              checked={live}
              onCheckedChange={setLive}
              className="shrink-0"
            />
          </div>

          <dl className="grid grid-cols-2 gap-x-8 gap-y-3 text-sm">
            <Field label="Screen" value={frame?.screen ?? "…"} />
            <Field label="Panel" value={`${nativeW} × ${nativeH}`} />
            <Field
              label="Sent"
              value={
                frame
                  ? `${frame.width} × ${frame.height}${frame.scale > 1 ? ` (1/${frame.scale})` : ""}`
                  : "…"
              }
            />
            <Field label="Last frame" value={frameMs !== null ? `${frameMs} ms` : "…"} />
          </dl>

          <Button variant="outline" onClick={refresh} disabled={busy || live}>
            <RefreshCwIcon className={busy && !live ? "animate-spin" : undefined} />
            Full resolution
          </Button>
        </CardContent>
      </Card>
    </div>
  )
}

function Field({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <dt className="text-muted-foreground text-xs">{label}</dt>
      <dd className="font-medium tabular-nums">{value}</dd>
    </div>
  )
}

/** Expand the panel's own RGB565 into the RGBA a canvas wants. The low bits are
 *  replicated into the gap rather than zero-filled, which is what the panel's own
 *  driver does — otherwise white comes out at 0xF8,0xFC,0xF8 and the whole frame
 *  reads slightly dim. `stride` is honoured because it is not always width*2. */
function drawFrame(canvas: HTMLCanvasElement, frame: PanelFrame) {
  const ctx = canvas.getContext("2d")
  if (!ctx) return

  const { width: w, height: h, stride, pixels } = frame
  const img = ctx.createImageData(w, h)
  const out = img.data

  for (let y = 0; y < h; y++) {
    let src = y * stride
    let dst = y * w * 4
    for (let x = 0; x < w; x++, src += 2, dst += 4) {
      const v = pixels[src] | (pixels[src + 1] << 8)
      const r = (v >> 11) & 0x1f
      const g = (v >> 5) & 0x3f
      const b = v & 0x1f
      out[dst] = (r << 3) | (r >> 2)
      out[dst + 1] = (g << 2) | (g >> 4)
      out[dst + 2] = (b << 3) | (b >> 2)
      out[dst + 3] = 255
    }
  }
  ctx.putImageData(img, 0, 0)
}
