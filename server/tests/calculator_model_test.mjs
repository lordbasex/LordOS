// node server/tests/calculator_model_test.mjs — apps/calculator/model.js: the arithmetic, % and ± (GitHub #271).
import assert from 'node:assert/strict';
import { evaluate, addPercent, toggleSign } from '../../apps/calculator/model.js';

const ev = (s, v) => assert.equal(evaluate(s), v, s);
// what it did before
ev('12+3', 15); ev('2+3×4', 14); ev('(2+3)×4', 20); ev('10÷4', 2.5); ev('5×-3', -15); ev('5--3', 8); ev('0.1+0.2', 0.3);
assert.throws(() => evaluate('1÷0')); assert.throws(() => evaluate('2+')); assert.throws(() => evaluate('(2'));
// % after + and −: percent of what is before
ev('200+10%', 220); ev('200-10%', 180); ev('50+50%', 75); ev('2×100+10%', 220); ev('(100+100)-25%', 150);
// % anywhere else: / 100
ev('200×10%', 20); ev('200÷50%', 400); ev('50%', 0.5); ev('-50%', -0.5); ev('10%+5', 5.1);
// % of a term that isn't alone after +: plain / 100 inside it
ev('100+10%×2', 100.2);
// a negative percentage after +
ev('200+-10%', 180);
assert.throws(() => evaluate('%'));
assert.throws(() => evaluate('5%%'));
// the % key: only after a number or ")"
assert.equal(addPercent('200+10'), '200+10%');
assert.equal(addPercent('(5+5)'), '(5+5)%');
assert.equal(addPercent('200+'), '200+');
assert.equal(addPercent(''), '');
// ±: the sign of the entry being typed
assert.equal(toggleSign('12'), '-12');
assert.equal(toggleSign('-12'), '12');
assert.equal(toggleSign('5+3'), '5+-3');
assert.equal(toggleSign('5+-3'), '5+3');
assert.equal(toggleSign('5-3'), '5--3');          // the binary minus stays; the entry becomes −3
assert.equal(toggleSign('5--3'), '5-3');
assert.equal(toggleSign('5×2.5'), '5×-2.5');
assert.equal(toggleSign('200+10%'), '200+-10%');
assert.equal(toggleSign('(4'), '(-4');
assert.equal(toggleSign(''), '-');
assert.equal(toggleSign('-'), '');
assert.equal(toggleSign('5×'), '5×-');
assert.equal(toggleSign('5×-'), '5×');
assert.equal(toggleSign('(2+3)'), '(2+3)');      // a closed group is left as it is
assert.equal(evaluate(toggleSign('5-3')), 8);
assert.equal(evaluate(toggleSign('2+3')), -1);
console.log('calculator-model-test: ok');
