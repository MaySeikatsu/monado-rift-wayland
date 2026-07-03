# Wayland compositors: Hyprland, Niri and friends

Monado drives the CV1's display through **DRM leasing**: the compositor
hands the headset's output (marked `non-desktop` by the kernel) to Monado
via the `wp-drm-lease-device-v1` Wayland protocol, and Monado scans it out
directly at 90 Hz — no window, no compositor latency.

The superbuild always enables Monado's `wayland-direct` backend
(`XRT_HAVE_WAYLAND_DIRECT`), so on a compositor that implements DRM
leasing everything works out of the box: start `monado-service` from a
terminal inside your session and it leases the headset automatically.

## Compositor support

| Compositor | DRM lease | Notes |
| --- | --- | --- |
| **Niri** | ✅ | Implemented in the tty backend; recent releases work out of the box. |
| **Hyprland** | ✅ | Supported in recent versions (Aquamarine backend). See quirks below. |
| KDE Plasma (kwin_wayland) | ✅ | Long-standing support. |
| Sway / wlroots | ✅ | wlroots implements the protocol. |
| GNOME (mutter) | 🔶 | Lease support landed recently; older versions need X11 fallback. |
| X11 session | ✅ | Monado falls back to RandR non-desktop output leasing. |

### Hyprland quirks

- Make sure the headset output is *not* being configured as a desktop
  monitor. It should appear as non-desktop and be left alone. If Hyprland
  grabs it anyway (older versions did), add an explicit ignore rule to
  `hyprland.conf` — the CV1 output usually shows up as `HDMI-A-n`:

  ```
  monitor = HDMI-A-1, disable
  ```

  (Only do this if leasing fails; on fixed versions the `non-desktop`
  property is respected automatically.)

- If leasing still fails, check `hyprctl monitors all` to confirm the
  headset's connector, and run with `XRT_COMPOSITOR_LOG=debug
  monado-service` to see what the lease negotiation reports.

### Niri

Niri implements the DRM lease protocol and ignores non-desktop outputs by
default — the CV1 should "just work". If you previously forced the output
on in your niri config, remove that block.

## Useful environment variables

```sh
# Force the Wayland direct backend (skip other probing):
XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1 monado-service

# See modes and lease negotiation:
XRT_COMPOSITOR_LOG=debug monado-service

# Force X11/RandR direct mode instead (X11 sessions / XWayland leasing):
XRT_COMPOSITOR_FORCE_RANDR=1 monado-service
```

## No headset display? (debugging without direct mode)

You can run Monado against a regular desktop window to debug everything
except the display path:

```sh
XRT_COMPOSITOR_FORCE_WAYLAND=1 monado-service   # composited window "HMD"
```

Tracking, controllers and haptics all behave normally in this mode, so
it's handy to verify the driver before wrestling with lease permissions.

## NVIDIA note

**On NVIDIA + Wayland you currently need to force the Wayland backend:**
when `DISPLAY` (Xwayland) is set and the GPU is NVIDIA, Monado prefers its
X11/NVIDIA direct-mode backend, which fails under a Wayland session with
`vkAcquireXlibDisplayEXT: VK_ERROR_UNKNOWN`. Fix:

```sh
XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1 monado-service
```

(Verified working on niri + GTX 1060: the compositor hands over the Rift
connector via DRM lease and the swapchain runs on the headset panel.)
When running via `services.monado` on NixOS, set it on the unit:

```nix
systemd.user.services.monado.environment.XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT = "1";
```

DRM leasing on the proprietary NVIDIA driver historically needed
`nvidia-drm.modeset=1` and a reasonably new driver. If the lease succeeds
but you get long frame times, check Monado's compositor log for direct
mode confirmation — a hidden fallback to a window is the usual culprit.
