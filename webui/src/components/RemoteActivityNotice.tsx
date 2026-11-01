import { Eye, Radio } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Actions distantes sensibles, rendues visibles sur le poste.
//
// Un compte d'espace détourné pouvait regarder le programme (aperçus WHIP,
// captures) ou poser une autre destination de diffusion sans que rien ne se
// voie dans OBS. Les deux lignes restent tant que la situation dure (aperçu)
// ou jusqu'à ce que le streamer la ferme (destination).
export function RemoteActivityNotice({ state, send }: Props) {
  const t = useT();
  const destination = (state.remoteDestinationNotice ?? '').trim();

  // Instance cloud : la régie pilote et regarde par nature. Rien à signaler,
  // et le bouton couperait ses aperçus.
  if (state.kiosk) return null;
  if (!state.remotePreviewActive && !destination) return null;

  return (
    <div className="flex flex-col gap-2">
      {state.remotePreviewActive && (
        <div className="flex items-center justify-between gap-4 rounded-lg border border-border p-3">
          <div className="flex min-w-0 items-center gap-2.5">
            <Eye className="h-4 w-4 shrink-0 text-muted-foreground" />
            <div className="text-sm font-medium">{t('remote.previewActive')}</div>
          </div>
          <Button
            size="sm"
            variant="outline"
            className="shrink-0"
            onClick={() => send({ type: 'set_preview_enabled', on: false })}
          >
            {t('remote.stopPreview')}
          </Button>
        </div>
      )}
      {destination && (
        <div className="flex items-center justify-between gap-4 rounded-lg border border-border p-3">
          <div className="flex min-w-0 items-center gap-2.5">
            <Radio className="h-4 w-4 shrink-0 text-muted-foreground" />
            <div className="min-w-0 break-words text-sm font-medium">{destination}</div>
          </div>
          <Button
            size="sm"
            variant="ghost"
            className="shrink-0"
            onClick={() => send({ type: 'dismiss_remote_notice' })}
          >
            {t('remote.dismiss')}
          </Button>
        </div>
      )}
    </div>
  );
}
