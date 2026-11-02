import * as React from 'react';
import { useEffect, useMemo, useState } from 'react';

import { Button } from '@/components/ui/button';
import { Modal } from '@/components/ui/dialog';
import { Empty, NavRow, Row, Section } from '@/components/ui/section';
import { Switch } from '@/components/ui/switch';
import { intlTag, useLocale, useT, type Locale, type TFunction } from '@/i18n';
import { cn } from '@/lib/utils';
import type { BackupItem, Command, PluginState } from '@/lib/ws';
import { FeatureBlocked } from '@/components/FeatureBlocked';

/**
 * Onglet COLLECTIONS — ce poste d'un côté, Semirprod de l'autre.
 *
 * ─── Pourquoi une catégorie à part, avant les réglages ─────────────────────
 *
 * Sauvegarder n'est pas un réglage. Un réglage se pose une fois ; ici
 * l'opérateur vient FAIRE quelque chose, et plus souvent que pour renommer son
 * poste.
 *
 * ─── Ce qui est affiché, et ce qui a été RETIRÉ ────────────────────────────
 *
 * Règle de cet écran : n'afficher que ce qui aide à DÉCIDER. Chaque ligne
 * répond à « est-ce que je clique ? ».
 *
 * Retiré pour cette raison, et à ne pas remettre :
 *   - le chemin des fichiers écrits. Personne n'y va, et il pousse la ligne sur
 *     deux hauteurs ;
 *   - la version d'OBS du poste d'origine, l'empreinte, le poids des médias
 *     séparé du poids du paquet ;
 *   - la liste des identifiants de types de source. Le NOMBRE décide (« il me
 *     manquera des sources »), la liste ne fait que remplir.
 *
 * Ce qui reste : le nom, quand, combien de place, et ce qui manquera.
 *
 * ─── Une page principale, deux fenêtres ────────────────────────────────────
 *
 * La page principale porte les deux gestes courants : sauvegarder la
 * collection OUVERTE (un clic) et voir ce qui est sur Semirprod. Le reste
 * s'ouvre dans une fenêtre PAR-DESSUS la page (`Modal`). Une première version
 * remplaçait l'onglet par une sous-page : « ça change juste la page », retour
 * de l'opérateur — il voulait une fenêtre qui s'ouvre, la page restant derrière :
 *   - « Choisir les collections » : les interrupteurs, pour un envoi groupé ;
 *   - une sauvegarde Semirprod : son détail et le bouton Importer.
 * Les états en cours (envoi, import, échecs) restent AUSSI sur la page
 * principale : on ne doit pas ouvrir un écran pour savoir que quelque chose
 * tourne.
 *
 * ─── Ce que cet écran ne bloque jamais ─────────────────────────────────────
 *
 * La file d'empaquetage (`backups.phase`) et l'import (`backups.import.phase`)
 * sont indépendants. Un envoi qui échoue laisse l'import disponible, et
 * l'inverse — le défaut du 20/08/2026 était un écran entièrement figé dès qu'un
 * seul geste attendait le serveur.
 */

function formatBytes(bytes: number, t: TFunction): string {
  const units = [t('unit.byte'), t('unit.kilobyte'), t('unit.megabyte'), t('unit.gigabyte')];
  if (bytes <= 0) return `0 ${units[0]}`;
  const power = Math.min(units.length - 1, Math.floor(Math.log(bytes) / Math.log(1024)));
  const value = bytes / 1024 ** power;
  return `${value >= 10 || power === 0 ? Math.round(value) : value.toFixed(1)} ${units[power]}`;
}

