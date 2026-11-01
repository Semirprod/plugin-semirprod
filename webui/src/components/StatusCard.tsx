import { Dot, Row, Section } from '@/components/ui/section';
import { useT, type TFunction } from '@/i18n';
import type { PluginState } from '@/lib/ws';

type Tone = 'success' | 'warning' | 'danger' | 'neutral';

interface Link {
  label: string;
  value: string;
  hint: string;
  tone: Tone;
  pulse?: boolean;
}

function linksFor(s: PluginState, t: TFunction): Link[] {
  const backend: Link = {
    label: t('connections.link'),
    value: t('connections.link.offline'),
    hint: t('connections.link.offlineHint'),
    tone: 'neutral',
  };
  if (s.wsConnected) {
    backend.value = t('connections.link.connected');
    backend.hint = s.organizationName
      ? t('connections.link.workspace', { name: s.organizationName })
      : t('connections.link.sessionActive');
    backend.tone = 'success';
  } else if (s.authFlowRunning) {
    backend.value = t('connections.link.pairing');
    backend.hint = t('connections.link.pairingHint');
    backend.tone = 'warning';
    backend.pulse = true;
  } else if (s.connected) {
    backend.value = t('connections.link.interrupted');
    backend.hint = t('connections.link.interruptedHint');
    backend.tone = 'danger';
  }

  const source: Link = s.sourceReady
    ? {
        label: t('connections.source'),
        value: t('connections.source.live'),
        hint: `${s.sourceWidth} × ${s.sourceHeight} px`,
        tone: 'success',
      }
    : {
        label: t('connections.source'),
        value: t('connections.source.waiting'),
        hint: t('connections.source.waitingHint'),
        tone: 'neutral',
        pulse: true,
      };

  const studio: Link = {
    label: t('connections.access'),
    value: t('connections.access.checking'),
    hint: t('connections.access.checkingHint'),
    tone: 'neutral',
    pulse: true,
  };
  if (s.studioAccessKnown) {
    // Le motif vient de la matrice serveur (titre du refus), jamais du code brut
    // `studioReason` (« subscription_required »), qui s'affichait ici tel quel.
    // Pas de rouge : un abonnement absent n'est pas une panne. Le bandeau du
    // haut porte la phrase complète et le bouton.
    const live = s.features?.features.find((entry) => entry.feature === 'remote-control');
    studio.value = s.studioAccess ? t('connections.access.active') : t('connections.access.inactive');
    studio.hint = s.studioAccess
      ? t('connections.access.validHint')
      : (live?.title ?? t('connections.access.requiredHint'));
    studio.tone = s.studioAccess ? 'success' : 'neutral';
    studio.pulse = false;
  }

  // Pas de tuile « Mode » : le type de build et la politique d'endpoints sont
  // des détails internes, rien à afficher à l'utilisateur.
  return [backend, source, studio];
}

export function StatusCard({ state }: { state: PluginState }) {
  const t = useT();
  return (
    <Section title={t('connections.title')}>
      {linksFor(state, t).map((l) => (
        <Row
          key={l.label}
          title={l.label}
          hint={l.hint}
          control={
            <div className="flex items-center gap-2 text-[13px] font-medium">
              <Dot tone={l.tone} pulse={l.pulse} />
              {l.value}
            </div>
          }
        />
      ))}
    </Section>
  );
}
