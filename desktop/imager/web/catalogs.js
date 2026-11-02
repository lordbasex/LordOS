// catalogs.js — the imager's texts in the desktop app: the same catalogs LordOS ships
// (assets/locales/<lang>/), so both apps say the same thing in the same languages.
import enImager from '../../../assets/locales/en/imager.json';
import esImager from '../../../assets/locales/es/imager.json';
import ptImager from '../../../assets/locales/pt/imager.json';
import frImager from '../../../assets/locales/fr/imager.json';
import enErrors from '../../../assets/locales/en/errors.json';
import esErrors from '../../../assets/locales/es/errors.json';
import ptErrors from '../../../assets/locales/pt/errors.json';
import frErrors from '../../../assets/locales/fr/errors.json';
import enStatus from '../../../assets/locales/en/status.json';
import esStatus from '../../../assets/locales/es/status.json';
import ptStatus from '../../../assets/locales/pt/status.json';
import frStatus from '../../../assets/locales/fr/status.json';
import enMascot from '../../../assets/locales/en/mascot.json';
import esMascot from '../../../assets/locales/es/mascot.json';
import ptMascot from '../../../assets/locales/pt/mascot.json';
import frMascot from '../../../assets/locales/fr/mascot.json';
import enCountries from '../../../assets/locales/en/countries.json';
import esCountries from '../../../assets/locales/es/countries.json';
import ptCountries from '../../../assets/locales/pt/countries.json';
import frCountries from '../../../assets/locales/fr/countries.json';
import enShell from '../../../assets/locales/en/shell.json';
import esShell from '../../../assets/locales/es/shell.json';
import ptShell from '../../../assets/locales/pt/shell.json';
import frShell from '../../../assets/locales/fr/shell.json';

// shell.json: only the wallpaper names are used here
const walls = (s) => Object.fromEntries(Object.entries(s).filter(([k]) => k.startsWith('wallpaper.')));

export const catalogs = {
  en: { imager: enImager, errors: enErrors, status: enStatus, mascot: enMascot, countries: enCountries, shell: walls(enShell) },
  es: { imager: esImager, errors: esErrors, status: esStatus, mascot: esMascot, countries: esCountries, shell: walls(esShell) },
  pt: { imager: ptImager, errors: ptErrors, status: ptStatus, mascot: ptMascot, countries: ptCountries, shell: walls(ptShell) },
  fr: { imager: frImager, errors: frErrors, status: frStatus, mascot: frMascot, countries: frCountries, shell: walls(frShell) },
};
