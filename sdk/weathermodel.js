// weathermodel.js — the weather widget's plain logic (0.8.8.10; server/tests/weather_model_test.mjs). The forecast comes
// from lord.weather (server/src/fetch/wxsvc.h: Open-Meteo through lord-fetch, by the time zone's coordinates), in °C
// with WMO weather codes.
//
//   kindOf(code)          'clear' | 'partly' | 'cloudy' | 'fog' | 'drizzle' | 'rain' | 'snow' | 'showers' | 'storm'
//   unitFor(country)      'f' where Fahrenheit is the custom (US and a few others), else 'c'
//   temp(c, unit)         a whole number in the unit
//   skyOf(kind, day)      the card's two gradient colors (blue by day, navy at night, grey
//                         when overcast or raining)
export function kindOf(code) {
  const c = Number(code);
  if (c === 0) return 'clear';
  if (c === 1 || c === 2) return 'partly';
  if (c === 3) return 'cloudy';
  if (c === 45 || c === 48) return 'fog';
  if (c >= 51 && c <= 57) return 'drizzle';
  if ((c >= 61 && c <= 67)) return 'rain';
  if ((c >= 71 && c <= 77) || c === 85 || c === 86) return 'snow';
  if (c >= 80 && c <= 82) return 'showers';
  if (c >= 95 && c <= 99) return 'storm';
  return 'cloudy';
}

const FAHRENHEIT = new Set(['US', 'LR', 'MM', 'BS', 'BZ', 'KY', 'PW', 'FM', 'MH']);
export const unitFor = (country) => (FAHRENHEIT.has(String(country || '').toUpperCase()) ? 'f' : 'c');

export const temp = (c, unit) => Math.round(unit === 'f' ? (c * 9) / 5 + 32 : c);

export function skyOf(kind, day) {
  const grey = kind === 'cloudy' || kind === 'fog' || kind === 'rain' || kind === 'drizzle' || kind === 'showers' || kind === 'storm';
  if (!day) return grey ? ['#2b3440', '#161b22'] : ['#1b2a4a', '#0b1226'];
  if (kind === 'snow') return ['#8fa6bf', '#5b7088'];
  return grey ? ['#6b7d90', '#46566a'] : ['#3d8fe0', '#2563b8'];
}

// ---- each widget's own city (0.8.9.1, #193) ----
// Until 0.8.9 every weather widget showed settings.weather.city, one city for all: changing it on one changed them
// all. Since 0.8.9.1 each widget keeps its own in opts.city: a city of the offline list ({ id, name, cc }) or null for
// the time zone's. settings.weather.city stays as the default: the last city chosen, which a new widget starts on
// and the calendar's weather style shows; a widget without opts.city (one made before migration 8) follows it.
//
//   cityOk(c)              { id, name, cc } when c is a city of the list (a whole id above 0), else null
//   widgetCity(opts, def)  the widget's city: its own when opts has one (null: the time zone's), else the default
//   placeOf(city, zone)    what lord.weather is asked for: "geonames/<id>", or the time zone without a city
//   withWidgetCities(s)    settings migration 8: every weather widget gets the city it showed (the default), so a
//                          later choice on another widget or a changed default never moves it
export function cityOk(c) {
  if (!c || typeof c !== 'object' || !Number.isInteger(c.id) || c.id <= 0) return null;
  return { id: c.id, name: typeof c.name === 'string' ? c.name.slice(0, 80) : '', cc: typeof c.cc === 'string' ? c.cc.slice(0, 2) : '' };
}
const own = (o, k) => !!o && typeof o === 'object' && Object.prototype.hasOwnProperty.call(o, k);
export const widgetCity = (opts, def) => (own(opts, 'city') ? cityOk(opts.city) : cityOk(def));
export const placeOf = (city, zone) => (city ? `geonames/${city.id}` : zone || null);
export function withWidgetCities(s) {
  if (!s || !Array.isArray(s.deskWidgets) || !s.deskWidgets.some((w) => w && w.type === 'weather' && !own(w.opts, 'city'))) return s;
  const def = cityOk(s.weather?.city);
  return { ...s, deskWidgets: s.deskWidgets.map((w) => (w && w.type === 'weather' && !own(w.opts, 'city') ? { ...w, opts: { ...(w.opts || {}), city: def } } : w)) };
}
