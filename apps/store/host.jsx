// Host package management stays with the distribution; native .lpk apps stay here.
import { View, Text, Button, useTheme, useLocale, useLauncherApps, launchApp, spacing, typeScale } from '@lordos/sdk';

export function HostPackages() {
  const th = useTheme();
  const { t } = useLocale();
  const apps = useLauncherApps();
  const manager = apps.find((a) => a.kind === 'linux' && /^(org\.gnome\.Software|org\.kde\.discover|org\.kde\.plasma-discover|synaptic)$/.test(a.desktopId));
  return (
    <View style={{ gap: spacing.l }}>
      <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: th.text }}>{t('store.host.title')}</Text>
      <Text style={{ fontSize: typeScale.body, lineHeight: 1.5, color: th.textDim }}>{t('store.host.description')}</Text>
      {manager && <Button primary label={t('store.host.open')} onClick={() => launchApp(manager.id)} />}
    </View>
  );
}

export const useDebian = () => ({ upgrades: [], job: null, msg: null });
export const debian = {
  Discover: HostPackages, Updates: HostPackages, Status: () => null,
  canRemove: () => false,
};
