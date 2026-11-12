#!/usr/bin/env node
// Private visit statistics for lordos.org, built only from what CloudFront already receives (see
// web/content/legal/cookies-and-statistics.*.md). Nothing is added to the site: no cookie, no
// script, no redirect. CloudFront sends its standard (v2) access logs to a CloudWatch Logs group,
// and three CloudWatch dashboards read them with Logs Insights. Only the team sees them.
//
//   node tools/web/stats.mjs setup      log group (400 days), delivery source, destination, delivery
//   node tools/web/stats.mjs dashboards create or replace the three dashboards
//   node tools/web/stats.mjs test       run every dashboard query over the last hour and report
//   node tools/web/stats.mjs downloads  GitHub's public download counts of the release files (ISO, OVA, Raspberry Pi
//                                       image, Imager, .deb) into the LordOS-Descargas dashboard and the LordOS/Descargas
//                                       metrics; run it once a day. GitHub's numbers only: no CloudFront log is read
//   node tools/web/stats.mjs countries out.csv
//                                       the country lookup table (ISO 3166-1 alpha-2, Spanish name,
//                                       alpha-3, numeric) from Debian's iso-codes, to upload as
//                                       `lordos_paises` (CloudWatch → Settings → Logs → Lookup tables)
//
// The AWS profile, the distribution and the log group come from ~/.aws/.lordos-deploy-env (tools/web/aws-env.sh); the
// profile needs CloudFront log delivery, CloudWatch Logs and dashboard rights. Every step
// is idempotent. Dashboard titles are in Spanish on purpose: they are read by the maintainer only,
// never shipped in LordOS or on the site.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const REGION = 'us-east-1';
// KEY=value lines from the private settings file; the environment wins over the file.
function deployEnv(...keys) {
  const file = process.env.LORDOS_DEPLOY_ENV || path.join(os.homedir(), '.aws', '.lordos-deploy-env');
  const vals = {};
  try {
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
      const m = /^(LORDOS_[A-Z0-9_]+)=(.*)$/.exec(line.trim());
      if (m) vals[m[1]] = m[2];
    }
  } catch { /* missing file: only the environment counts */ }
  return keys.map(k => {
    const v = process.env[k] || vals[k];
    if (!v) { console.error(`${k} is not set: put it in ${file} (see tools/web/aws-env.sh)`); process.exit(1); }
    return v;
  });
}
const [PROFILE, DISTRIBUTION, LOG_GROUP] = deployEnv('LORDOS_AWS_PROFILE', 'LORDOS_CF_DISTRIBUTION', 'LORDOS_STATS_LOG_GROUP');
const RETENTION_DAYS = 400; // 13 months, as the privacy policy says
const SOURCE = 'lordos-org-access';
const DESTINATION = 'lordos-org-cwl';
// Only the fields the privacy policy lists, plus technical data about the response.
const FIELDS = ['timestamp', 'c-ip', 'c-country', 'cs-method', 'cs-uri-stem', 'sc-status', 'cs(Referer)',
  'cs(User-Agent)', 'x-host-header', 'x-edge-location', 'x-edge-result-type', 'time-taken', 'sc-bytes'];

