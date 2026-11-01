import { useEffect, useMemo, useState } from 'react';
import { AlertTriangle } from 'lucide-react';

import { Header } from '@/components/Header';
import { TokenGate } from '@/components/TokenGate';
import { PairingGate } from '@/components/PairingGate';
import { SubscribeNotice } from '@/components/SubscribeNotice';
import { UpdateNotice } from '@/components/UpdateNotice';
import { RemoteActivityNotice } from '@/components/RemoteActivityNotice';
import { StatusCard } from '@/components/StatusCard';
import { AccountCard } from '@/components/AccountCard';
import { SceneCard } from '@/components/SceneCard';
import { DiffusionSettings } from '@/components/DiffusionSettings';
import { StreamsPanel } from '@/components/StreamsPanel';
import { CollectionsPanel } from '@/components/CollectionsPanel';
import { MachineSettings } from '@/components/MachineSettings';
import { StatsCard } from '@/components/StatsCard';
import { SessionsCard } from '@/components/SessionsCard';
import { cn } from '@/lib/utils';
import { resolveToken, useWsBridge } from '@/lib/ws';
import { I18nProvider, createI18n, normalizeLocale, type MessageKey } from '@/i18n';

type Tab = 'direct' | 'collections' | 'reglages';

const TABS: { id: Tab; label: MessageKey }[] = [
  { id: 'direct', label: 'tab.live' },
  // AVANT « Réglages », et c'est le point : sauvegarder n'est pas un réglage.
  // Un réglage se pose une fois, une sauvegarde se refait. La carte a vécu dans
  // « Réglages » entre les scènes et le nom du poste : le geste le plus fréquent
  // y était le plus enfoui.
  { id: 'collections', label: 'tab.collections' },
  { id: 'reglages', label: 'tab.settings' },
];

export default function App() {
  const { status, state, connect, send } = useWsBridge();
  const [tab, setTab] = useState<Tab>('direct');

  // Langue d'OBS, fixe pour la session. Français tant que le premier
  // instantané n'est pas arrivé.
  const locale = normalizeLocale(state?.locale ?? 'fr');
  const i18n = useMemo(() => createI18n(locale), [locale]);
  const { t } = i18n;

  useEffect(() => {
    document.documentElement.lang = locale;
  }, [locale]);

  // L'onglet Collections n'existe que si le plugin annonce `backups`.
  //
  // C'est ce qui permet à UN SEUL bundle de servir les deux variantes du
  // binaire : celle qui expose la sauvegarde de collections (option CMake
  // `SEMIRPROD_COLLECTIONS`, la valeur par défaut) et celle compilée sans,
  // pour un backend qui n'a pas encore les routes. Un second bundle aurait
  // dérivé du premier à la première correction faite d'un seul côté.
  //
  // Ne pas confondre avec « pas encore reçu » : tant que le WS n'est pas relié,
  // `state` est nul et cette page rend une porte, pas des onglets.
  const visibleTabs = useMemo(
    () => TABS.filter((item) => item.id !== 'collections' || !!state?.backups),
    [state?.backups]
  );

  // Onglet effectif, dérivé au lieu d'être corrigé par un effet : si l'onglet
  // sélectionné disparaît en cours de session (le plugin cesse d'annoncer
  // `backups`), on retombe sur Direct sans re-render supplémentaire ni écran
  // vide d'une frame.
  const activeTab: Tab = visibleTabs.some((item) => item.id === tab) ? tab : 'direct';

  // Auto-connect on load if a token is already present (URL hash or storage).
  useEffect(() => {
    const token = resolveToken();
    if (token) connect(token);
  }, [connect]);

  const online = status === 'online';
  const version = state?.pluginVersion || '0.0.0';

  return (
    <I18nProvider value={i18n}>
    <div className="mx-auto flex min-h-full max-w-3xl flex-col gap-5 p-6">
      <Header
        status={status}
        version={version}
        onClose={state?.kiosk ? () => send({ type: 'close_studio' }) : undefined}
      />

      {/* Avant les deux portes : une mise à jour ne dépend ni du jeton du pont
          ni du compte, et une version obligatoire doit se voir même sur un
          poste pas encore appairé. */}
      {state && <UpdateNotice state={state} send={send} />}

      {!online || !state ? (
        <div className="flex flex-1 items-center justify-center py-12">
          <TokenGate onSubmit={connect} />
        </div>
      ) : !state.connected ? (
        // Deux portes successives : le jeton relie la page au plugin, celle-ci
        // relie le plugin au compte. Sans compte, il n'y a ni flux, ni accès
        // Studio, ni scène à mapper — afficher les onglets reviendrait à
        // proposer des réglages qui ne peuvent rien piloter.
        <div className="flex flex-1 items-center justify-center py-12">
          <PairingGate state={state} send={send} />
        </div>
      ) : (
        <>
          {/* Abonnement inactif : bandeau permanent, tous onglets confondus.
              C'est la raison pour laquelle un direct sera refusé. */}
          <SubscribeNotice state={state} send={send} />

          {/* Bandeau santé : visible quel que soit l'onglet, le direct passe
              avant la navigation. */}
          {state.healthAlert && (
            <div className="flex items-start gap-2.5 rounded-lg border border-destructive/60 p-3.5">
              <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0 text-destructive" />
              <div>
                <div className="text-sm font-semibold text-destructive">
                  {t('health.title')}
                </div>
                <div className="mt-0.5 text-[12px] leading-snug text-muted-foreground">
                  {t('health.detail', {
                    reason: state.healthReason || t('health.fallback'),
                  })}
                </div>
              </div>
            </div>
          )}

          {/* Actions distantes sensibles : apercu du programme en cours,
              destination de diffusion changee par la regie. */}
          <RemoteActivityNotice state={state} send={send} />

          <nav className="flex gap-6 border-b border-border">
            {visibleTabs.map((tabItem) => (
              <button
                key={tabItem.id}
                onClick={() => setTab(tabItem.id)}
                className={cn(
                  '-mb-px border-b px-0.5 pb-2.5 text-sm font-medium transition-colors',
                  activeTab === tabItem.id
                    ? 'border-foreground text-foreground'
                    : 'border-transparent text-muted-foreground hover:text-foreground'
                )}
              >
                {t(tabItem.label)}
              </button>
            ))}
          </nav>

          {/* Une seule colonne : le WebUI vit dans un dock OBS étroit, deux
              colonnes y écrasaient les libellés. L'ordre suit l'urgence :
              l'état du direct d'abord, l'administratif ensuite. */}
          {activeTab === 'collections' ? (
            <CollectionsPanel state={state} send={send} />
          ) : activeTab === 'direct' ? (
            <div className="flex flex-col gap-6">
              <StatsCard state={state} />
              <StatusCard state={state} />
              <SessionsCard state={state} />
            </div>
          ) : (
            <div className="flex flex-col gap-6">
              <DiffusionSettings state={state} send={send} />
              {/* Juste sous le flux du poste : on vient de lire quel flux ce
                  poste suit, la question suivante est « et les autres ? ». */}
              <StreamsPanel state={state} send={send} />
              {/* Plus de garde `state.connected` : cet onglet n'est atteignable
                  que compte relié, la porte est au-dessus. */}
              <SceneCard state={state} send={send} />
              <MachineSettings state={state} send={send} />
              <AccountCard state={state} send={send} />
            </div>
          )}
        </>
      )}
    </div>
    </I18nProvider>
  );
}
