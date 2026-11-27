// searchmodel.js — Settings' search (apps/settings/index.jsx): what each section is found by. Pure, node-tested by
// server/tests/settings_search_test.mjs.
// What a section is found by in the search: its name, its keywords and, for AI, the names of its functions and
// capabilities as they are shown (✨ Writing, Sentinela, Hands, Brain, Voice…; BUG-4 of the 0.8.9.6.2 QA: "Escritura"
// found nothing). Accents and case don't count ("musica" finds "Música").
const AI_NAMES = ['writing.name', 'sentinela.name', 'hands.name', 'settings.aiStore.cap.brain', 'settings.aiStore.cap.ear',
  'settings.aiStore.cap.voice', 'settings.aiStore.cap.eye', 'settings.aiStore.cap.img'];
export const foldSearch = (s) => String(s || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
export function sectionWords(id, tr) {
  const words = [tr(`settings.section.${id}`), tr(`settings.keywords.${id}`)];
  if (id === 'ai') for (const k of AI_NAMES) words.push(tr(k));
  return words.join(' ');
}