function aws(args, { json = true, quiet = false } = {}) {
  try {
    const out = execFileSync('aws', ['--profile', PROFILE, '--region', REGION, ...args, ...(json ? ['--output', 'json'] : [])],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', quiet ? 'pipe' : 'inherit'] });
    return json && out.trim() ? JSON.parse(out) : out;
  } catch (e) {
    if (quiet) return null;
    throw e;
  }
}

// ---- queries --------------------------------------------------------------------------------

const UA = '`cs(User-Agent)`';
const BOTS = '/(?i)bot|crawl|spider|slurp|curl|wget|python|go-http|headless|lighthouse|preview|monitor|externalhit|ahrefs|semrush|scan|httpclient|java\\/|okhttp|axios|node-fetch|lordos-stats-test/';
// A page view: a successful GET of an HTML page, not the APT repository, not a bot.
const PAGE = '`cs-method` = "GET" and `sc-status` in ["200", "304"] and `cs-uri-stem` like /\\/$|\\.html$/'
  + ' and `cs-uri-stem` not like /^\\/(apt|apt-staging|keys)\\//';
const HUMAN_PAGES = `filter ${PAGE} and ${UA} not like ${BOTS}`;
// Operating system and kind of device from the user agent (Android and iOS before Linux and macOS,
// whose names also appear in their user agents).
const OS = `parse ${UA} /(?<os_raw>Android|iPhone|iPad|CrOS|Windows|Macintosh|Linux)(?!.*(Android|iPhone|iPad))/`
  + ' | fields coalesce(os_raw, "Otro") as os_name'
  + ' | fields if(os_name = "Macintosh", "macOS", if(os_name = "iPhone" or os_name = "iPad", "iOS", if(os_name = "CrOS", "ChromeOS", os_name))) as sistema';
const DEVICE = `parse ${UA} /(?<dev_raw>iPad|Tablet|Mobi)/`
  + ' | fields coalesce(dev_raw, "none") as dev_name'
  + ' | fields if(dev_name = "Mobi", "Celular", if(dev_name = "iPad" or dev_name = "Tablet", "Tableta", "Computadora")) as dispositivo';
const LANG = 'parse `cs-uri-stem` /^\\/(?<lang_raw>es|pt)\\// | fields coalesce(lang_raw, "en") as idioma';
const REFERRER = 'parse `cs(Referer)` /^https?:\\/\\/(www\\.)?(?<ref_host>[^\\/:]+)/'
  + ' | fields coalesce(ref_host, "-") as ref_name'
  + ' | fields if(ref_name = "-", "(directo: sin origen)", if(ref_name = "lordos.org", "(desde el mismo sitio)", ref_name)) as origen';
const OUTSIDE = 'filter origen != "(directo: sin origen)" and origen != "(desde el mismo sitio)"';
// Country name (Spanish), ISO 3166-1 alpha-3 and numeric from the lookup table (see the countries
// command); a code CloudFront gives that the table lacks shows as "(sin dato)".
const LOOKUP_TABLE = 'lordos_paises';
// lookup does not match a field with a hyphen in its name (`c-country`), so the code is copied to
// alfa2, the table's column, first.
const COUNTRY = 'fields `c-country` as alfa2'
  + ` | lookup ${LOOKUP_TABLE} alfa2 OUTPUT nombre, alfa3, numerico`
  + ' | fields coalesce(alfa2, "-") as c_a2, coalesce(nombre, "(sin dato)") as c_name, coalesce(alfa3, "-") as c_a3, coalesce(numerico, "-") as c_num';
const BY_COUNTRY = 'c_name as pais, c_a2 as alfa_2, c_a3 as alfa_3, c_num as numerico_iso';
// Hour of day in Argentina (UTC-3, no daylight saving time).
const HOUR = 'fields (floor(toMillis(@timestamp) / 3600000) + 21) % 24 as hora_ar';

const Q = {
  resumen: `${HUMAN_PAGES} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes, count_distinct(\`c-country\`) as paises`,
  porHora: `${HUMAN_PAGES} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes by bin(1h)`,
  porDia: `${HUMAN_PAGES} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes by bin(1d)`,
  porSemana: `${HUMAN_PAGES} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes by bin(1w)`,
  porMes: `${HUMAN_PAGES} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes by bin(1mo)`,
  paises: `${HUMAN_PAGES} | ${COUNTRY} | stats count(*) as paginas_vistas, count_distinct(\`c-ip\`) as visitantes by ${BY_COUNTRY} | sort visitantes desc | limit 50`,
  // the page views as a pie: one slice per country (its Spanish name), the 20 with the most views
  paisesTorta: `${HUMAN_PAGES} | ${COUNTRY} | stats count(*) as paginas_vistas by c_name | sort paginas_vistas desc | limit 20`,
  sistemas: `${HUMAN_PAGES} | ${OS} | stats count_distinct(\`c-ip\`) as visitantes by sistema | sort visitantes desc`,
  dispositivos: `${HUMAN_PAGES} | ${DEVICE} | stats count_distinct(\`c-ip\`) as visitantes by dispositivo | sort visitantes desc`,
  idiomas: `${HUMAN_PAGES} | ${LANG} | stats count(*) as paginas_vistas by idioma | sort paginas_vistas desc`,
  paginas: `${HUMAN_PAGES} | stats count(*) as vistas, count_distinct(\`c-ip\`) as visitantes by \`cs-uri-stem\` as pagina | sort vistas desc | limit 30`,
  origenes: `${HUMAN_PAGES} | ${REFERRER} | stats count(*) as visitas, count_distinct(\`c-ip\`) as visitantes by origen | sort visitas desc | limit 25`,
  rotos: `filter \`sc-status\` = "404" and ${UA} not like ${BOTS} | stats count(*) as veces by \`cs-uri-stem\` as direccion, \`cs(Referer)\` as desde | sort veces desc | limit 25`,
  bots: `filter ${PAGE} | fields if(${UA} like ${BOTS}, "Bots y programas", "Personas") as quien | stats count(*) as paginas_vistas by quien`,
  instalaciones: 'filter `cs-uri-stem` = "/install.sh" and `sc-status` in ["200", "304"] | stats count(*) as descargas, count_distinct(`c-ip`) as ips by bin(1d)',
  velocidad: `${HUMAN_PAGES} | ${COUNTRY} | stats count(*) as vistas, avg(\`time-taken\`) as promedio_s, pct(\`time-taken\`, 95) as p95_s by ${BY_COUNTRY} | filter vistas >= 5 | sort vistas desc | limit 30`,
  paisIdioma: `${HUMAN_PAGES} | ${COUNTRY} | ${LANG} | stats count_distinct(\`c-ip\`) as visitantes by ${BY_COUNTRY}, idioma | sort visitantes desc | limit 60`,
  paisSistema: `${HUMAN_PAGES} | ${COUNTRY} | ${OS} | stats count_distinct(\`c-ip\`) as visitantes by ${BY_COUNTRY}, sistema | sort visitantes desc | limit 60`,
  paisDispositivo: `${HUMAN_PAGES} | ${COUNTRY} | ${DEVICE} | stats count_distinct(\`c-ip\`) as visitantes by ${BY_COUNTRY}, dispositivo | sort visitantes desc | limit 60`,
  horaPais: `${HUMAN_PAGES} | ${COUNTRY} | ${HOUR} | stats count(*) as paginas_vistas by ${BY_COUNTRY}, hora_ar | sort pais asc, hora_ar asc | limit 500`,
  horas: `${HUMAN_PAGES} | ${HOUR} | stats count(*) as paginas_vistas by hora_ar | sort hora_ar asc`,
  origenPagina: `${HUMAN_PAGES} | ${REFERRER} | ${OUTSIDE} | stats count(*) as visitas by origen, \`cs-uri-stem\` as pagina | sort visitas desc | limit 50`,
  sistemaMes: `${HUMAN_PAGES} | ${OS} | stats count_distinct(\`c-ip\`) as visitantes by sistema, bin(1mo) as mes | sort mes asc`,
  paisesAnio: `${HUMAN_PAGES} | ${COUNTRY} | stats count_distinct(\`c-ip\`) as visitantes by ${BY_COUNTRY} | sort visitantes desc | limit 100`,
};

// ---- dashboards -----------------------------------------------------------------------------

function log(title, query, view, x, y, w, h, extra = {}) {
  return { type: 'log', x, y, width: w, height: h,
    properties: { title, query: `SOURCE '${LOG_GROUP}' | ${query}`, region: REGION, view, stacked: false, ...extra } };
}
function metric(title, metrics, x, y, w, h, extra = {}) {
  return { type: 'metric', x, y, width: w, height: h,
    properties: { title, region: REGION, view: 'timeSeries', stacked: false, period: 3600, stat: 'Sum',
      metrics: metrics.map(([name, opts]) => ['AWS/CloudFront', name, 'Region', 'Global', 'DistributionId', DISTRIBUTION, opts || {}]),
      ...extra } };
}
function text(md, x, y, w, h) { return { type: 'text', x, y, width: w, height: h, properties: { markdown: md } }; }

const NOTE = 'Páginas vistas por personas en lordos.org, a partir de los logs de CloudFront (sin cookies ni scripts). '
  + 'No cuenta el repositorio `apt`, los archivos del sitio (CSS, JS, imágenes) ni los bots. **Visitantes** = IPs distintas. '
  + 'Cada vez que se abre o se cambia el rango, cada gráfico de logs lee los logs de ese rango: rangos largos consumen la capa gratis '
  + '(5 GB por mes); para meses y años usá *LordOS-Historico*.';

const DASHBOARDS = {
  'LordOS-Visitas': { start: '-P7D', widgets: [
    text(`## LordOS: visitas\n${NOTE}`, 0, 0, 24, 2),
    log('Resumen del período', Q.resumen, 'table', 0, 2, 8, 3),
    metric('Pedidos a CloudFront (todo, incluido apt)', [['Requests']], 8, 2, 8, 3),
    metric('Errores (% de pedidos)', [['4xxErrorRate', { stat: 'Average' }], ['5xxErrorRate', { stat: 'Average' }]], 16, 2, 8, 3),
    log('Páginas vistas y visitantes por hora', Q.porHora, 'timeSeries', 0, 5, 24, 6),
    log('Países', Q.paises, 'table', 0, 11, 8, 9),
    log('Países (torta, páginas vistas)', Q.paisesTorta, 'pie', 8, 11, 8, 9),
    log('Sistema operativo (visitantes)', Q.sistemas, 'pie', 16, 11, 8, 9),
    log('Dispositivo (visitantes)', Q.dispositivos, 'pie', 0, 20, 8, 9),
    log('Páginas más vistas', Q.paginas, 'table', 8, 20, 8, 9),
    log('De dónde llegan', Q.origenes, 'table', 16, 20, 8, 9),
    log('Idioma del sitio', Q.idiomas, 'pie', 0, 29, 8, 6),
    log('Personas y bots', Q.bots, 'pie', 8, 29, 8, 6),
    log('Descargas de install.sh por día', Q.instalaciones, 'bar', 16, 29, 8, 6),
    log('Links rotos (404)', Q.rotos, 'table', 0, 35, 12, 8),
    log('Velocidad por país (segundos)', Q.velocidad, 'table', 12, 35, 12, 8),
  ] },
  'LordOS-Cruces': { start: '-P7D', widgets: [
    text(`## LordOS: cruces\n${NOTE}`, 0, 0, 24, 2),
    log('País × idioma', Q.paisIdioma, 'table', 0, 2, 8, 10),
    log('País × sistema', Q.paisSistema, 'table', 8, 2, 8, 10),
    log('País × dispositivo', Q.paisDispositivo, 'table', 16, 2, 8, 10),
    log('Hora del día (Argentina)', Q.horas, 'bar', 0, 12, 12, 7),
    log('Hora del día (Argentina) × país', Q.horaPais, 'table', 12, 12, 12, 7),
    log('Origen × página de entrada', Q.origenPagina, 'table', 0, 19, 24, 9),
  ] },
  'LordOS-Historico': { start: '-P365D', widgets: [
    text('## LordOS: histórico\nAbrilo solo cuando lo necesites: con el rango de un año cada gráfico lee todos los logs guardados (hasta 13 meses).', 0, 0, 24, 2),
    log('Por mes', Q.porMes, 'bar', 0, 2, 12, 7),
    log('Por semana', Q.porSemana, 'timeSeries', 12, 2, 12, 7),
    log('Por día', Q.porDia, 'timeSeries', 0, 9, 24, 6),
    log('Sistema operativo por mes', Q.sistemaMes, 'table', 0, 15, 12, 9),
    log('Países del período', Q.paisesAnio, 'table', 12, 15, 12, 9),
  ] },
};


// ---- downloads (GitHub's public counters; no visitor data) ----------------------------------

const REPO_API = 'https://api.github.com/repos/lordbasex/LordOS/releases?per_page=100';
const KINDS = [['iso', 'ISO amd64', (n) => /-amd64\.iso$/.test(n)], ['isoArm', 'ISO arm64', (n) => /-arm64\.iso$/.test(n)],
  ['ova', 'OVA', (n) => /\.ova$/.test(n)], ['pi', 'Raspberry Pi', (n) => /\.img(\.xz)?$/.test(n)],
  ['imager', 'Imager', (n) => /imager/.test(n) && /\.(deb|exe|dmg|zip)$/.test(n)], ['deb', '.deb de LordOS', (n) => /^lordos_[0-9.]+_(amd64|arm64)\.deb$/.test(n)]];

async function downloads() {
  const releases = [];
  for (let url = REPO_API; url;) {
    const r = await fetch(url, { headers: { accept: 'application/vnd.github+json', 'user-agent': 'lordos-stats' } });
    if (!r.ok) throw new Error(`GitHub: ${r.status}`);
    releases.push(...await r.json());
    const next = /<([^>]+)>;\s*rel="next"/.exec(r.headers.get('link') || '');
    url = next ? next[1] : null;
  }
  const total = Object.fromEntries(KINDS.map(([k]) => [k, 0]));
  const rows = [];
  for (const rel of releases) {
    const per = Object.fromEntries(KINDS.map(([k]) => [k, 0]));
    for (const a of rel.assets || []) for (const [k, , match] of KINDS) if (match(a.name)) { per[k] += a.download_count; total[k] += a.download_count; }
    if (Object.values(per).some(Boolean)) rows.push([rel.tag_name, (rel.published_at || '').slice(0, 10), per]);
  }
  const head = `| Versión | Fecha | ${KINDS.map(([, l]) => l).join(' | ')} |\n|---|---|${KINDS.map(() => '---:').join('|')}|\n`;
  const line = (a, b, per) => `| ${a} | ${b} | ${KINDS.map(([k]) => per[k].toLocaleString('es-AR')).join(' | ')} |\n`;
  const when = new Date().toISOString().slice(0, 16).replace('T', ' ');
  const md = `## LordOS: descargas\nContadores públicos de GitHub de los archivos de cada release. `
    + `Actualizado ${when} UTC con \`node tools/web/stats.mjs downloads\`.\n\n`
    + `**Son números aproximados: una descarga no es una persona ni una instalación.** GitHub suma 1 cada vez que alguien `
    + `empieza a bajar un archivo, y varias cosas inflan el número:\n`
    + `1. **Descargas cortadas y reintentos:** la ISO pesa varios GB; si se corta y se vuelve a empezar, cuenta otra vez.\n`
    + `2. **Gestores de descarga y navegadores:** algunos piden el archivo en partes o más de una vez.\n`
    + `3. **El LordOS Imager e install.sh:** bajan los archivos de GitHub; cada uso cuenta, aunque sea la misma persona probando de nuevo.\n`
    + `4. **Bots y espejos:** sitios de software, robots que indexan releases y escaneos de seguridad también bajan archivos, `
    + `y GitHub no separa personas de robots en este contador.\n`
    + `5. **Gente que la baja y nunca la instala.**\n\n`
    + `Puede haber más descargas que visitantes de lordos.org: mucha gente baja directo desde GitHub (enlaces en foros, Reddit, `
    + `notas de prensa) sin pasar por el sitio, y las estadísticas del sitio empezaron después de publicada la ISO 0.8.8 (3 de noviembre). `
    + `Tomalos como **interés**, no como usuarios. Las actualizaciones por APT no se cuentan aquí.\n\n`
    + head + line('**Total**', '', total) + rows.map(([t, d, p]) => line(t, d, p)).join('');
  // the totals also go to metrics, so the dashboard can draw how they grow day by day
  const data = KINDS.map(([k, label]) => ({ MetricName: 'Descargas', Dimensions: [{ Name: 'Archivo', Value: label }], Value: total[k], Unit: 'Count' }));
  // (needs cloudwatch:PutMetricData on the deploy profile; without it the dashboard has the table only)
  const pushed = aws(['cloudwatch', 'put-metric-data', '--namespace', 'LordOS/Descargas', '--metric-data', JSON.stringify(data)],
    { json: false, quiet: true }) !== null;
  if (!pushed) console.log('no cloudwatch:PutMetricData for this profile: the table only, no history graph');
  const widgets = [text(md, 0, 0, 24, 9 + Math.ceil(rows.length * 0.6))];
  if (pushed) widgets.push(
    { type: 'metric', x: 0, y: 20, width: 24, height: 7, properties: { title: 'Descargas acumuladas por tipo de archivo', region: REGION,
      view: 'timeSeries', stacked: false, period: 86400, stat: 'Maximum',
      metrics: KINDS.map(([, label]) => ['LordOS/Descargas', 'Descargas', 'Archivo', label]) } });
  aws(['cloudwatch', 'put-dashboard', '--dashboard-name', 'LordOS-Descargas', '--dashboard-body', JSON.stringify({ start: '-P90D', widgets })]);
  console.log('LordOS-Descargas:', KINDS.map(([k, l]) => `${l} ${total[k]}`).join(', '));
  console.log(`  https://${REGION}.console.aws.amazon.com/cloudwatch/home?region=${REGION}#dashboards/dashboard/LordOS-Descargas`);
}

// ---- commands -------------------------------------------------------------------------------

function setup() {
  // The account id is read from the credentials, never written in the repository.
  const ACCOUNT = aws(['sts', 'get-caller-identity']).Account;
  const groups = aws(['logs', 'describe-log-groups', '--log-group-name-prefix', LOG_GROUP]).logGroups;
  if (!groups.some(g => g.logGroupName === LOG_GROUP)) aws(['logs', 'create-log-group', '--log-group-name', LOG_GROUP], { json: false });
  aws(['logs', 'put-retention-policy', '--log-group-name', LOG_GROUP, '--retention-in-days', String(RETENTION_DAYS)], { json: false });
  aws(['logs', 'put-delivery-source', '--name', SOURCE, '--log-type', 'ACCESS_LOGS',
    '--resource-arn', `arn:aws:cloudfront::${ACCOUNT}:distribution/${DISTRIBUTION}`]);
  const dest = aws(['logs', 'put-delivery-destination', '--name', DESTINATION, '--output-format', 'json',
    '--delivery-destination-configuration', `destinationResourceArn=arn:aws:logs:${REGION}:${ACCOUNT}:log-group:${LOG_GROUP}`]).deliveryDestination.arn;
  const existing = aws(['logs', 'describe-deliveries']).deliveries.find(d => d.deliverySourceName === SOURCE && d.deliveryDestinationArn === dest);
  if (existing) aws(['logs', 'update-delivery-configuration', '--id', existing.id, '--record-fields', ...FIELDS], { json: false });
  else aws(['logs', 'create-delivery', '--delivery-source-name', SOURCE, '--delivery-destination-arn', dest, '--record-fields', ...FIELDS]);
  console.log(`logs: ${LOG_GROUP}, ${RETENTION_DAYS} days, ${FIELDS.length} fields`);
}

function dashboards() {
  for (const [name, d] of Object.entries(DASHBOARDS)) {
    const body = JSON.stringify({ start: d.start, periodOverride: 'inherit', widgets: d.widgets });
    const r = aws(['cloudwatch', 'put-dashboard', '--dashboard-name', name, '--dashboard-body', body]);
    const problems = r.DashboardValidationMessages || [];
    console.log(`${name}: ${d.widgets.length} widgets${problems.length ? ', ' + JSON.stringify(problems) : ''}`);
    console.log(`  https://${REGION}.console.aws.amazon.com/cloudwatch/home?region=${REGION}#dashboards/dashboard/${name}`);
  }
}

function countries(out) {
  if (!out) { console.error('usage: node tools/web/stats.mjs countries out.csv'); process.exit(2); }
  const iso = JSON.parse(fs.readFileSync('/usr/share/iso-codes/json/iso_3166-1.json', 'utf8'))['3166-1'];
  // Spanish names from iso-codes' own gettext catalog (.mo: a header, then two tables of length+offset).
  const mo = fs.readFileSync('/usr/share/locale/es/LC_MESSAGES/iso_3166-1.mo');
  const u32 = mo.readUInt32LE(0) === 0x950412de ? (o) => mo.readUInt32LE(o) : (o) => mo.readUInt32BE(o);
  const es = new Map();
  for (let i = 0, n = u32(8), orig = u32(12), tran = u32(16); i < n; i++) {
    const str = (t) => mo.toString('utf8', u32(t + i * 8 + 4), u32(t + i * 8 + 4) + u32(t + i * 8));
    es.set(str(orig), str(tran));
  }
  // CloudWatch lookup tables take no quoted CSV fields, so names with a comma get their usual form.
  const fixes = {
    'Costa de Marfíl': 'Costa de Marfil',
    'Bolivia, Estado plurinacional de': 'Bolivia', 'Congo, República Democrática del': 'República Democrática del Congo',
    'Micronesia, Estados Federados de': 'Micronesia', 'Irán, República islámica de': 'Irán', 'Comores, Islas': 'Comoras',
    'Corea, República Democrática Popular de': 'Corea del Norte', 'Corea, República de': 'Corea del Sur',
    'Moldavia, República de': 'Moldavia', 'Palestina, Estado de': 'Palestina',
    'Santa Elena, Ascensión y Tristán de Acuña': 'Santa Elena Ascensión y Tristán de Acuña', 'Taiwán, Provincia de China': 'Taiwán',
    'Tanzania, República unida de': 'Tanzania', 'Venezuela, República Bolivariana de': 'Venezuela',
    'Islas Vírgenes, Británicas': 'Islas Vírgenes Británicas', 'Islas Vírgenes, de EEUU': 'Islas Vírgenes de EE. UU.',
  };
  const rows = iso.map(c => { const n = es.get(c.name) || c.name; return [c.alpha_2, fixes[n] || n, c.alpha_3, c.numeric]; });
  rows.push(['XK', 'Kosovo', 'XKX', '']); // CloudFront reports Kosovo, which ISO 3166-1 does not list
  rows.sort((a, b) => a[0].localeCompare(b[0]));
  const bad = rows.flat().find(v => /[",\n]/.test(v));
  if (bad) { console.error(`a value CloudWatch can't read (comma, quote or newline): ${bad}`); process.exit(1); }
  fs.writeFileSync(out, ['alfa2,nombre,alfa3,numerico', ...rows.map(r => r.join(','))].join('\n') + '\n');
  console.log(`${out}: ${rows.length} countries; upload it as the lookup table ${LOOKUP_TABLE}`);
}

function sleep(ms) { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms); }

function test() {
  const end = Math.floor(Date.now() / 1000), start = end - 3600;
  let failed = 0, bytes = 0;
  for (const [name, q] of Object.entries(Q)) {
    let id;
    try {
      id = JSON.parse(execFileSync('aws', ['--profile', PROFILE, '--region', REGION, 'logs', 'start-query', '--log-group-name', LOG_GROUP,
        '--start-time', String(start), '--end-time', String(end), '--query-string', q, '--output', 'json'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] })).queryId;
    } catch (e) {
      console.log(`FAIL ${name}: rejected: ${String(e.stderr).trim().split('\n').pop()}`); failed++; continue;
    }
    let r;
    for (let i = 0; i < 60; i++) {
      r = aws(['logs', 'get-query-results', '--query-id', id]);
      if (['Complete', 'Failed', 'Cancelled', 'Timeout'].includes(r.status)) break;
      sleep(1000);
    }
    bytes += r.statistics?.bytesScanned || 0;
    const ok = r.status === 'Complete';
    if (!ok) failed++;
    const first = r.results[0]?.filter(c => c.field !== '@ptr').map(c => `${c.field}=${c.value}`).join(' ') || '(no rows)';
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${r.results.length} rows; ${first}`);
  }
  console.log(`${Object.keys(Q).length - failed}/${Object.keys(Q).length} queries ok, ${(bytes / 1e6).toFixed(1)} MB scanned`);
  process.exit(failed ? 1 : 0);
}

const cmd = process.argv[2];
if (cmd === 'setup') setup();
else if (cmd === 'dashboards') dashboards();
else if (cmd === 'test') test();
else if (cmd === 'downloads') await downloads();
else if (cmd === 'countries') countries(process.argv[3]);
else { console.error('usage: node tools/web/stats.mjs setup|dashboards|test|countries'); process.exit(2); }
