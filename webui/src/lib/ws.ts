import { useCallback, useEffect, useRef, useState } from 'react';

import { MOCK_STATE, mockEnabled } from './mock';

// ---------------------------------------------------------------------------
// Protocol — mirrors WebUiServer.cpp / SemirprodDock::broadcastToWeb().
// The plugin pushes a full `state` snapshot on every change (no diffs); the UI
// sends one of six command messages. Port + bearer handshake are fixed by the
// C++ side and must not drift.
// ---------------------------------------------------------------------------

// Fallback only. The page is served by the plugin on this port, but the live
// connection derives the port from window.location.port (see connect()), so the
// UI keeps working even if the plugin's port ever changes.
export const WS_PORT = 47615;

export interface StreamItem {
  id: string;
  name: string;
  /** Flux du poste : celui que suivent les sources restées en automatique. */
  active: boolean;
  /** Dans combien de scènes ce flux est montré. */
  scenes?: number;
  /** Une source punaisée sur ce flux décode des images en ce moment. */
  receiving?: boolean;
}

export interface SessionRecord {
  endedAt: string;
  durationSec: number;
  avgKbps: number;
  peakKbps: number;
  droppedFrames: number;
  totalFrames: number;
  droppedPct: number;
  incidents: number;
}

/** Une sauvegarde deja stockee sur le compte. */
export interface BackupItem {
  id: string;
  collectionName: string;
  sizeBytes: number;
  mediaCount: number;
  createdAt: string;
  instanceLabel: string;
  /** Fichiers ecartes du paquet, tous motifs confondus. */
  skippedCount: number;
  /**
   * Types de source que le poste d'origine ne savait pas rendre, donc fournis
   * par un plugin tiers. Ils manqueront a la restauration alors que les medias
   * seront la : c'est la seule chose qui l'explique.
   */
  unknownSourceTypes: string[];
}

/**
 * Import d'un paquet DANS OBS : medias sur le disque, collection dans la liste.
 *
 * `downloading` et `extracting` sont distingues parce que le premier depend du
 * reseau et le second du disque : quand ca traine, l'operateur doit savoir
 * lequel attendre, sinon il relance.
 *
 * Aucun chemin de fichier ici, volontairement : il n'aide a rien decider.
 */
export interface BackupImport {
  phase: 'idle' | 'downloading' | 'extracting' | 'done' | 'failed';
  id: string;
  collectionName: string;
  progress: number;
  error: string;
  /** Types de source absents de CE poste, releves a l'import. */
  missingSourceTypes: string[];
}

/**
 * Etat des sauvegardes de collections.
 *
 * `quotaBytes` / `usedBytes` valent `-1` tant que le serveur n'a pas repondu :
 * distinct de zero, qui serait un quota nul. L'ecran n'affiche la barre
 * d'occupation qu'a partir d'une valeur connue, sinon il annoncerait « 0 octet
 * disponible » a quelqu'un qui a tout son espace.
 */
export interface BackupFailure {
  collectionName: string;
  error: string;
}

export interface BackupState {
  phase: 'idle' | 'packaging' | 'reserving' | 'uploading' | 'done' | 'failed';
  /** Pourcentage d'envoi de la collection EN COURS. Sens uniquement en `uploading`. */
  progress: number;
  error: string;
  /** Les collections empaquetables, telles qu'OBS les declare. La page ne devine aucun nom. */
  collections: string[];
  /** Celle chargee dans OBS en ce moment. */
  activeCollection: string;
  /** Celle en cours de traitement, vide au repos. */
  currentCollection: string;
  /** Taille de la salve et nombre de collections terminees, reussies ou non. */
  batchTotal: number;
  batchDone: number;
  /** Ce qui a echoue dans la salve. Une collection ratee n'arrete pas les suivantes. */
  failures: BackupFailure[];
  quotaBytes: number;
  /** Total engage : sauvegardes confirmees ET envois retenus. */
  usedBytes: number;
  /** Part retenue par un envoi non confirme. Comptee dans `usedBytes`, dite a part. */
  reservedBytes: number;
  items: BackupItem[];
  /**
   * Import en cours dans OBS. Etat SEPARE de la file d'empaquetage : un geste
   * qui echoue ne doit pas donner l'impression que tout est bloque.
   */
  import?: BackupImport;
}

