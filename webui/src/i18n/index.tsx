import { createContext, useContext, type ReactNode } from 'react';

import en from './en.json';
import fr from './fr.json';

// i18n minimal de l'interface du plugin. Deux langues, pas de bibliothèque.
//
// La langue est celle d'OBS (`obs_get_locale()` côté plugin), poussée dans
// l'instantané sous `locale`. Elle est fixe pour la session : OBS redémarre
// pour en changer.
//
// Les dictionnaires sont PLATS (clé pointée -> texte). Une valeur est soit un
// texte, soit un objet de pluriels `{ one, other }` choisi par Intl.PluralRules
// d'après la variable `count`. `{nom}` est remplacé par la variable du même nom.
//
// `fr.json` fait foi pour la liste des clés : `en.json` doit avoir exactement
// les mêmes, sinon `tsc -b` échoue (voir `EN` plus bas).

export type Locale = 'fr' | 'en';

type Plural = { one: string; other: string };
type Message = string | Plural;

export type MessageKey = keyof typeof fr;
export type Vars = Record<string, string | number>;
export type TFunction = (key: MessageKey, vars?: Vars) => string;

const FR: Record<MessageKey, Message> = fr;
// Mêmes clés que le français, ni plus ni moins.
const EN: Record<MessageKey, Message> = en;

const DICTIONARIES: Record<Locale, Record<MessageKey, Message>> = { fr: FR, en: EN };

/** Étiquette BCP 47 pour Intl (dates, nombres, pluriels). */
export function intlTag(locale: Locale): string {
  return locale === 'fr' ? 'fr-FR' : 'en-US';
}

/** Tout ce qui ne commence pas par « fr » est de l'anglais. */
export function normalizeLocale(value: unknown): Locale {
  return typeof value === 'string' && value.toLowerCase().startsWith('fr') ? 'fr' : 'en';
}

export interface I18n {
  locale: Locale;
  t: TFunction;
}

export function createI18n(locale: Locale): I18n {
  const dict = DICTIONARIES[locale];
  const plurals = new Intl.PluralRules(intlTag(locale));

  const t: TFunction = (key, vars) => {
    const entry = dict[key] ?? FR[key] ?? key;
    let text: string;
    if (typeof entry === 'string') {
      text = entry;
    } else {
      const count = Number(vars?.count ?? 0);
      text = plurals.select(count) === 'one' ? entry.one : entry.other;
    }
    if (!vars) return text;
    return text.replace(/\{(\w+)\}/g, (whole, name: string) =>
      name in vars ? String(vars[name]) : whole
    );
  };

  return { locale, t };
}

// Français tant que le premier instantané n'est pas arrivé.
export const I18nContext = createContext<I18n>(createI18n('fr'));

export function I18nProvider({ value, children }: { value: I18n; children: ReactNode }) {
  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>;
}

/** `const t = useT(); t('tab.live')`. */
export function useT(): TFunction {
  return useContext(I18nContext).t;
}

export function useLocale(): Locale {
  return useContext(I18nContext).locale;
}
