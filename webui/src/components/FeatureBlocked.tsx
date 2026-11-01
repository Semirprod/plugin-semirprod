import { Button } from '@/components/ui/button';
import type { Command, PluginFeatureVerdict, PluginState } from '@/lib/ws';

interface Props {
  verdict: PluginFeatureVerdict;
  state: PluginState;
  send: (cmd: Command) => void;
}

/**
 * Une fonction fermee, dite comme le serveur la dit : un titre, une phrase, et
 * le bouton qui la rouvre.
 *
 * Les textes ne sont PAS ecrits ici : ils viennent de la matrice serveur
 * (`describePluginFeatureRefusal`), pour que le plugin et le site disent la meme
 * chose. Pas de rouge : ce n'est pas une panne, c'est un etat commercial.
 *
 * Le bouton n'apparait que s'il mene quelque part : une mise a jour sans version
 * annoncee par le manifeste, ou un geste sans page (une place s'attribue par un
 * administrateur), ne proposent pas un bouton qui ne ferait rien.
 */
export function FeatureBlocked({ verdict, state, send }: Props) {
  const canAct =
    verdict.action === 'update-plugin'
      ? !!state.updateVersion
      : !!verdict.action && !!verdict.actionPath;

  return (
    <div className="flex items-start justify-between gap-4 px-4 py-3.5">
      <div className="min-w-0">
        <div className="text-sm font-medium">{verdict.title}</div>
        {verdict.message && (
          <div className="mt-1 text-[12px] leading-snug text-muted-foreground">{verdict.message}</div>
        )}
      </div>
      {canAct && verdict.actionLabel && (
        <Button
          size="sm"
          variant="outline"
          className="shrink-0"
          onClick={() => send({ type: 'open_feature_action', feature: verdict.feature })}
        >
          {verdict.actionLabel}
        </Button>
      )}
    </div>
  );
}
