import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Mise à jour disponible.
//
// C'était une QMessageBox native. Elle volait le focus au démarrage d'OBS — en
// régie déportée, l'opérateur ne la voit même pas : son OBS a juste l'air figé.
// Et elle se dédupait par version : vue une fois, elle ne revenait jamais, même
// si la mise à jour n'était pas installée. Un bandeau fait l'inverse — il ne
// coupe rien et il reste tant que la version n'a pas bougé.
//
// Affiché avant même l'appairage : une mise à jour ne dépend pas du compte.
export function UpdateNotice({ state, send }: Props) {
  const t = useT();
  const version = state.updateVersion.trim();

  if (state.pluginBlocked) {
    const update = state.pluginBlockReason === 'update_required';
    return (
      <div className="flex items-start justify-between gap-4 rounded-lg border border-destructive/60 p-3.5">
        <div className="min-w-0">
          <div className="text-sm font-medium text-destructive">
            {update
              ? version
                ? t('update.requiredVersion', { version })
                : t('update.required')
              : t('update.blockedSecurity')}
          </div>
          <div className="mt-1 text-[12px] leading-snug text-muted-foreground">
            {update ? t('update.pausedUntilInstalled') : t('update.securityAdvice')}
          </div>
        </div>
        <Button size="sm" className="shrink-0" onClick={() => send({ type: 'open_update' })}>
          {t('update.install')}
        </Button>
      </div>
    );
  }

  if (!version) return null;

  const critical = state.updateSeverity === 'critical';

  return (
    <div
      className={cn(
        'flex items-start justify-between gap-4 rounded-lg border p-3.5',
        critical ? 'border-destructive/60' : 'border-border'
      )}
    >
      <div className="min-w-0">
        <div className={cn('text-sm font-medium', critical && 'text-destructive')}>
          {critical
            ? t('update.requiredVersion', { version })
            : t('update.available', { version })}
        </div>
        <div className="mt-1 text-[12px] leading-snug text-muted-foreground">
          {state.updateNotes ||
            (critical ? t('update.criticalNotes') : t('update.defaultNotes'))}
        </div>
      </div>
      <Button
        size="sm"
        variant={critical ? 'default' : 'outline'}
        className="shrink-0"
        onClick={() => send({ type: 'open_update' })}
      >
        {t('update.download')}
      </Button>
    </div>
  );
}
