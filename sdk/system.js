// system.js — system and server information, as React hooks.
import { useEffect, useState } from 'react';
import { formatNumber, t } from './i18n.js';

export const systemInfo = () => lord.system.info();

// LordOS's own version, from the build (package.json "version" and "lordos": {"channel"}, see
// server/Makefile): { version: '0.8.4', channel: 'Beta' (empty for a stable release), build }.
export function lordosVersion(info) {
  const i = info || systemInfo();
  const id = String(i.channel || '');
  const channel = id === 'beta' ? t('about.channelBeta') : id === 'alpha' ? t('about.channelAlpha')
    : id === 'rc' ? t('about.channelRc') : id === 'dev' ? t('about.channelDev') : id ? id[0].toUpperCase() + id.slice(1) : '';
  return { version: String(i.version || lord.version || ''), channel, build: String(i.build || '') };
}

function usePoll(read, intervalMs) {
  const [value, setValue] = useState(read);
  useEffect(() => {
    const id = setInterval(() => setValue(read()), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return value;
}

export const useSystemInfo = (intervalMs = 5000) => usePoll(systemInfo, intervalMs);
// System Information's report (lord.sysinfo, server/src/sysreport; docs/BENCHMARK.md): the hardware,
// what the kernel made of it and how this session composes the screen. null without the API.
export function systemReport() {
  try { return typeof lord !== 'undefined' && lord.sysinfo ? lord.sysinfo.report() : null; } catch (e) { return null; }
}
export const useServerStats = (intervalMs = 2000) => usePoll(() => lord.stats(), intervalMs);

// CPU usage (0..1) computed between reads of /proc/stat.
export function useCpuUsage(intervalMs = 1500) {
  const [usage, setUsage] = useState(0);
  useEffect(() => {
    let [t0, i0] = lord.system.cpuTimes();
    const id = setInterval(() => {
      const [t1, i1] = lord.system.cpuTimes();
      const dt = t1 - t0;
      setUsage(dt > 0 ? Math.max(0, Math.min(1, 1 - (i1 - i0) / dt)) : 0);
      t0 = t1; i0 = i1;
    }, intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return usage;
}

export function formatBytes(n) {
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n < 10 && i ? formatNumber(n, 1) : Math.round(n)} ${u[i]}`;
}

export function formatDuration(s) {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min`;
}