export interface PluginState {
  /** Langue de l'interface : celle d'OBS (`obs_get_locale()`), « fr » si elle
   *  commence par fr, « en » sinon. Fixe pour la session. Les textes que le
   *  plugin envoie (erreurs, raisons) sont déjà écrits dans cette langue. */
  locale: 'fr' | 'en';
  /** Le dernier appairage a échoué : `accountLabel` porte alors l'erreur. */
  pairingFailed?: boolean;
  connected: boolean;
  /**
   * Vrai quand la page tourne DANS une instance infogeree (variante kiosque du
   * plugin). Le compte y vient de l'appairage de l'instance, pas d'une
   * connexion faite par le client : la carte Compte masque donc
   * « Deconnecter ». Absent des anciens binaires, d'ou l'optionnel.
   */
  kiosk?: boolean;
  wsConnected: boolean;
  /** Espace auquel ce poste appartient (poste v2). Vide avant l'appairage. */
  organizationName?: string;
  authFlowRunning: boolean;
  pluginVersion: string;
  devBuild: boolean;
  /** Code et adresse d'appairage, en TEXTE SIMPLE (le plugin n'envoie plus de
   *  HTML : c'est cette interface qui met en forme). Vides une fois le compte
   *  lie ; `accountLabel` porte alors l'etat du compte, ou l'erreur du dernier
   *  appairage rate. */
  accountLabel: string;
  accountUrl: string;
  /** Mise a jour disponible. `updateVersion` vide = rien a signaler. L'URL de
   *  telechargement reste cote plugin : la page envoie `open_update`. */
  updateVersion: string;
  updateSeverity: string;
  updateNotes: string;
  /** Mode securite : version bloquee par le serveur, pilotage coupe. Absent
   *  d'un plugin anterieur a 0.2.3. */
  pluginBlocked?: boolean;
  /** Pause que le manifeste ne justifie plus : levee a la fin du direct en cours. */
  pluginBlockLiftPending?: boolean;
  pluginBlockReason?: 'security' | 'update_required';
  /** Identite du compte lie, renseignee APRES l'appairage. */
  accountName: string;
  accountEmail: string;
  /** Chemin LOCAL de l'avatar (`/avatar?v=<empreinte>`), servi par le pont —
   *  jamais l'URL d'origine : la CSP interdit les images distantes. Vide si le
   *  compte n'a pas d'avatar, ou s'il est heberge hors allowlist. */
  accountAvatar: string;
  streams: StreamItem[];
  sceneLive: string;
  sceneBug: string;
  autoSwitch: boolean;
  previewEnabled: boolean;
  streaming: boolean;
  recording: boolean;
  streamingStartedAt: string;
  statsActive: boolean;
  statsKbps: number;
  statsDroppedFrames: number;
  statsTotalFrames: number;
  statsCongestion: number;
  instanceLabel: string;
  healthAlert: boolean;
  healthReason: string;
  /** Une sortie d'apercu envoie le programme a la regie (ou une capture vient
   *  de partir). Absent des binaires anterieurs : l'ecran doit le tolerer. */
  remotePreviewActive?: boolean;
  /** Derniere destination de diffusion posee a distance, phrase deja
   *  localisee par le plugin (hote + heure, jamais la cle). Vide = rien. */
  remoteDestinationNotice?: string;
  connTestRunning: boolean;
  connTestApiMs: number;
  connTestLiveMs: number;
  connTestError: string;
  sessions: SessionRecord[];
  _scenes: string[];
  studioAccess: boolean;
  studioAccessKnown: boolean;
  studioReason: string;
  sourceReady: boolean;
  sourceWidth: number;
  sourceHeight: number;
  /** La scene de direct contient-elle une source Semirprod ?
   *
   *  Distinct de `sourceReady`, qui vaut false aussi bien quand la source
   *  manque que quand elle est la sans flux : impossible d'en tirer un
   *  diagnostic. C'est pourtant la condition qui bloque la bascule
   *  automatique le plus souvent. */
  sourceInLiveScene: boolean;
  /** Scenes qui tiennent une source Semirprod. Sert a dire ou elle est quand
   *  ce n'est pas dans la scene de direct — « introuvable » serait faux. */
  sourceScenes: string[];
  /** Absent des binaires anterieurs a cette fonction : l'ecran doit le tolerer. */
  backups?: BackupState;
  /** Droits par fonction resolus par le serveur (`/api/obs/plugin/features`).
   *  Sert a GRISER avec un motif, jamais a autoriser : le serveur refuse de
   *  lui-meme. Absent tant que le plugin n'a pas eu de reponse. */
  features?: PluginFeatureMatrix;
}

