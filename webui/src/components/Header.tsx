import { X } from 'lucide-react';

import { Dot } from '@/components/ui/section';
import { cn } from '@/lib/utils';
import { useT, type MessageKey } from '@/i18n';
import type { LinkStatus } from '@/lib/ws';

import logoWhite from '@/assets/logo-white.png';

type Tone = 'success' | 'warning' | 'danger' | 'neutral';

const DOT: Record<LinkStatus, { tone: Tone; label: MessageKey; pulse: boolean }> = {
  idle: { tone: 'neutral', label: 'link.idle', pulse: true },
  connecting: { tone: 'warning', label: 'link.connecting', pulse: true },
  online: { tone: 'success', label: 'link.online', pulse: false },
  offline: { tone: 'danger', label: 'link.offline', pulse: false },
};

interface Props {
  status: LinkStatus;
  version: string;
  /** Instance cloud : la fenetre n'a pas de barre de titre, la fermeture est ici. */
  onClose?: () => void;
}

// `devBuild` n est plus affiche : le mode de build est une information interne,
// et l en-tete est rendu sur TOUTES les vues, y compris l ecran de saisie du
// jeton. Un badge « DEV » visible pendant un partage d ecran ou une VOD signale
// a un observateur qu il s agit d un binaire hors production.
export function Header({ status, version, onClose }: Props) {
  const t = useT();
  const dot = DOT[status];
  return (
    <header className="flex items-center justify-between gap-3">
      <div className="flex items-center gap-2.5">
        <img src={logoWhite} alt="" className="h-5 w-5" />
        <div className="text-[15px] font-semibold tracking-tight">
          SEMIRPROD <span className="text-muted-foreground">STUDIO</span>
        </div>
      </div>
      <div className="flex items-center gap-3 text-[12px] text-muted-foreground">
        <span className="flex items-center gap-2">
          <Dot tone={dot.tone} pulse={dot.pulse} />
          {t(dot.label)}
        </span>
        {/* Pastille version : filet seul, pas de pastille grise pleine. */}
        <span
          className={cn(
            'rounded-full border px-2 py-0.5 font-mono text-[11px]',
            'border-border text-muted-foreground'
          )}
        >
          {`v${version}`}
        </span>
        {onClose && (
          <button
            type="button"
            onClick={onClose}
            aria-label={t('common.close')}
            className="-mr-1 rounded-md p-1.5 text-muted-foreground transition-colors hover:bg-white/5 hover:text-foreground"
          >
            <X className="h-4 w-4" />
          </button>
        )}
      </div>
    </header>
  );
}
