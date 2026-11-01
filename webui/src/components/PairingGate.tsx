import { useState } from 'react';
import { Eye, EyeOff, Link2 } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Écran unique tant qu'aucun compte Semirprod n'est relié.
//
// Avant, la page affichait tout — Direct, Réglages, scènes, débit — alors que
// rien de tout ça ne peut fonctionner sans compte : les flux viennent du
// compte, l'accès Studio aussi. On montrait des réglages inertes, et la seule
// action utile (se connecter) était enterrée en bas de l'onglet Réglages.
//
// Même cadre que TokenGate : les deux sont la même chose à deux étages —
// relier la page au plugin, puis le plugin au compte.
export function PairingGate({ state, send }: Props) {
  const t = useT();
  const pairing = state.authFlowRunning;
  const code = state.accountLabel.trim();
  // `accountLabel` porte l'état du compte hors appairage : on ne remonte que
  // ce qui est une vraie erreur, pas le « Aucune session active. » par défaut
  // (le titre le dit déjà). Le plugin l'écrit dans la langue d'OBS et le drapeau
  // `pairingFailed` dit que c'est bien une erreur, sans deviner d'après le texte.
  const error = !pairing && state.pairingFailed ? code : '';
  // Code MASQUÉ par défaut : le dock se retrouve souvent à l'écran pendant un
  // direct, et n'importe quel compte qui lit ce code peut l'approuver dans son
  // propre espace, ce qui lui donnerait la main sur les scènes de ce poste. Le
  // navigateur s'ouvre déjà sur la page avec le code rempli : on ne le révèle
  // que pour le comparer, ou pour le taper ailleurs si rien ne s'est ouvert.
  const [revealed, setRevealed] = useState(false);
  const masked = code.replace(/[^-]/g, '•');

  return (
    <div className="mx-auto w-full max-w-sm rounded-lg border border-border p-8">
      <div className="flex flex-col items-center gap-4 text-center">
        <div className="flex h-10 w-10 items-center justify-center rounded-full border border-border">
          <Link2 className="h-4 w-4 text-muted-foreground" />
        </div>

        <div>
          <h2 className="text-base font-semibold">
            {pairing ? t('pairing.titleWaiting') : t('pairing.titleIdle')}
          </h2>
          <p className="mt-1 text-[13px] leading-snug text-muted-foreground">
            {pairing ? t('pairing.descriptionWaiting') : t('pairing.descriptionIdle')}
          </p>
        </div>

        {pairing ? (
          <>
            <div className="flex w-full items-center gap-2 rounded-lg border border-border px-4 py-3">
              <span className="flex-1 font-mono text-xl tracking-[0.2em]">
                {code ? (revealed ? code : masked) : '…'}
              </span>
              {code && (
                <button
                  type="button"
                  onClick={() => setRevealed((v) => !v)}
                  aria-label={revealed ? t('pairing.hideCode') : t('pairing.showCode')}
                  aria-pressed={revealed}
                  className="rounded-md p-1.5 text-muted-foreground transition-colors hover:text-foreground focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring"
                >
                  {revealed ? <EyeOff className="h-4 w-4" /> : <Eye className="h-4 w-4" />}
                </button>
              )}
            </div>
            {!revealed && code && (
              <p className="text-[12px] leading-snug text-muted-foreground">{t('pairing.codeHidden')}</p>
            )}
            {state.accountUrl && (
              <p className="text-[12px] leading-snug text-muted-foreground">
                {t('pairing.fallbackUrl')}{' '}
                <span className="font-mono text-foreground">
                  {state.accountUrl}
                </span>
              </p>
            )}
          </>
        ) : (
          <>
            {error && (
              <p className="text-[12px] leading-snug text-destructive">{error}</p>
            )}
            <Button className="w-full" onClick={() => send({ type: 'connect' })}>
              {t('pairing.connect')}
            </Button>
          </>
        )}
      </div>
    </div>
  );
}
