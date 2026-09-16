import { useEffect } from "react"
import { LightbulbIcon, TerminalIcon, SettingsIcon, DownloadIcon, SmartphoneIcon } from "lucide-react"
import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
} from "@/components/ui/sidebar"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { useDeviceInfo } from "@/hooks/use-device-info"
import { useLatestRelease } from "@/hooks/use-latest-release"
import { isNewerVersion } from "@/lib/version"
import { PreReleaseBadge } from "@/components/PreReleaseBadge"
import { DeviceInfoDialog } from "@/components/DeviceInfoDialog"

// The nav, and the first entry is the product. "home" is a fixed id rather than the
// feature's name so that a fork replacing this template's demo feature replaces the
// CONTENTS of HomePage.tsx and the title and icon on this line — a bookmark to "/"
// still lands on whatever that product's own screen is. LED is what Strux ships as
// its worked example; a real product's first entry is its own thing.
//
// There is no "Device" entry: a chip name and a heap figure are reference material
// you go looking for, not a destination, so they live behind the footer instead of
// taking a place in the navigation beside the product. See
// docs/reasoning/2026-09-09-22h00.
const navItems = [
  { title: "LED", icon: LightbulbIcon, page: "home" as const },
  { title: "Panel", icon: SmartphoneIcon, page: "panel" as const },
  { title: "Console", icon: TerminalIcon, page: "console" as const },
  { title: "Settings", icon: SettingsIcon, page: "settings" as const },
  { title: "Firmware", icon: DownloadIcon, page: "firmware" as const },
]

export type Page = (typeof navItems)[number]["page"]

interface AppSidebarProps {
  currentPage: Page
  onNavigate: (page: Page) => void
}

const statusColor = {
  connected: "bg-emerald-500",
  connecting: "bg-amber-500 animate-pulse",
  disconnected: "bg-red-500",
} as const

const statusLabel = {
  connected: "Online",
  connecting: "Connecting",
  disconnected: "Offline",
} as const

export function AppSidebar({ currentPage, onNavigate }: AppSidebarProps) {
  const connection = useConnectionStatus()
  const info = useDeviceInfo()
  const release = useLatestRelease()
  const updateAvailable = info && release && isNewerVersion(info.firmware, release.version)

  // Browser tab title follows the device name (login page covers pre-auth).
  useEffect(() => {
    if (info?.name) document.title = info.name
  }, [info?.name])

  return (
    <Sidebar>
      <SidebarHeader className="px-4 py-3">
        <div className="flex items-center gap-2">
          <span className="text-sm font-semibold">{info?.name ?? "…"}</span>
          <PreReleaseBadge version={info?.firmware} />
        </div>
      </SidebarHeader>
      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupContent>
            <SidebarMenu>
              {navItems.map((item) => (
                <SidebarMenuItem key={item.page}>
                  <SidebarMenuButton
                    isActive={currentPage === item.page}
                    onClick={() => onNavigate(item.page)}
                  >
                    <item.icon />
                    <span>{item.title}</span>
                    {item.page === "firmware" && updateAvailable && (
                      <span className="ml-auto h-2 w-2 rounded-full bg-emerald-500" />
                    )}
                  </SidebarMenuButton>
                </SidebarMenuItem>
              ))}
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>
      </SidebarContent>
      {/* The footer is the way in to the device's details. It already shows the
          version and the link state, so it is where somebody looks when they want to
          know more about either. A button, not a div with an onClick: keyboard focus
          and Enter come for free, and a dialog reached only by mouse is a dialog some
          people cannot reach. */}
      <SidebarFooter className="p-3">
        <DeviceInfoDialog>
          <button
            type="button"
            aria-label="Device info"
            className="w-full cursor-pointer rounded-lg border bg-card p-3 text-left text-xs transition-colors hover:bg-muted focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
          >
            {info && (
              <div className="mb-1.5 flex items-center justify-between">
                <span className="text-muted-foreground">Version</span>
                <div className="flex items-center gap-1.5">
                  <PreReleaseBadge version={info.firmware} />
                  <span className="font-mono">{info.firmware}</span>
                </div>
              </div>
            )}
            <div className="flex items-center justify-between">
              <span className="text-muted-foreground">Status</span>
              <div className="flex items-center gap-1.5">
                <span className={`h-2 w-2 rounded-full ${statusColor[connection]}`} />
                <span>{statusLabel[connection]}</span>
              </div>
            </div>
          </button>
        </DeviceInfoDialog>
      </SidebarFooter>
    </Sidebar>
  )
}
