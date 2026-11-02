// Sandbox Probe — System Information's test app (docs/BENCHMARK.md, "Checks"). It runs as an isolated
// process (`lord-server --app sysprobe`), where app mode tries what its sandbox must refuse
// (server/src/app/probe.c) and reports to lord-server before any of this code would load. This
// component only shows if the app couldn't start isolated.
import { View, Text, useTheme, useLocale, typeScale, spacing } from '@lordos/sdk';
import { Icon as SysInfoIcon } from '../sysinfo/index.jsx';

export function Icon({ size }) { return <SysInfoIcon size={size} />; }

export default function SandboxProbe() {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flex: 1, padding: spacing.xl, justifyContent: 'center' }}>
      <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'center', lineHeight: 1.4 }}>{tr('sysinfo.probe.inProcess')}</Text>
    </View>
  );
}
