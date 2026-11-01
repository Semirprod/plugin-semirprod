import type { PluginState } from './ws';

// Harnais de prévisualisation local. Uniquement actif en `npm run dev` avec
// `?mock=1` dans l'URL : permet de voir toutes les cartes sans OBS ni plugin
// (le WS réel exige un token qui tourne à chaque démarrage d'OBS).
// `import.meta.env.DEV` est constant-folded par Vite, donc ce module et son
// import disparaissent du bundle de production.
export function mockEnabled(): boolean {
  return import.meta.env.DEV && new URLSearchParams(window.location.search).has('mock');
}

// `?lang=en` bascule l'aperçu en anglais (le plugin, lui, envoie la langue d'OBS).
const mockLocale: 'fr' | 'en' =
  import.meta.env.DEV && new URLSearchParams(window.location.search).get('lang') === 'en'
    ? 'en'
    : 'fr';

export const MOCK_STATE: PluginState = {
  locale: mockLocale,
  connected: true,
  wsConnected: true,
  organizationName: 'Studio démo',
  authFlowRunning: false,
  pluginVersion: '0.1.4',
  // Manquait, alors que le type l'exige : le build du webui echouait donc
  // AVANT toute modification, et le fichier embarque ne pouvait plus etre
  // regenere.
  devBuild: false,
  accountLabel: 'Studio Semirprod',
  accountUrl: 'https://semirprod.fr/dashboard',
  accountName: 'Alex Martin',
  accountEmail: 'contact@semirprod.fr',
  // Vide : l'aperçu montre le dégradé par défaut. Le vrai avatar n'existe que
  // servi par le plugin, il n'y a rien à simuler ici.
  accountAvatar: '',
  // Vides : l'aperçu montre l'état courant, pas un bandeau de mise à jour.
  // Mets une version ici pour voir UpdateNotice.
  updateVersion: '',
  updateSeverity: '',
  updateNotes: '',
  pluginBlocked: false,
  streams: [
    { id: 'a1', name: 'Caméra principale', active: true, scenes: 2, receiving: true },
    { id: 'a2', name: 'Caméra scène', active: false, scenes: 1, receiving: true },
    { id: 'a3', name: 'Plateau régie', active: false, scenes: 0, receiving: false },
  ],
  sceneLive: 'Direct',
  sceneBug: 'Écran technique',
  autoSwitch: true,
  previewEnabled: true,
  streaming: true,
  recording: false,
  streamingStartedAt: new Date(Date.now() - 4_517_000).toISOString(),
  statsActive: true,
  statsKbps: 6800,
  statsDroppedFrames: 214,
  statsTotalFrames: 182_400,
  statsCongestion: 0.12,
  instanceLabel: 'Régie principale',
  healthAlert: true,
  healthReason: 'Congestion réseau soutenue depuis 40 s.',
  connTestRunning: false,
  connTestApiMs: 84,
  connTestLiveMs: 312,
  connTestError: '',
  sessions: [
    {
      endedAt: new Date(Date.now() - 86_400_000).toISOString(),
      durationSec: 7380,
      avgKbps: 7900,
      peakKbps: 8200,
      droppedFrames: 12,
      totalFrames: 331_000,
      droppedPct: 0.0,
      incidents: 0,
    },
    {
      endedAt: new Date(Date.now() - 3 * 86_400_000).toISOString(),
      durationSec: 2640,
      avgKbps: 6100,
      peakKbps: 8000,
      droppedFrames: 4300,
      totalFrames: 118_000,
      droppedPct: 3.6,
      incidents: 2,
    },
  ],
  _scenes: ['Direct', 'Écran technique', 'Intro', 'Pause'],
  studioAccess: true,
  studioAccessKnown: true,
  studioReason: '',
  sourceReady: true,
  sourceWidth: 1920,
  sourceHeight: 1080,
  sourceInLiveScene: true,
  sourceScenes: ['Direct'],
  backups: {
    phase: 'idle',
    progress: 0,
    error: '',
    collections: ['Émission du soir', 'Plateau', 'Concert', 'Interview'],
    activeCollection: 'Émission du soir',
    currentCollection: '',
    batchTotal: 0,
    batchDone: 0,
    failures: [],
    quotaBytes: 5 * 1024 ** 3,
    usedBytes: 1.2 * 1024 ** 3,
    reservedBytes: 0,
    items: [
      {
        id: 'b1',
        collectionName: 'Émission du soir',
        sizeBytes: 412 * 1024 ** 2,
        mediaCount: 18,
        createdAt: new Date(Date.now() - 2 * 3_600_000).toISOString(),
        instanceLabel: 'PC-STUDIO',
        skippedCount: 0,
        unknownSourceTypes: ['streamfx-blur'],
      },
      {
        id: 'b0',
        collectionName: 'Émission du soir',
        sizeBytes: 398 * 1024 ** 2,
        mediaCount: 17,
        createdAt: new Date(Date.now() - 4 * 86_400_000).toISOString(),
        instanceLabel: 'PC-STUDIO',
        skippedCount: 0,
        unknownSourceTypes: [],
      },
      {
        id: 'b2',
        collectionName: 'Concert',
        sizeBytes: 820 * 1024 ** 2,
        mediaCount: 41,
        createdAt: new Date(Date.now() - 9 * 86_400_000).toISOString(),
        instanceLabel: 'PC-REGIE',
        skippedCount: 2,
        unknownSourceTypes: [],
      },
    ],
  },
};
