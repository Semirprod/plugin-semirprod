import { Button } from '@/components/ui/button';
import { FeatureBlocked } from '@/components/FeatureBlocked';
import { useT } from '@/i18n';
import type { Command, PluginState } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Compte relié mais sans accès au direct : le serveur refuse le passage en
// direct, donc autant le dire ici plutôt que de laisser l'opérateur le
// découvrir au moment de passer en live.
//
// Le MOTIF vient de la matrice serveur (`features`) : pas d'abonnement, paiement
// en échec, place manquante… chacun avec sa phrase et son bouton. Avant elle, ce
// bandeau affichait le code brut du serveur (« subscription_required — … ») et
// proposait « S'abonner » à tout le monde, y compris à un abonné dont seul le
// paiement avait échoué.
//
// Volontairement pas en rouge : ce n'est pas une panne, c'est un état
// commercial. Le rouge est réservé à ce qui casse une diffusion en cours.
export function SubscribeNotice({ state, send }: Props) {
  const t = useT();
  const live = state.features?.features.find((entry) => entry.feature === 'remote-control');

  // Impayé dont le délai court encore : RIEN n'est coupé, donc rien n'est grisé.
  // Mais le client perd sa diffusion à la date dite s'il ne fait rien, et son
  // OBS est le seul endroit où il le lira à temps. Passe avant le refus : tant
  // que la tolérance tient, il n'y a pas de refus à afficher.
  const grace = state.features?.paymentGrace;
  if (grace) {
    return (
      <div className="flex items-start justify-between gap-4 rounded-lg border border-border p-3.5">
        <div className="min-w-0">
          <div className="text-sm font-medium">{grace.title}</div>
          <div className="mt-1 text-[12px] leading-snug text-muted-foreground">{grace.message}</div>
        </div>
        {grace.actionPath && (
          <Button
            size="sm"
            variant="outline"
            className="shrink-0"
            onClick={() => send({ type: 'open_feature_action', feature: 'payment-grace' })}
          >
            {grace.actionLabel}
          </Button>
        )}
      </div>
    );
  }

  if (live) {
    if (live.allowed) return null;
    return (
      <div className="rounded-lg border border-border">
        <FeatureBlocked verdict={live} send={send} />
      </div>
    );
  }

  // Serveur qui ne publie pas encore la matrice : on ne connaît que « pas
  // d'accès », sans le motif. Phrase neutre, et le bouton mène à l'offre.
  if (!state.studioAccessKnown || state.studioAccess) return null;

  return (
    <div className="flex items-start justify-between gap-4 rounded-lg border border-border p-3.5">
      <div className="min-w-0">
        <div className="text-sm font-medium">{t('subscribe.title')}</div>
        <div className="mt-1 text-[12px] leading-snug text-muted-foreground">
          {t('subscribe.message')}
        </div>
      </div>
      <Button
        size="sm"
        variant="outline"
        className="shrink-0"
        onClick={() => send({ type: 'open_subscribe' })}
      >
        {t('subscribe.action')}
      </Button>
    </div>
  );
}
