// LordOS Studio — the camera's effects and Mic Mode in a window of their own (docs/CAMERA-STUDIO-PLAN.md 9.2, its first
// step: the popover's controls; scenes, overlays and recording come later), the
// same controls as the capture item's popover: a live preview, the camera, Portrait, Background, Face Light, Auto
// Framing, Filters, Retouch, Mirror, Mic Mode and Every app (the "LordOS Studio" camera for browsers and other apps,
// with the way to the Store's LordOS Studio Camera when it isn't installed). Everything lives in the SDK
// (sdk/studio.jsx StudioWindow; settings.studio), so the window, the popover and every app that chose the "LordOS
// Studio" camera agree. It runs in the desktop's own process like Settings and uses the desktop-only lord.studio; it
// is listed only where LordOS Studio can work (available below).
import { View, useTheme, StudioWindow, studioAvailable, SkinHeader, appName, getApp } from '@lordos/sdk';

// the launcher and search list it only where lord-studio and its model are installed (sdk/windows.js listApps)
export const available = () => studioAvailable();

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(150deg, #5e5ce6, #1b1f4b)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden', direction: 'ltr' }}>
      {/* a soft studio light behind the lens */}
      <View style={{ position: 'absolute', left: s * 0.1, top: s * 0.1, width: s * 0.8, height: s * 0.8, borderRadius: s * 0.4, backgroundColor: 'rgba(255,214,140,0.16)' }} />
      <View style={{ position: 'absolute', left: s * 0.16, top: s * 0.16, width: s * 0.68, height: s * 0.68, borderRadius: s * 0.34, backgroundColor: 'rgba(255,214,140,0.2)' }} />
      {/* the lens: rings and a glint */}
      <View style={{ position: 'absolute', left: s * 0.22, top: s * 0.22, width: s * 0.56, height: s * 0.56, borderRadius: s * 0.28, backgroundColor: '#e9ebf2' }} />
      <View style={{ position: 'absolute', left: s * 0.29, top: s * 0.29, width: s * 0.42, height: s * 0.42, borderRadius: s * 0.21, backgroundColor: '#20232c' }} />
      <View style={{ position: 'absolute', left: s * 0.35, top: s * 0.35, width: s * 0.3, height: s * 0.3, borderRadius: s * 0.15,
        background: 'linear-gradient(135deg, #64d2ff, #5e5ce6 55%, #bf5af2)' }} />
      <View style={{ position: 'absolute', left: s * 0.41, top: s * 0.4, width: s * 0.08, height: s * 0.08, borderRadius: s * 0.04, backgroundColor: 'rgba(255,255,255,0.85)' }} />
      {/* a sparkle: the effects */}
      <View style={{ position: 'absolute', left: s * 0.7, top: s * 0.13, width: s * 0.13, height: s * 0.13, borderRadius: s * 0.02, transform: 'rotate(45deg)', backgroundColor: '#ffd60a' }} />
      <View style={{ position: 'absolute', left: s * 0.735, top: s * 0.165, width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: '#fff6cf' }} />
    </View>
  );
}

// (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → the shared shapes"): a header a theme may add above the app
export default function Studio() { return <SkinHeader app="studio" title={appName(getApp('studio'))}><StudioMain /></SkinHeader>; }

function StudioMain() {
  const th = useTheme();
  return <StudioWindow th={th} />;
}
