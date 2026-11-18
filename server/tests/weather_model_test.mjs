// weather_model_test.mjs — the weather widget's plain logic (sdk/weathermodel.js): WMO codes to kinds, the unit by
// country, the conversion and the card's sky. node server/tests/weather_model_test.mjs
import { kindOf, unitFor, temp, skyOf } from '../../sdk/weathermodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
eq('kinds by WMO code', [0, 1, 2, 3, 45, 48, 51, 57, 61, 67, 71, 77, 80, 82, 85, 95, 99].map(kindOf),
  ['clear', 'partly', 'partly', 'cloudy', 'fog', 'fog', 'drizzle', 'drizzle', 'rain', 'rain', 'snow', 'snow', 'showers', 'showers', 'snow', 'storm', 'storm']);
eq('an unknown code is cloudy', [kindOf(42), kindOf('x')], ['cloudy', 'cloudy']);
eq('Fahrenheit only where it is the custom', ['US', 'us', 'AR', 'ES', 'BR', '', 'LR'].map(unitFor), ['f', 'f', 'c', 'c', 'c', 'c', 'f']);
eq('conversion, rounded', [temp(24.3, 'c'), temp(24.3, 'f'), temp(-40, 'f'), temp(0, 'f')], [24, 76, -40, 32]);
eq('a sky for each case', [skyOf('clear', 1)[0], skyOf('rain', 1)[0], skyOf('clear', 0)[0], skyOf('snow', 1)[0]], ['#3d8fe0', '#6b7d90', '#1b2a4a', '#8fa6bf']);
console.log(bad ? `weather-model-test: ${bad} FAILED` : `weather-model-test: ${ok} ok`);
if (bad) process.exit(1);
