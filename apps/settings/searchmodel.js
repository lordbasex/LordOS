// searchmodel.js — Settings' search (apps/settings/index.jsx): what each section is found by. Pure, node-tested by
// server/tests/settings_search_test.mjs.
// What a section is found by in the search: its name, its keywords and, for AI, the names of its functions and
// capabilities as they are shown (✨ Writing, Sentinela, Hands, Brain, Voice…; BUG-4 of the 0.8.9.6.2 QA: "Escritura"
// found nothing), and the short text its page shows: every row label, choice and short note (searchkeys.js, generated
// from the pages by tools/gen-settings-search.mjs; BUG-18 of the ws170 QA: "Acomodar" found nothing). Accents and case
// don't count ("musica" finds "Música").
import { SECTION_KEYS } from './searchkeys.js';

const AI_NAMES = ['writing.name', 'sentinela.name', 'hands.name', 'settings.aiStore.cap.brain', 'settings.aiStore.cap.ear',
  'settings.aiStore.cap.voice', 'settings.aiStore.cap.eye', 'settings.aiStore.cap.img'];
export const foldSearch = (s) => String(s || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
export function sectionWords(id, tr) {
  const words = [tr(`settings.section.${id}`), tr(`settings.keywords.${id}`)];
  if (id === 'ai') for (const k of AI_NAMES) words.push(tr(k));
  for (const k of SECTION_KEYS[id] || []) words.push(String(tr(k)).replace(/\{[^}]*\}/g, ' '));
  return words.join(' ');
}