/** Date courte : le jour et l'heure suffisent à situer une sauvegarde. */
function formatDate(iso: string, locale: Locale, t: TFunction): string {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return t('common.unknown');
  return date.toLocaleString(intlTag(locale), {
    day: '2-digit',
    month: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

/** Ce que la file dit à l'opérateur. Une phrase, jamais deux. */
function phaseLabel(
  phase: string,
  progress: number,
  collection: string,
  done: number,
  total: number,
  t: TFunction,
): string | null {
  const step =
    total > 1 ? t('collections.phase.step', { current: Math.min(done + 1, total), total }) : '';
  const name = collection ? ` ${t('fmt.quotedName', { name: collection })}` : '';
  switch (phase) {
    case 'packaging':
      return t('collections.phase.packaging', { name, step });
    case 'reserving':
      return t('collections.phase.reserving', { step });
    case 'uploading':
      return t('collections.phase.uploading', { name, progress, step });
    default:
      return null;
  }
}

/**
 * Une ligne par COLLECTION : chaque sauvegarde en est une version, la plus
 * récente est celle qu'on importe. Même regroupement que le site (nom, casse et
 * espaces ignorés) ; l'historique complet se consulte sur le site.
 */
function parCollection(items: BackupItem[]): { latest: BackupItem; count: number }[] {
  const groupes = new Map<string, BackupItem[]>();
  for (const item of items) {
    const cle = item.collectionName.trim().toLocaleLowerCase('fr');
    groupes.set(cle, [...(groupes.get(cle) ?? []), item]);
  }
  return [...groupes.values()]
    .map((versions) => {
      const tri = [...versions].sort(
        (a, b) => new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime(),
      );
      return { latest: tri[0]!, count: tri.length };
    })
    .sort((a, b) => new Date(b.latest.createdAt).getTime() - new Date(a.latest.createdAt).getTime());
}

/** La sauvegarde la plus récente d'une collection, s'il y en a une. */
function derniere(items: BackupItem[], collectionName: string): BackupItem | null {
  const pour = items.filter((item) => item.collectionName === collectionName);
  if (pour.length === 0) return null;
  return pour.reduce((recent, item) =>
    new Date(item.createdAt) > new Date(recent.createdAt) ? item : recent,
  );
}

type View = { kind: 'main' } | { kind: 'select' } | { kind: 'backup'; key: string };

const cleDe = (name: string) => name.trim().toLocaleLowerCase('fr');

export function CollectionsPanel({
  state,
  send,
}: {
  state: PluginState;
  send: (cmd: Command) => void;
}) {
  const t = useT();
  const locale = useLocale();
  const backups = state.backups;
  const [selection, setSelection] = useState<string[] | null>(null);
  const [view, setView] = useState<View>({ kind: 'main' });

  // Demandée UNE fois, au montage : le plugin pousse ensuite chaque changement.
  useEffect(() => {
    send({ type: 'refresh_backups' });
  }, [send]);

  const collections = backups?.collections ?? [];
  const active = backups?.activeCollection ?? '';
  const items = backups?.items ?? [];

  // Sélection par défaut : la collection OUVERTE dans OBS. `null` veut dire
  // « l'opérateur n'a rien touché », pour que l'arrivée de la liste ne remplace
  // pas un choix déjà fait.
  const chosen = useMemo(() => {
    if (selection) return selection.filter((name) => collections.includes(name));
    return active && collections.includes(active) ? [active] : [];
  }, [selection, collections, active]);

  if (!backups) {
    return (
      <Section title={t('collections.title')}>
        <Empty>{t('collections.unsupported')}</Empty>
      </Section>
    );
  }

  // Motif resolu par le serveur, avec sa phrase et son bouton. Absent = pas
  // encore de reponse : on ne ferme rien sur une supposition, le serveur
  // refusera de lui-meme si besoin.
  const backupFeature = state.features?.features.find((entry) => entry.feature === 'scene-backup');
  if (backupFeature && !backupFeature.allowed) {
    return (
      <Section title={t('collections.title')}>
        <FeatureBlocked verdict={backupFeature} send={send} />
      </Section>
    );
  }

  const busy =
    backups.phase === 'packaging' ||
    backups.phase === 'reserving' ||
    backups.phase === 'uploading';
  const blocked = state.streaming || state.recording;
  const imp = backups.import;
  const importing = imp?.phase === 'downloading' || imp?.phase === 'extracting';
  const quotaKnown = backups.quotaBytes >= 0 && backups.usedBytes >= 0;
  const ratio =
    quotaKnown && backups.quotaBytes > 0
      ? Math.min(1, backups.usedBytes / backups.quotaBytes)
      : 0;
  const almostFull = quotaKnown && ratio >= 0.9;
  const tout = collections.length > 0 && chosen.length === collections.length;
  const enCours = phaseLabel(
    backups.phase,
    backups.progress,
    backups.currentCollection,
    backups.batchDone,
    backups.batchTotal,
    t,
  );
  const groupes = parCollection(items);
  const retour = () => setView({ kind: 'main' });

  // État de la file d'envoi : sur la page principale ET dans la fenêtre de
  // sélection, pour ne jamais cacher un envoi qui tourne.
  const etatEnvoi = (
    <>
      {blocked && (
        <Row
          title={t('collections.pausedTitle')}
          hint={t('collections.pausedBackupHint')}
        />
      )}
      {enCours && <Row title={enCours} />}
      {/* Une collection ratée n'arrête pas les autres : son motif s'affiche
          seul, sans transformer la salve en échec global. */}
      {!busy &&
        backups.failures.map((failure) => (
          <Row
            key={failure.collectionName}
            tone="danger"
            title={failure.collectionName || t('collections.unknownCollection')}
            hint={failure.error}
          />
        ))}
    </>
  );

  // Les deux phases sont nommées : quand ça traine, l'opérateur doit savoir
  // s'il attend le réseau ou son disque, sinon il relance.
  const etatImport = (
    <>
      {imp?.phase === 'downloading' && (
        <Row
          title={t('collections.import.downloading', {
            name: imp.collectionName,
            progress: imp.progress,
          })}
        />
      )}
      {imp?.phase === 'extracting' && (
        <Row title={t('collections.import.extracting', { name: imp.collectionName })} />
      )}
      {/* L'import n'ouvre pas la collection : c'est l'opérateur qui décide
          de la charger, hors diffusion et hors enregistrement. */}
      {imp?.phase === 'done' && (
        <Row
          title={t('collections.import.done', { name: imp.collectionName })}
          hint={
            imp.missingSourceTypes.length > 0
              ? t('collections.import.missingSources', {
                  count: imp.missingSourceTypes.length,
                })
              : undefined
          }
          control={
            <Button
              size="sm"
              variant="outline"
              disabled={blocked || active === imp.collectionName}
              onClick={() => send({ type: 'open_imported_collection' })}
            >
              {t('collections.import.open')}
            </Button>
          }
        />
      )}
      {imp?.phase === 'failed' && <Row tone="danger" title={t('collections.import.failed')} hint={imp.error} />}
    </>
  );

  // ─── FENÊTRE : choisir les collections à envoyer ────────────────────────
  const fenetreSelection = (
    <Modal
      open={view.kind === 'select'}
      onClose={retour}
      title={t('collections.select.title')}
      description={t('collections.select.description')}
      footer={
        <>
          <Button size="sm" variant="outline" onClick={retour}>
            {t('common.close')}
          </Button>
          <Button
            size="sm"
            disabled={busy || blocked || chosen.length === 0}
            onClick={() => send({ type: 'backup_collection', collections: chosen })}
          >
            {busy
              ? t('collections.select.backingUp')
              : chosen.length > 1
                ? t('collections.select.backupMany', { count: chosen.length })
                : t('collections.select.backupOne')}
          </Button>
        </>
      }
    >
      {etatEnvoi}
      {collections.length === 0 ? (
        <Empty>{t('collections.none')}</Empty>
      ) : (
        <>
          {/* Le « tout » ne se justifie qu'à partir de trois collections :
              en dessous, deux interrupteurs font le même travail. */}
          {collections.length > 2 && (
            <Row
              title={t('collections.select.all')}
              hint={t('collections.select.allCount', {
                chosen: chosen.length,
                total: collections.length,
              })}
              control={
                <Switch
                  checked={tout}
                  disabled={busy}
                  onCheckedChange={(coche) => setSelection(coche ? [...collections] : [])}
                />
              }
            />
          )}
          {collections.map((name) => {
            const sauvegarde = derniere(items, name);
            return (
              <Row
                key={name}
                title={name}
                hint={
                  sauvegarde
                    ? t('collections.select.backedUpOn', {
                        date: formatDate(sauvegarde.createdAt, locale, t),
                      })
                    : t('collections.select.neverBackedUp')
                }
                control={
                  <Switch
                    checked={chosen.includes(name)}
                    disabled={busy}
                    onCheckedChange={(coche) =>
                      setSelection(
                        coche ? [...chosen, name] : chosen.filter((autre) => autre !== name),
                      )
                    }
                  />
                }
              />
            );
          })}
        </>
      )}
    </Modal>
  );

  // ─── FENÊTRE : une sauvegarde sur Semirprod ─────────────────────────────
  // Supprimée depuis le site pendant que la fenêtre était ouverte : elle se
  // ferme d'elle-même (plus de groupe = `open` faux).
  const groupe =
    view.kind === 'backup'
      ? groupes.find((g) => cleDe(g.latest.collectionName) === view.key)
      : undefined;
  let fenetreSauvegarde: React.ReactNode = null;
  if (groupe) {
    const { latest: item, count } = groupe;
    const tiers = item.unknownSourceTypes.length;
    const celleCi = importing && imp?.id === item.id;
    fenetreSauvegarde = (
      <Modal
        open
        onClose={retour}
        title={item.collectionName}
        description={t('collections.detail.description')}
        footer={
          <>
            <Button size="sm" variant="outline" onClick={retour}>
              {t('common.close')}
            </Button>
            <Button
              size="sm"
              disabled={importing || blocked}
              onClick={() => send({ type: 'import_backup', id: item.id })}
            >
              {celleCi ? t('collections.import.running') : t('collections.detail.import')}
            </Button>
          </>
        }
      >
        {blocked && (
          <Row
            title={t('collections.pausedTitle')}
            hint={t('collections.pausedImportHint')}
          />
        )}
        {imp?.id === item.id && etatImport}
        <Row
          title={t('collections.detail.lastBackup')}
          control={<Value>{formatDate(item.createdAt, locale, t)}</Value>}
        />
        <Row
          title={t('collections.detail.size')}
          control={<Value>{formatBytes(item.sizeBytes, t)}</Value>}
        />
        <Row title={t('collections.detail.media')} control={<Value>{item.mediaCount}</Value>} />
        {item.instanceLabel && (
          <Row title={t('collections.detail.sentFrom')} control={<Value>{item.instanceLabel}</Value>} />
        )}
        {count > 1 && (
          <Row
            title={t('collections.detail.versions')}
            hint={t('collections.detail.versionsHint')}
            control={<Value>{count}</Value>}
          />
        )}
        {tiers > 0 && (
          <Row
            title={t('collections.detail.thirdParty', { count: tiers })}
            hint={t('collections.detail.thirdPartyHint')}
          />
        )}
      </Modal>
    );
  }

  // ─── PAGE PRINCIPALE ────────────────────────────────────────────────────
  const ouverte = active && collections.includes(active) ? active : '';
  const sauvegardeOuverte = ouverte ? derniere(items, ouverte) : null;

  return (
    <div className="flex flex-col gap-6">
      <Section title={t('collections.local.title')}>
        {etatEnvoi}
        {collections.length === 0 ? (
          <Empty>{t('collections.none')}</Empty>
        ) : (
          <>
            {ouverte && (
              <Row
                title={ouverte}
                hint={
                  sauvegardeOuverte
                    ? t('collections.local.openBackedUp', {
                        date: formatDate(sauvegardeOuverte.createdAt, locale, t),
                      })
                    : t('collections.local.openNeverBackedUp')
                }
                control={
                  <Button
                    size="sm"
                    disabled={busy || blocked}
                    onClick={() => send({ type: 'backup_collection', collections: [ouverte] })}
                  >
                    {busy ? t('collections.local.backingUp') : t('collections.local.backup')}
                  </Button>
                }
              />
            )}
            <NavRow
              title={t('collections.local.chooseTitle')}
              hint={t('collections.local.chooseHint')}
              value={collections.length}
              onClick={() => setView({ kind: 'select' })}
            />
          </>
        )}
      </Section>

      <Section title={t('collections.remote.title')}>
        {quotaKnown && (
          <div className="px-4 py-3.5">
            <div className="flex items-baseline justify-between gap-3 text-[12px]">
              <span className="text-muted-foreground">
                {t('collections.remote.usage', {
                  used: formatBytes(backups.usedBytes, t),
                  quota: formatBytes(backups.quotaBytes, t),
                })}
              </span>
              {almostFull && <span className="text-[hsl(var(--warning))]">{t('collections.remote.almostFull')}</span>}
            </div>
            {/* `transform` seul : animer une largeur recalcule la mise en page à
                chaque image. */}
            <div className="mt-1.5 h-1.5 w-full overflow-hidden rounded-full bg-muted">
              <div
                className={cn(
                  'h-full w-full origin-left rounded-full transition-transform duration-200 motion-reduce:transition-none',
                  almostFull ? 'bg-[hsl(var(--warning))]' : 'bg-foreground/40',
                )}
                style={{ transform: `scaleX(${ratio})` }}
              />
            </div>
          </div>
        )}

        {etatImport}

        {groupes.length === 0 ? (
          <Empty>{t(quotaKnown ? 'collections.noBackups' : 'collections.backupsUnavailable')}</Empty>
        ) : (
          groupes.map(({ latest: item, count }) => (
            <NavRow
              key={item.id}
              title={item.collectionName}
              hint={
                <>
                  {formatDate(item.createdAt, locale, t)} · {formatBytes(item.sizeBytes, t)}
                  {count > 1 ? ` · ${t('collections.remote.versions', { count })}` : ''}
                </>
              }
              value={importing && imp?.id === item.id ? t('collections.import.running') : undefined}
              onClick={() => setView({ kind: 'backup', key: cleDe(item.collectionName) })}
            />
          ))
        )}
      </Section>

      {fenetreSelection}
      {fenetreSauvegarde}
    </div>
  );
}

/** Valeur à droite d'une ligne de détail : à lire, pas un contrôle. */
function Value({ children }: { children: React.ReactNode }) {
  return <span className="text-[13px] text-muted-foreground">{children}</span>;
}