export type PluginFeatureReason =
  | 'ok'
  | 'no-subscription'
  | 'payment-failing'
  | 'no-seat'
  | 'not-in-plan'
  | 'update-required'
  // Miroir de `@semirprod/types` : places gratuites de l'essai du lancement
  // prises. Le titre et la phrase viennent du serveur, jamais d'ici.
  | 'free-trial-full';

export type PluginFeatureAction = 'subscribe' | 'upgrade' | 'regularize' | 'update-plugin' | null;

export interface PluginFeatureVerdict {
  feature: string;
  label: string;
  allowed: boolean;
  reason: PluginFeatureReason;
  minPluginVersion: string;
  /** Titre, phrase et bouton du refus, ecrits par le serveur. `null` si ouverte. */
  title: string | null;
  message: string | null;
  action: PluginFeatureAction;
  actionLabel: string | null;
  /** Chemin relatif ouvert par le plugin sous son propre hote. Jamais lu par la page. */
  actionPath: string | null;
  requiredTier: string | null;
}

export interface PluginFeatureMatrix {
  minSupportedVersion: string;
  installedVersion: string | null;
  updateRequired: boolean;
  features: PluginFeatureVerdict[];
  /** Impaye en cours dont le delai court : tout marche, on previent. */
  paymentGrace?: PluginPaymentGraceNotice | null;
}

export interface PluginPaymentGraceNotice {
  /** Fin de la tolerance, ISO. */
  until: string;
  title: string;
  message: string;
  action: 'regularize';
  actionLabel: string;
  actionPath: string | null;
}

