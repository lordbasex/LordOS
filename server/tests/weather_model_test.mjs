// weather_model_test.mjs — the weather widget's plain logic (sdk/weathermodel.js): WMO codes to kinds, the unit by
// country, the conversion, the card's sky, and each widget's own city (0.8.9.1, #193). node server/tests/weather_model_test.mjs
import { kindOf, unitFor, temp, skyOf, cityOk, widgetCity, placeOf, withWidgetCities } from '../../sdk/weathermodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
eq('kinds by WMO code', [0, 1, 2, 3, 45, 48, 51, 57, 61, 67, 71, 77, 80, 82, 85, 95, 99].map(kindOf),
  ['clear', 'partly', 'partly', 'cloudy', 'fog', 'fog', 'drizzle', 'drizzle', 'rain', 'rain', 'snow', 'snow', 'showers', 'showers', 'snow', 'storm', 'storm']);
eq('an unknown code is cloudy', [kindOf(42), kindOf('x')], ['cloudy', 'cloudy']);
eq('Fahrenheit only where it is the custom', ['US', 'us', 'AR', 'ES', 'BR', '', 'LR'].map(unitFor), ['f', 'f', 'c', 'c', 'c', 'c', 'f']);
eq('conversion, rounded', [temp(24.3, 'c'), temp(24.3, 'f'), temp(-40, 'f'), temp(0, 'f')], [24, 76, -40, 32]);
eq('a sky for each case', [skyOf('clear', 1)[0], skyOf('rain', 1)[0], skyOf('clear', 0)[0], skyOf('snow', 1)[0]], ['#3d8fe0', '#6b7d90', '#1b2a4a', '#8fa6bf']);
// each widget's own city (#193)
const nice = { id: 2988507, name: 'Paris', cc: 'FR' }, rouret = { id: 3002280, name: 'Le Rouret', cc: 'FR' };
eq('a city of the list', cityOk({ ...nice, extra: 1 }), nice);
eq('not a city', [cityOk(null), cityOk({ id: 0 }), cityOk({ id: -3 }), cityOk({ id: 1.5 }), cityOk({ id: '12' }), cityOk('x')], [null, null, null, null, null, null]);
eq('a widget without its own city follows the default', widgetCity({}, nice), nice);
eq('no opts at all: the default', widgetCity(undefined, nice), nice);
eq('its own city wins', widgetCity({ city: rouret }, nice), rouret);
eq('its own "time zone" (null) wins over the default', widgetCity({ city: null }, nice), null);
eq('a broken own city is the time zone, not the default', widgetCity({ city: { id: 'x' } }, nice), null);
eq('no city anywhere: the time zone', widgetCity({}, null), null);
eq('what is asked for', [placeOf(rouret, 'Europe/Paris'), placeOf(null, 'Europe/Paris'), placeOf(null, '')], ['geonames/3002280', 'Europe/Paris', null]);
// two widgets: one moved to another city, the other keeps its own
const two = [{ id: 'a', type: 'weather', opts: { city: nice } }, { id: 'b', type: 'weather', opts: { city: rouret } }];
eq('two widgets, two places', two.map((w) => placeOf(widgetCity(w.opts, null), 'Europe/Paris')), ['geonames/2988507', 'geonames/3002280']);
// migration 8: the widgets keep the city they showed
const before = { schema: 7, weather: { on: true, city: nice }, deskWidgets: [
  { id: 'w1', type: 'weather', size: 's', opts: {} }, { id: 'w2', type: 'weather', size: 'l' }, { id: 'c1', type: 'clock', opts: { style: 'digital' } },
  { id: 'w3', type: 'weather', opts: { city: null } }] };
const after = withWidgetCities(before);
eq('migration 8: each weather widget gets the city it showed', after.deskWidgets.map((w) => w.opts?.city ?? null), [nice, nice, null, null]);
eq('migration 8: other kinds untouched', after.deskWidgets[2], before.deskWidgets[2]);
eq('migration 8: a widget with its own choice keeps it', after.deskWidgets[3], before.deskWidgets[3]);
eq('migration 8: the default stays', after.weather, before.weather);
eq('migration 8: what each shows is unchanged', before.deskWidgets.map((w) => widgetCity(w.opts, before.weather.city)), after.deskWidgets.map((w) => widgetCity(w.opts, after.weather.city)));
const auto = withWidgetCities({ weather: { on: true }, deskWidgets: [{ id: 'w', type: 'weather', opts: {} }] });
eq('migration 8: without a default, the time zone (and it stays so)', [Object.prototype.hasOwnProperty.call(auto.deskWidgets[0].opts, 'city'), auto.deskWidgets[0].opts.city], [true, null]);
const none = { weather: { on: false }, deskWidgets: [{ id: 'c', type: 'clock' }] };
eq('migration 8: nothing to do, the same object', withWidgetCities(none) === none && withWidgetCities({}) !== null, true);
console.log(bad ? `weather-model-test: ${bad} FAILED` : `weather-model-test: ${ok} ok`);
if (bad) process.exit(1);
