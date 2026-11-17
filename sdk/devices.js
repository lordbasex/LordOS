// devices.js — the batteries of everything connected, as a React hook (docs/DEVICES.md): the Devices widget, the bar's
// battery panel and Monitor → Devices show the same list (the pure part is devicesmodel.js).
import { useEffect, useState } from 'react';
import { useStore } from './store.js';
import { battery, bluetooth } from './hardware.jsx';
import { useFeed } from './widgetfeed.js';
import { t } from './i18n.js';
import { devicesFrom, shortName } from './devicesmodel.js';

export const hidppRead = () => { try { return lord.hidpp?.batteries ? lord.hidpp.batteries() || [] : []; } catch (e) { return []; } };

// No timer of its own for the kernel's and BlueZ's batteries: lord.power and lord.bt send their changes. lord-hidpp's
// Logitech percentages come through the shared feed once a minute while something shows them, and again a few
// seconds after a power_supply change (a keyboard waking up: an MX Keys answers only while awake).
export function useDevices() {
  const pw = useStore(battery), bt = useStore(bluetooth);
  const hp = useFeed('hidpp', hidppRead, 60000);
  const [fresh, setFresh] = useState(null);
  useEffect(() => {
    hidppRead();
    const id = setTimeout(() => setFresh(hidppRead()), 4000);
    return () => clearTimeout(id);
  }, [pw]);
  return devicesFrom(pw, bt, fresh && fresh.length >= (hp || []).length ? fresh : hp);
}

export const deviceName = (d) => (d.kind === 'laptop' && !d.name ? t('shell.widgets.devices.computer') : d.name);
export const deviceShort = (d) => (d.kind === 'laptop' && !d.name ? t('shell.widgets.devices.computer') : shortName(d.name));
// "85 %", "⚡100 %", the level's word, "Charging" with no level, "No data"
export const deviceValue = (d) => `${d.charging ? '⚡' : ''}${d.percent >= 0 ? `${d.percent} %`
  : d.level ? t(`shell.widgets.devices.level.${d.level}`) : d.charging ? t('shell.widgets.devices.chargingNow') : t('shell.widgets.devices.unknown')}`;
// "Logitech · charging"
export const deviceVia = (d) => `${t(`shell.widgets.devices.via.${d.via}`)}${d.charging ? ` · ${t('shell.widgets.devices.charging')}` : ''}`;