export type Command =
  | { type: 'get_state' }
  /** Ferme la fenetre Studio. Instance cloud seulement : sa fenetre n'a pas de
   *  barre de titre, donc pas de croix. */
  | { type: 'close_studio' }
  /** Rouvre une fonction fermee : la page donne un NOM, le plugin choisit la page. */
  | { type: 'open_feature_action'; feature: string }
  | { type: 'connect' }
  | { type: 'disconnect' }
  | { type: 'refresh' }
  | { type: 'select_stream'; id: string }
  /** Pose une source punaisée sur ce flux dans la scène ouverte dans OBS. Un
   *  IDENTIFIANT, jamais un nom de source ni de scène : le plugin refuse tout
   *  flux absent de la liste du compte et choisit lui-même la scène. */
  | { type: 'add_stream_source'; id: string }
  | { type: 'set_config'; live: string; bug: string; autoSwitch: boolean }
  /** Pose une source Semirprod dans la scene de direct. Sans parametre : la
   *  scene visee est celle deja configuree cote plugin, la page demande la
   *  reparation sans nommer ce qu'elle contient. */
  | { type: 'place_source_in_live_scene' }
  | { type: 'set_preview_enabled'; on: boolean }
  /** Ferme le bandeau « destination changee a distance ». */
  | { type: 'dismiss_remote_notice' }
  | { type: 'set_instance_label'; label: string }
  | { type: 'run_conn_test' }
  /** Ouvrent une page dans le navigateur du systeme. Sans URL : le plugin
   *  decide de la destination, la page ne fait que demander. */
  | { type: 'open_subscribe' }
  | { type: 'open_update' }
  /** Empaquette ces collections, l'une apres l'autre, et les depose.
   *
   *  Des NOMS pris dans `backups.collections`, jamais un chemin : le plugin
   *  refuse tout nom qu'OBS ne declare pas. Tableau absent ou vide = la
   *  collection active. */
  | { type: 'backup_collection'; collections?: string[] }
  | { type: 'refresh_backups' }
  /** Importe un paquet dans OBS. Un identifiant, jamais un chemin : les
   *  dossiers d'ecriture sont choisis par le plugin. */
  | { type: 'import_backup'; id: string }
  /** Ouvre dans OBS la collection que le dernier import a installee. Sans
   *  nom : le plugin ouvre celle qu'il a lui-meme ecrite. */
  | { type: 'open_imported_collection' };

export type LinkStatus = 'idle' | 'connecting' | 'online' | 'offline';

// Token resolution matches the old mirror: URL hash > localStorage.
const STORAGE_KEY = 'semirprod_token';

export function resolveToken(): string | null {
  const fromHash = window.location.hash.replace(/^#/, '').trim();
  if (fromHash) return fromHash.startsWith('bearer-') ? fromHash.slice(7) : fromHash;
  const stored = localStorage.getItem(STORAGE_KEY);
  return stored || null;
}

export function useWsBridge() {
  const [status, setStatus] = useState<LinkStatus>(
    mockEnabled() ? 'online' : 'idle'
  );
  const [state, setState] = useState<PluginState | null>(
    mockEnabled() ? MOCK_STATE : null
  );

  const wsRef = useRef<WebSocket | null>(null);
  const tokenRef = useRef<string | null>(null);
  const reconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const delayRef = useRef(1000);

  const send = useCallback((cmd: Command) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(cmd));
  }, []);

  const connect = useCallback((token: string) => {
    if (mockEnabled()) return;
    tokenRef.current = token;
    localStorage.setItem(STORAGE_KEY, token);

    if (wsRef.current) {
      try {
        wsRef.current.close();
      } catch {
        /* ignore */
      }
      wsRef.current = null;
    }

    setStatus('connecting');
    const host = window.location.hostname || '127.0.0.1';
    const port = window.location.port || String(WS_PORT);
    const url = `ws://${host}:${port}/`;
    let ws: WebSocket;
    try {
      ws = new WebSocket(url, `bearer-${token}`);
    } catch {
      scheduleReconnect();
      return;
    }
    wsRef.current = ws;

    ws.onopen = () => {
      delayRef.current = 1000;
      setStatus('online');
      send({ type: 'get_state' });
    };
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === 'state') setState(msg.data as PluginState);
      } catch {
        /* ignore malformed frames */
      }
    };
    ws.onclose = () => {
      setStatus('offline');
      scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose handles reconnect */
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [send]);

  const scheduleReconnect = useCallback(() => {
    if (reconnectRef.current) return;
    reconnectRef.current = setTimeout(() => {
      reconnectRef.current = null;
      const token = tokenRef.current || resolveToken();
      if (token) connect(token);
    }, delayRef.current);
    delayRef.current = Math.min(delayRef.current * 2, 15000);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connect]);

  useEffect(() => {
    return () => {
      if (reconnectRef.current) clearTimeout(reconnectRef.current);
      if (wsRef.current) {
        try {
          wsRef.current.close();
        } catch {
          /* ignore */
        }
      }
    };
  }, []);

  return { status, state, connect, send };
}
