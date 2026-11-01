import { Button } from '@/components/ui/button';
import { Empty, Row, Section } from '@/components/ui/section';
import { useT, type TFunction } from '@/i18n';
import type { Command, PluginState, StreamItem } from '@/lib/ws';

interface Props {
  state: PluginState;
  send: (cmd: Command) => void;
}

// Ce que la ligne dit sous le nom du flux, et rien de plus : où il est montré,
// et s'il arrive. Pas de phrase d'explication par ligne — la description de la
// section la porte une fois pour toutes.
function usage(stream: StreamItem, t: TFunction): string {
  const parts: string[] = [];
  if (stream.active) parts.push(t('streams.usage.active'));

  const scenes = stream.scenes ?? 0;
  if (scenes === 0) parts.push(t('streams.usage.unused'));
  else parts.push(t('streams.usage.scenes', { count: scenes }));

  // « Reçoit des images » est une connaissance LOCALE : ce poste décode ce
  // flux. On n'écrit jamais « hors ligne » à la place, ce serait affirmer une
  // panne alors qu'un flux qui n'est affiché nulle part n'a simplement rien à
  // décoder ici.
  if (stream.receiving) parts.push(t('streams.usage.receiving'));

  return parts.join(' · ');
}

// Les flux de l'espace, et le geste qui les met dans une scène.
//
// C'est le chemin de découverte du multicam : sans cette liste, poser une
// deuxième caméra demandait d'ajouter une source « Flux Semirprod » à la main,
// puis d'ouvrir ses propriétés pour la punaiser. Le bouton fait les deux, et
// RÉUTILISE la source déjà punaisée sur ce flux si elle existe — deux sources
// sur la même caméra, ce sont deux décodeurs pour une seule image.
export function StreamsPanel({ state, send }: Props) {
  const t = useT();
  const streams = state.streams || [];

  return (
    <Section
      title={t('streams.title')}
      description={t('streams.description')}
    >
      {streams.length === 0 ? (
        <Empty>{t('streams.empty')}</Empty>
      ) : (
        streams.map((stream) => (
          <Row
            key={stream.id}
            title={stream.name || t('common.unnamedStream')}
            hint={usage(stream, t)}
            control={
              <Button
                size="sm"
                variant="outline"
                onClick={() => send({ type: 'add_stream_source', id: stream.id })}
              >
                {t('streams.add')}
              </Button>
            }
          />
        ))
      )}
    </Section>
  );
}
